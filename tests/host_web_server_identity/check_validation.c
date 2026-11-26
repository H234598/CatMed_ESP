#include "katmed_web_server_validation.h"
#include "katmed_web_server_journal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

typedef struct { unsigned char bytes[sizeof(KatmedServerRecord)]; size_t len; } Blob;
static Blob blobs[4], staged;
static int staged_index = -1;
static bool mock_set_failure, mock_commit_failure, mock_read_failure;
static int mock_read_index = -1;
static int which(const char *key)
{
    if (!strcmp(key, "server0")) return 0;
    if (!strcmp(key, "server1")) return 1;
    if (!strcmp(key, "server_active")) return 2;
    if (!strcmp(key, "server_anchor")) return 3;
    return -1;
}
bool katmed_source_writer_enter(void) { return true; }
void katmed_source_writer_leave(void) { }
esp_err_t nvs_get_blob(nvs_handle_t nvs, const char *key, void *out, size_t *len)
{
    (void)nvs; int i = which(key);
    if (mock_read_failure || i == mock_read_index) return -1;
    if (i < 0 || !blobs[i].len) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < blobs[i].len) return -1;
    memcpy(out, blobs[i].bytes, blobs[i].len); *len = blobs[i].len; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t nvs, const char *key, const void *in, size_t len)
{
    (void)nvs; staged_index = which(key);
    if (mock_set_failure) return -1;
    if (staged_index < 0 || len > sizeof(staged.bytes)) return -1;
    memcpy(staged.bytes, in, len); staged.len = len; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t nvs)
{
    (void)nvs; if (staged_index < 0 || mock_commit_failure) return -1;
    blobs[staged_index] = staged; staged_index = -1; return ESP_OK;
}

typedef struct { unsigned char bytes[65536]; size_t length; } Queue;
typedef struct { Queue *in, *out; } Endpoint;
static int send_bytes(void *ctx, const unsigned char *data, size_t len)
{
    Endpoint *ep = ctx;
    if (len > sizeof(ep->out->bytes) - ep->out->length) return MBEDTLS_ERR_SSL_WANT_WRITE;
    memcpy(ep->out->bytes + ep->out->length, data, len); ep->out->length += len;
    return (int)len;
}
static int receive_bytes(void *ctx, unsigned char *data, size_t len)
{
    Endpoint *ep = ctx;
    if (!ep->in->length) return MBEDTLS_ERR_SSL_WANT_READ;
    if (len > ep->in->length) len = ep->in->length;
    memcpy(data, ep->in->bytes, len); ep->in->length -= len;
    memmove(ep->in->bytes, ep->in->bytes + len, ep->in->length);
    return (int)len;
}
static bool handshake(const unsigned char *key, const unsigned char *chain, size_t chain_len,
                      const unsigned char *anchor, size_t anchor_len, const char *hostname)
{
    mbedtls_x509_crt cert, trust;
    mbedtls_pk_context pk;
    mbedtls_ssl_context server, client;
    mbedtls_ssl_config server_conf, client_conf;
    mbedtls_x509_crt_init(&cert); mbedtls_x509_crt_init(&trust); mbedtls_pk_init(&pk);
    mbedtls_ssl_init(&server); mbedtls_ssl_init(&client);
    mbedtls_ssl_config_init(&server_conf); mbedtls_ssl_config_init(&client_conf);
    psa_key_id_t id = 0;
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attrs, 256);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attrs, MBEDTLS_PK_ALG_ECDSA(PSA_ALG_SHA_256));
    int result = psa_import_key(&attrs, key, 32, &id);
    psa_reset_key_attributes(&attrs);
    if (!result) result = mbedtls_pk_wrap_psa(&pk, id);
    if (!result) result = mbedtls_x509_crt_parse(&cert, chain, chain_len);
    if (!result) result = mbedtls_x509_crt_parse(&trust, anchor, anchor_len);
    if (!result) result = mbedtls_ssl_config_defaults(&server_conf, MBEDTLS_SSL_IS_SERVER,
                       MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (!result) result = mbedtls_ssl_config_defaults(&client_conf, MBEDTLS_SSL_IS_CLIENT,
                       MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&client_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&client_conf, &trust, NULL);
    if (!result) result = mbedtls_ssl_conf_own_cert(&server_conf, &cert, &pk);
    if (!result) result = mbedtls_ssl_setup(&server, &server_conf);
    if (!result) result = mbedtls_ssl_setup(&client, &client_conf);
    if (!result) result = mbedtls_ssl_set_hostname(&client, hostname);
    Queue c2s = {0}, s2c = {0}; Endpoint server_io = {&c2s, &s2c}, client_io = {&s2c, &c2s};
    mbedtls_ssl_set_bio(&server, &server_io, send_bytes, receive_bytes, NULL);
    mbedtls_ssl_set_bio(&client, &client_io, send_bytes, receive_bytes, NULL);
    bool sd = false, cd = false;
    for (unsigned i = 0; !result && i < 1000 && (!sd || !cd); ++i) {
        if (!cd) { int r = mbedtls_ssl_handshake(&client); if (!r) cd = true;
            else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) result = r; }
        if (!result && !sd) { int r = mbedtls_ssl_handshake(&server); if (!r) sd = true;
            else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) result = r; }
    }
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&client);
    bool ok = !result && sd && cd && !mbedtls_ssl_get_verify_result(&client) && peer && peer->next && cert.next &&
        peer->raw.len == cert.raw.len && !memcmp(peer->raw.p, cert.raw.p, cert.raw.len) &&
        peer->next->raw.len == cert.next->raw.len &&
        !memcmp(peer->next->raw.p, cert.next->raw.p, cert.next->raw.len);
    mbedtls_ssl_free(&client); mbedtls_ssl_free(&server);
    mbedtls_ssl_config_free(&client_conf); mbedtls_ssl_config_free(&server_conf);
    mbedtls_pk_free(&pk); if (id) (void)psa_destroy_key(id);
    mbedtls_x509_crt_free(&cert); mbedtls_x509_crt_free(&trust);
    return ok;
}

static size_t read_file(const char *path, unsigned char *out, size_t capacity)
{
    FILE *f = fopen(path, "rb");
    if (!f) exit(2);
    size_t n = fread(out, 1, capacity, f);
    if (ferror(f) || !feof(f)) exit(2);
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 6) return 2;
    unsigned char key[33], chain[6144], anchor[2048], other[2048];
    size_t kl = read_file(argv[1], key, sizeof(key));
    size_t cl = read_file(argv[2], chain, sizeof(chain) - 1);
    size_t al = read_file(argv[3], anchor, sizeof(anchor) - 1);
    size_t ol = read_file(argv[4], other, sizeof(other) - 1);
    chain[cl++] = 0; anchor[al++] = 0; other[ol++] = 0;
    int64_t now = strtoll(argv[5], NULL, 10);
    if (!katmed_web_server_identity_validate(key, kl, (char *)chain, cl,
                                              (char *)anchor, al, "device.example", now)) return 3;
    KatmedServerRecord candidate = {.magic = KATMED_SERVER_RECORD_MAGIC}, stored = {0};
    unsigned slot = 0, active = 0;
    memcpy(candidate.key, key, sizeof(candidate.key));
    memcpy(candidate.chain, chain, cl); candidate.chain_len = (uint16_t)cl;
    strcpy(candidate.dns, "device.example");
    if (!katmed_server_journal_stage(1, &candidate, &slot) ||
        katmed_server_journal_load(1, &stored, &active) != KATMED_SERVER_LEGACY ||
        !katmed_server_journal_commit(1, slot) ||
        katmed_server_journal_load(1, &stored, &active) != KATMED_SERVER_ACTIVE ||
        memcmp(&stored, &candidate, sizeof(stored))) return 11;
    if (!handshake(stored.key, (unsigned char *)stored.chain, stored.chain_len,
                   anchor, al, stored.dns)) return 9;
    if (handshake(key, chain, cl, anchor, al, "other.example")) return 10;
    if (katmed_web_server_identity_validate(key, kl, (char *)chain, cl,
                                             (char *)anchor, al, "other.example", now)) return 4;
    if (katmed_web_server_identity_validate(key, kl, (char *)chain, cl,
                                             (char *)other, ol, "device.example", now)) return 5;
    if (katmed_web_server_identity_validate(key, kl, (char *)chain, cl,
                                             (char *)anchor, al, "device.example", now - 86400 * 30)) return 6;
    if (katmed_web_server_identity_validate(key, kl, (char *)chain, cl,
                                             (char *)anchor, al, "device.example", now + 86400 * 30)) return 7;
    key[31] ^= 1;
    if (katmed_web_server_identity_validate(key, kl, (char *)chain, cl,
                                             (char *)anchor, al, "device.example", now)) return 8;
    puts("real X.509 chain, SAN, trust, time and key validation: PASS");
    return 0;
}
