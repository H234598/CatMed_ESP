#include "katmed_web_identity.h"
#include "katmed_web_import.h"
#include "katmed_web_auth.h"

#include <stdio.h>
#include <string.h>
#include "esp_mac.h"
#include "esp_random.h"
#include "mbedtls/pk.h"
#include "mbedtls/md.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#define IDENTITY_MAGIC 0x4b574931u /* KWI1 */
_Static_assert(sizeof(KatmedWebAuthRecord) == 1112, "Web auth NVS format changed");
_Static_assert(sizeof(KatmedWebIdentity) == 1288, "Web identity NVS format changed");

static void wipe(void *ptr, size_t len)
{
    volatile uint8_t *p = ptr;
    while (len--) *p++ = 0;
}

static bool identity_valid(const KatmedWebIdentity *identity)
{
    if (identity->magic != IDENTITY_MAGIC ||
        identity->key_len == 0 || identity->key_len > KATMED_WEB_KEY_MAX ||
        identity->cert_len == 0 || identity->cert_len > KATMED_WEB_SERVER_CERT_MAX)
        return false;
    mbedtls_pk_context key;
    mbedtls_x509_crt cert;
    mbedtls_pk_init(&key);
    mbedtls_x509_crt_init(&cert);
    mbedtls_svc_key_id_t psa_key = 0;
    int key_result = -1;
    bool imported = false;
    if (psa_crypto_init() == PSA_SUCCESS) {
        if (identity->key_len == 32) {
            psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
            psa_set_key_type(&attributes,
                             PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
            psa_set_key_bits(&attributes, 256);
            psa_set_key_usage_flags(&attributes,
                                    PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
            psa_set_key_algorithm(&attributes,
                                  PSA_ALG_ECDSA(PSA_ALG_SHA_256));
            if (psa_import_key(&attributes, identity->key, identity->key_len,
                               &psa_key) == PSA_SUCCESS) {
                imported = true;
                if (mbedtls_pk_wrap_psa(&key, psa_key) == 0) key_result = 0;
            }
            psa_reset_key_attributes(&attributes);
        } else {
            /* Keep existing NVS identities created by the former DER API. */
            key_result = mbedtls_pk_parse_key(&key, identity->key,
                                              identity->key_len, NULL, 0);
        }
    }
    int cert_result = mbedtls_x509_crt_parse_der(&cert, identity->cert,
                                                 identity->cert_len);
    int pair_result = key_result == 0 && cert_result == 0 &&
        mbedtls_pk_can_do_psa(&cert.pk, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                              PSA_KEY_USAGE_VERIFY_HASH) &&
        mbedtls_pk_get_bitlen(&cert.pk) == 256 ?
        mbedtls_pk_check_pair(&cert.pk, &key) : -1;
    mbedtls_x509_crt_free(&cert);
    mbedtls_pk_free(&key);
    if (imported) (void)psa_destroy_key(psa_key);
    return pair_result == 0;
}

bool katmed_web_import_records_valid(const uint8_t *auth_bytes, size_t auth_len,
                                     const uint8_t *identity_bytes, size_t identity_len)
{
    if ((auth_len && (!auth_bytes || !identity_len)) ||
        (identity_len && !identity_bytes) ||
        (auth_len && auth_len != sizeof(KatmedWebAuthRecord)) ||
        (identity_len && identity_len != sizeof(KatmedWebIdentity))) return false;
    bool ok = true;
    if (auth_len) {
        KatmedWebAuthRecord auth;
        memcpy(&auth, auth_bytes, sizeof(auth));
        ok = auth.magic == 0x4b574131u && auth.version == 1 &&
             auth.cert_len <= KATMED_WEB_CERT_MAX;
        if (ok) {
            for (size_t i = auth.cert_len; i < KATMED_WEB_CERT_MAX; ++i)
                if (auth.client_cert[i] != 0) ok = false;
        }
        if (ok && auth.cert_len == 0) {
            for (size_t i = 0; i < sizeof(auth.client_fingerprint); ++i)
                if (auth.client_fingerprint[i] != 0) ok = false;
        }
        if (ok && auth.cert_len) {
            mbedtls_x509_crt cert;
            mbedtls_x509_crt_init(&cert);
            uint8_t digest[32] = {0};
            const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
            ok = md && mbedtls_x509_crt_parse_der(&cert, auth.client_cert,
                                                  auth.cert_len) == 0 &&
                 mbedtls_md(md, auth.client_cert, auth.cert_len, digest) == 0;
            uint8_t difference = 0;
            if (ok) for (size_t i = 0; i < sizeof(digest); ++i)
                difference |= digest[i] ^ auth.client_fingerprint[i];
            ok = ok && difference == 0;
            wipe(digest, sizeof(digest));
            mbedtls_x509_crt_free(&cert);
        }
        wipe(&auth, sizeof(auth));
    }
    if (ok && identity_len) {
        KatmedWebIdentity identity;
        memcpy(&identity, identity_bytes, sizeof(identity));
        ok = identity.magic == IDENTITY_MAGIC &&
             identity.key_len > 0 && identity.key_len <= KATMED_WEB_KEY_MAX &&
             identity.cert_len > 0 && identity.cert_len <= KATMED_WEB_SERVER_CERT_MAX;
        if (ok) {
            for (size_t i = identity.key_len; i < KATMED_WEB_KEY_MAX; ++i)
                if (identity.key[i] != 0) ok = false;
            for (size_t i = identity.cert_len; i < KATMED_WEB_SERVER_CERT_MAX; ++i)
                if (identity.cert[i] != 0) ok = false;
        }
        if (ok) ok = identity_valid(&identity);
        wipe(&identity, sizeof(identity));
    }
    return ok;
}

static bool identity_create(KatmedWebIdentity *out)
{
    bool ok = false;
    mbedtls_pk_context key;
    mbedtls_x509write_cert cert;
    uint8_t raw_key[32] = {0};
    uint8_t cert_buf[KATMED_WEB_SERVER_CERT_MAX] = {0};
    uint8_t serial[16];
    uint8_t mac[6];
    char name[48];
    char dn[64];
    mbedtls_svc_key_id_t psa_key = 0;
    bool wrapped = false;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&cert);
    if (psa_crypto_init() != PSA_SUCCESS) goto done;
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) goto done;
    snprintf(name, sizeof(name), "katmed-%02x%02x%02x.local", mac[3], mac[4], mac[5]);
    snprintf(dn, sizeof(dn), "CN=%s", name);
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes,
                            PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_status_t key_status = psa_generate_key(&attributes, &psa_key);
    psa_reset_key_attributes(&attributes);
    size_t raw_key_len = 0;
    if (key_status != PSA_SUCCESS ||
        psa_export_key(psa_key, raw_key, sizeof(raw_key), &raw_key_len) != PSA_SUCCESS ||
        raw_key_len != 32 || mbedtls_pk_wrap_psa(&key, psa_key) != 0) goto done;
    wrapped = true;
    mbedtls_x509write_crt_set_version(&cert, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_subject_key(&cert, &key);
    mbedtls_x509write_crt_set_issuer_key(&cert, &key);
    mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
    esp_fill_random(serial, sizeof(serial));
    serial[0] &= 0x7f;
    if (mbedtls_x509write_crt_set_serial_raw(&cert, serial, sizeof(serial)) != 0 ||
        mbedtls_x509write_crt_set_subject_name(&cert, dn) != 0 ||
        mbedtls_x509write_crt_set_issuer_name(&cert, dn) != 0 ||
        mbedtls_x509write_crt_set_validity(&cert, "20200101000000", "20991231235959") != 0 ||
        mbedtls_x509write_crt_set_basic_constraints(&cert, 0, -1) != 0 ||
        mbedtls_x509write_crt_set_key_usage(&cert, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0)
        goto done;
    mbedtls_x509_san_list san = {0};
    san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san.node.san.unstructured_name.p = (unsigned char *)name;
    san.node.san.unstructured_name.len = strlen(name);
    if (mbedtls_x509write_crt_set_subject_alternative_name(&cert, &san) != 0)
        goto done;
    int cert_len = mbedtls_x509write_crt_der(&cert, cert_buf, sizeof(cert_buf));
    if (cert_len <= 0 || cert_len > KATMED_WEB_SERVER_CERT_MAX) goto done;
    memset(out, 0, sizeof(*out));
    out->magic = IDENTITY_MAGIC;
    out->key_len = (uint16_t)raw_key_len;
    out->cert_len = (uint16_t)cert_len;
    memcpy(out->key, raw_key, raw_key_len);
    memcpy(out->cert, cert_buf + sizeof(cert_buf) - cert_len, cert_len);
    ok = identity_valid(out);
done:
    if (!ok) wipe(out, sizeof(*out));
    wipe(raw_key, sizeof(raw_key));
    wipe(cert_buf, sizeof(cert_buf));
    wipe(serial, sizeof(serial));
    mbedtls_x509write_crt_free(&cert);
    mbedtls_pk_free(&key);
    if (wrapped) (void)psa_destroy_key(psa_key);
    else if (psa_key != 0) (void)psa_destroy_key(psa_key);
    return ok;
}

bool katmed_web_identity_load_or_create(nvs_handle_t nvs, KatmedWebIdentity *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(nvs, "identity", out, &len);
    if (err == ESP_OK) return len == sizeof(*out) && identity_valid(out);
    if (err != ESP_ERR_NVS_NOT_FOUND || !identity_create(out)) return false;
    if (nvs_set_blob(nvs, "identity", out, sizeof(*out)) != ESP_OK ||
        nvs_commit(nvs) != ESP_OK) {
        wipe(out, sizeof(*out));
        return false;
    }
    return true;
}
