#include "katmed_web_server_validation.h"

#include <string.h>
#include <strings.h>
#include <time.h>
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

/* Certificate validity uses the caller's trusted clock, not libc's possibly
 * unset wall clock. Signature, constraints and trust flags remain intact. */
static int trusted_time_callback(void *context, mbedtls_x509_crt *crt,
                                 int depth, uint32_t *flags)
{
    (void)context;
    (void)crt;
    (void)depth;
    *flags &= ~(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    return 0;
}

static int64_t time_number(const mbedtls_x509_time *t)
{
    return (((((int64_t)t->year * 100 + t->mon) * 100 + t->day) * 100 + t->hour)
             * 100 + t->min) * 100 + t->sec;
}

static bool current(const mbedtls_x509_crt *crt, int64_t now)
{
    int64_t start = time_number(&crt->valid_from);
    int64_t end = time_number(&crt->valid_to);
    return start <= now && now <= end;
}

static bool exact_dns_san(const mbedtls_x509_crt *crt, const char *dns)
{
    size_t length = strlen(dns);
    for (const mbedtls_x509_sequence *p = &crt->subject_alt_names; p; p = p->next) {
        if (p->buf.tag == (MBEDTLS_ASN1_CONTEXT_SPECIFIC | 2) &&
            p->buf.len == length && !strncasecmp((const char *)p->buf.p, dns, length))
            return true;
    }
    return false;
}

bool katmed_web_server_identity_validate(const uint8_t *key, size_t key_len,
                                         const char *chain, size_t chain_len,
                                         const char *anchor, size_t anchor_len,
                                         const char *dns, int64_t utc_seconds)
{
    if (!key || key_len != 32 || !chain || chain_len < 2 || chain_len > 6144 ||
        chain[chain_len - 1] || strnlen(chain, chain_len) != chain_len - 1 ||
        !anchor || anchor_len < 2 || anchor_len > 2048 || anchor[anchor_len - 1] ||
        strnlen(anchor, anchor_len) != anchor_len - 1 || !dns || !*dns ||
        strnlen(dns, 254) >= 254 || utc_seconds < 1577836800) return false;
    time_t seconds = (time_t)utc_seconds;
    struct tm utc;
    if ((int64_t)seconds != utc_seconds || !gmtime_r(&seconds, &utc) ||
        psa_crypto_init() != PSA_SUCCESS) return false;
    int64_t now = (((((int64_t)(utc.tm_year + 1900) * 100 + utc.tm_mon + 1) * 100 +
                       utc.tm_mday) * 100 + utc.tm_hour) * 100 + utc.tm_min) * 100 + utc.tm_sec;
    mbedtls_x509_crt cert, trust;
    mbedtls_pk_context private_key;
    mbedtls_x509_crt_init(&cert);
    mbedtls_x509_crt_init(&trust);
    mbedtls_pk_init(&private_key);
    mbedtls_svc_key_id_t key_id = 0;
    bool imported = false, ok = false;
    uint32_t flags = 0;
    if (mbedtls_x509_crt_parse(&cert, (const unsigned char *)chain, chain_len) != 0 ||
        mbedtls_x509_crt_parse(&trust, (const unsigned char *)anchor, anchor_len) != 0 ||
        trust.next || !trust.MBEDTLS_PRIVATE(ca_istrue) || !current(&trust, now) ||
        cert.MBEDTLS_PRIVATE(ca_istrue) ||
        !exact_dns_san(&cert, dns) ||
        !mbedtls_pk_can_do_psa(&cert.pk, PSA_ALG_ECDSA(PSA_ALG_SHA_256), PSA_KEY_USAGE_VERIFY_HASH) ||
        mbedtls_pk_get_bitlen(&cert.pk) != 256 ||
        mbedtls_x509_crt_check_key_usage(&cert, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0 ||
        !cert.ext_key_usage.buf.p ||
        mbedtls_x509_crt_check_extended_key_usage(&cert, MBEDTLS_OID_SERVER_AUTH,
            MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)) != 0) goto done;
    for (const mbedtls_x509_crt *p = &cert; p; p = p->next)
        if (!current(p, now)) goto done;
    if (mbedtls_x509_crt_verify(&cert, &trust, NULL, NULL, &flags,
                                trusted_time_callback, NULL) != 0 || flags) goto done;
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attrs, 256);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attrs, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_status_t status = psa_import_key(&attrs, key, key_len, &key_id);
    psa_reset_key_attributes(&attrs);
    if (status != PSA_SUCCESS) goto done;
    imported = true;
    if (mbedtls_pk_wrap_psa(&private_key, key_id) != 0 ||
        mbedtls_pk_check_pair(&cert.pk, &private_key) != 0) goto done;
    ok = true;
done:
    mbedtls_pk_free(&private_key);
    if (imported) (void)psa_destroy_key(key_id);
    mbedtls_x509_crt_free(&cert);
    mbedtls_x509_crt_free(&trust);
    return ok;
}
