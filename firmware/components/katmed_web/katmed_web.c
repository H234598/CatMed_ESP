#include "katmed_web.h"
#include "katmed_source_export.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <strings.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_https_server.h"
#include "esp_key_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "katmed_config_bridge.h"
#include "katmed_web_auth.h"
#include "katmed_web_acme_route.h"
#include "katmed_web_identity.h"
#include "katmed_web_media.h"
#include "katmed_web_policy.h"
#include "mbedtls/md.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include "psa/crypto.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#if CONFIG_IDF_TARGET_ESP32C6
#include "lwip/opt.h"
#include "lwip/priv/sockets_priv.h"
#include "lwip/sys.h"
#endif
#ifdef KATMED_WEB_SECURITY_JS_GZIP
#include "miniz.h"
#include "esp_rom_crc.h"
#endif

extern const unsigned char _binary_index_html_start[];
extern const unsigned char _binary_index_html_end[];
extern const unsigned char _binary_style_css_start[];
extern const unsigned char _binary_style_css_end[];
extern const unsigned char _binary_app_js_start[];
extern const unsigned char _binary_app_js_end[];
extern const unsigned char _binary_i18n_js_start[];
extern const unsigned char _binary_i18n_js_end[];

#define WEB_TAG "katmed_web"
#define BODY_MAX 3072u
#define BRIDGE_TIMEOUT_MS 1500u
#define PRE_FREE_INTERNAL (48u * 1024u)
#define PRE_LARGEST_INTERNAL (16u * 1024u)
#define PRE_FREE_DMA (8u * 1024u)
#define PRE_LARGEST_DMA (2u * 1024u)
#define POST_FREE_INTERNAL (32u * 1024u)
#define POST_FREE_DMA (6u * 1024u)
#define LIVE_FREE_INTERNAL (12u * 1024u)
#define LIVE_FREE_DMA (4u * 1024u)

typedef struct {
    nvs_handle_t nvs;
    KatmedWebIdentity identity;
    psa_key_id_t tls_key_id;
    esp_key_config_t tls_key_config;
    KatmedWebAuth auth;
    httpd_handle_t https;
    httpd_handle_t redirect;
    int certificate_fd;
    bool certificate_verified;
    uint8_t certificate_fingerprint[32];
    uint64_t restart_due_ms;
    uint64_t retry_due_ms;
    bool nvs_open;
    bool started;
    bool restart_pending;
    KatmedWebServices services;
    char request_body[BODY_MAX + 1];
    SemaphoreHandle_t auth_mutex;
} WebContext;

static WebContext web = {.certificate_fd = -1};

#if CONFIG_IDF_TARGET_ESP32C6
/* Diagnostic only: the SDK's debug accessor exposes its actual socket slots.
 * Copy slot state under the same protection used by lwIP's allocator. A later
 * getsockname() may race a close, so port buckets are best-effort, not owners. */
static struct {
    uint64_t next_sample_ms;
    uint64_t next_report_ms;
    unsigned reports;
    unsigned occupied;
    unsigned retained;
    unsigned available;
    bool sampled;
} socket_diag;

static void web_socket_diag_sample(uint64_t now_ms, bool force)
{
    if (!force && now_ms < socket_diag.next_sample_ms) return;
    socket_diag.next_sample_ms = now_ms + (now_ms < 120000u ? 250u : 5000u);

    int fds[CONFIG_LWIP_MAX_SOCKETS];
    unsigned count = 0, retained = 0, available = 0;
    SYS_ARCH_DECL_PROTECT(level);
    SYS_ARCH_PROTECT(level);
    for (unsigned i = 0; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
        int fd = LWIP_SOCKET_OFFSET + (int)i;
        struct lwip_sock *slot = lwip_socket_dbg_get_socket(fd);
        if (slot && slot->conn) fds[count++] = fd;
        else if (slot && slot->fd_used) ++retained;
        else ++available;
    }
    SYS_ARCH_UNPROTECT(level);

    bool changed = !socket_diag.sampled || count != socket_diag.occupied ||
                   retained != socket_diag.retained || available != socket_diag.available;
    socket_diag.occupied = count;
    socket_diag.retained = retained;
    socket_diag.available = available;
    socket_diag.sampled = true;
    if (!force && !changed && now_ms < socket_diag.next_report_ms) return;
    if (socket_diag.reports >= 128u) {
        if (socket_diag.reports == 128u) {
            ++socket_diag.reports;
            ESP_LOGI(WEB_TAG, "socket slot diagnostic report limit reached");
        }
        return;
    }
    socket_diag.next_report_ms = now_ms + 30000u;

    unsigned web80 = 0, web443 = 0, other = 0, unclassified = 0;
    for (unsigned i = 0; i < count; ++i) {
        struct sockaddr_storage local = {0};
        socklen_t len = sizeof(local);
        if (getsockname(fds[i], (struct sockaddr *)&local, &len) != 0) {
            ++unclassified;
            continue;
        }
        uint16_t port = 0;
        if (local.ss_family == AF_INET)
            port = ntohs(((struct sockaddr_in *)&local)->sin_port);
        else if (local.ss_family == AF_INET6)
            port = ntohs(((struct sockaddr_in6 *)&local)->sin6_port);
        else {
            ++unclassified;
            continue;
        }
        if (port == 80u) ++web80;
        else if (port == 443u) ++web443;
        else ++other;
    }
    ++socket_diag.reports;
    ESP_LOGI(WEB_TAG,
             "socket slots sample=%u up_ms=%llu total=%u active=%u retained=%u free=%u local80=%u local443=%u other=%u unknown=%u",
             socket_diag.reports, (unsigned long long)now_ms,
             (unsigned)CONFIG_LWIP_MAX_SOCKETS, count, retained, available,
             web80, web443, other, unclassified);
}
#else
static void web_socket_diag_sample(uint64_t now_ms, bool force)
{
    (void)now_ms;
    (void)force;
}
#endif

static uint64_t monotonic_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void wipe(void *ptr, size_t len)
{
    volatile uint8_t *p = ptr;
    while (len--) *p++ = 0;
}

static bool random_bytes(void *context, uint8_t *out, size_t len)
{
    (void)context;
    esp_fill_random(out, len);
    return true;
}

static bool derive_password(void *context, const char *password,
                            const uint8_t salt[16], uint8_t out[32])
{
    (void)context;
    if (password == NULL || salt == NULL || out == NULL || psa_crypto_init() != PSA_SUCCESS)
        return false;
    psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(
        &operation, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS)
        status = psa_key_derivation_input_integer(
            &operation, PSA_KEY_DERIVATION_INPUT_COST, KATMED_WEB_PBKDF2_ROUNDS);
    if (status == PSA_SUCCESS)
        status = psa_key_derivation_input_bytes(
            &operation, PSA_KEY_DERIVATION_INPUT_SALT, salt, 16);
    if (status == PSA_SUCCESS)
        status = psa_key_derivation_input_bytes(
            &operation, PSA_KEY_DERIVATION_INPUT_PASSWORD,
            (const uint8_t *)password, strlen(password));
    if (status == PSA_SUCCESS)
        status = psa_key_derivation_output_bytes(&operation, out, 32);
    psa_status_t abort_status = psa_key_derivation_abort(&operation);
    return status == PSA_SUCCESS && abort_status == PSA_SUCCESS;
}

static int sha256_bytes(const uint8_t *input, size_t length, uint8_t output[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info == NULL ? -1 : mbedtls_md(info, input, length, output);
}

static bool commit_auth(void *context, const KatmedWebAuthRecord *record)
{
    WebContext *w = context;
    if (!w->nvs_open || !katmed_source_writer_enter()) return false;
    bool ok = nvs_set_blob(w->nvs, "auth", record, sizeof(*record)) == ESP_OK &&
              nvs_commit(w->nvs) == ESP_OK;
    katmed_source_writer_leave();
    return ok;
}

static bool load_auth(WebContext *w)
{
    KatmedWebAuthRecord stored;
    size_t len = sizeof(stored);
    esp_err_t err = nvs_get_blob(w->nvs, "auth", &stored, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    if (err == ESP_OK && len != sizeof(stored)) return false;
    bool ok = katmed_web_auth_init(&w->auth,
        err == ESP_OK ? &stored : NULL,
        random_bytes, derive_password, commit_auth, w, monotonic_ms());
    wipe(&stored, sizeof(stored));
    return ok;
}

static bool resources_allow(bool after_start)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t internal_free = heap_caps_get_free_size(internal);
    size_t internal_largest = heap_caps_get_largest_free_block(internal);
    size_t dma_free = heap_caps_get_free_size(dma);
    size_t dma_largest = heap_caps_get_largest_free_block(dma);
    if (after_start)
        return internal_free >= POST_FREE_INTERNAL && dma_free >= POST_FREE_DMA &&
               internal_largest >= 12u * 1024u && dma_largest >= 1024u;
    return internal_free >= PRE_FREE_INTERNAL &&
           internal_largest >= PRE_LARGEST_INTERNAL &&
           dma_free >= PRE_FREE_DMA && dma_largest >= PRE_LARGEST_DMA;
}

static bool certificate_time_valid(const mbedtls_x509_crt *cert)
{
    /* The firmware may boot before time sync. Certificate login waits until
     * plausible UTC is available; password login remains usable meanwhile. */
    time_t now = time(NULL);
    if (now < 1577836800) return false; /* 2020-01-01 UTC */
    struct tm utc;
    if (!gmtime_r(&now, &utc)) return false;
    int y = utc.tm_year + 1900, mon = utc.tm_mon + 1, d = utc.tm_mday;
    const mbedtls_x509_time *a = &cert->valid_from, *b = &cert->valid_to;
    int64_t value = (((((int64_t)y * 100 + mon) * 100 + d) * 100 + utc.tm_hour) * 100 +
                     utc.tm_min) * 100 + utc.tm_sec;
    int64_t first = (((((int64_t)a->year * 100 + a->mon) * 100 + a->day) * 100 + a->hour) * 100 +
                     a->min) * 100 + a->sec;
    int64_t last = (((((int64_t)b->year * 100 + b->mon) * 100 + b->day) * 100 + b->hour) * 100 +
                    b->min) * 100 + b->sec;
    return value >= first && value <= last;
}

static void tls_session_callback(esp_https_server_user_cb_arg_t *arg)
{
    if (!arg || !arg->tls) return;
    int fd = -1;
    if (esp_tls_get_conn_sockfd(arg->tls, &fd) != ESP_OK) return;
    if (arg->user_cb_state == HTTPD_SSL_USER_CB_SESS_CLOSE) {
        if (web.certificate_fd == fd) {
            web.certificate_fd = -1;
            web.certificate_verified = false;
            wipe(web.certificate_fingerprint, sizeof(web.certificate_fingerprint));
        }
        return;
    }
    web.certificate_fd = fd;
    web.certificate_verified = false;
    wipe(web.certificate_fingerprint, sizeof(web.certificate_fingerprint));
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (heap_caps_get_free_size(internal) < LIVE_FREE_INTERNAL ||
        heap_caps_get_free_size(dma) < LIVE_FREE_DMA ||
        heap_caps_get_largest_free_block(dma) < 1024u) {
        if (web.https) httpd_sess_trigger_close(web.https, fd);
        return;
    }
    if (web.auth.record.cert_len == 0) return;
    mbedtls_ssl_context *ssl = esp_tls_get_ssl_context(arg->tls);
    if (!ssl || mbedtls_ssl_get_verify_result(ssl) != 0) return;
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(ssl);
    if (!peer || peer->next || !certificate_time_valid(peer)) return;
    if (sha256_bytes(peer->raw.p, peer->raw.len,
                     web.certificate_fingerprint) != 0) return;
    web.certificate_verified = katmed_web_auth_equal(
        web.certificate_fingerprint, web.auth.record.client_fingerprint, 32);
}

static bool certificate_on_request(httpd_req_t *req, uint8_t out[32])
{
    int fd = httpd_req_to_sockfd(req);
    if (!web.certificate_verified || fd < 0 || fd != web.certificate_fd) return false;
    memcpy(out, web.certificate_fingerprint, 32);
    return true;
}

static bool stop_listeners(void)
{
    if (web.redirect) {
        if (httpd_stop(web.redirect) != ESP_OK) {
            ESP_LOGE(WEB_TAG, "HTTP redirect stop failed");
            return false;
        }
        web.redirect = NULL;
    }
    if (web.https) {
        if (httpd_ssl_stop(web.https) != ESP_OK) {
            ESP_LOGE(WEB_TAG, "HTTPS server stop failed");
            return false;
        }
        web.https = NULL;
    }
    /* The HTTPS server borrows this PSA key for every TLS session. Release it
     * only after httpd_ssl_stop has closed those sessions. */
    if (web.tls_key_id != PSA_KEY_ID_NULL) {
        if (psa_destroy_key(web.tls_key_id) == PSA_SUCCESS) {
            web.tls_key_id = PSA_KEY_ID_NULL;
            wipe(&web.tls_key_config, sizeof(web.tls_key_config));
        } else {
            ESP_LOGE(WEB_TAG, "HTTPS server key release failed");
            return false;
        }
    }
    web.started = false;
    web.certificate_fd = -1;
    web.certificate_verified = false;
    wipe(web.certificate_fingerprint, sizeof(web.certificate_fingerprint));
    return true;
}

/* URI handler registration and request logic follow below. */

static esp_err_t reply(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t error_reply(httpd_req_t *req, const char *status, const char *error)
{
    char text[96];
    snprintf(text, sizeof(text), "{\"error\":\"%s\"}", error);
    return reply(req, status, text);
}

static esp_err_t json_reply(httpd_req_t *req, const char *status, cJSON *obj)
{
    if (!obj) return error_reply(req, "503 Service Unavailable", "memory");
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!text) return error_reply(req, "503 Service Unavailable", "memory");
    esp_err_t result = reply(req, status, text);
    cJSON_free(text);
    return result;
}

static bool request_host(httpd_req_t *req, char host[INET_ADDRSTRLEN])
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage local = {0};
    socklen_t length = sizeof(local);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&local, &length) != 0 ||
        length < sizeof(struct sockaddr_in)) return false;
    const void *address = NULL;
    if (local.ss_family == AF_INET) {
        address = &((struct sockaddr_in *)&local)->sin_addr;
    } else if (local.ss_family == AF_INET6 &&
               length >= sizeof(struct sockaddr_in6) &&
               IN6_IS_ADDR_V4MAPPED(&((struct sockaddr_in6 *)&local)->sin6_addr)) {
        address = &((struct sockaddr_in6 *)&local)->sin6_addr.s6_addr[12];
    }
    if (!address || !inet_ntop(AF_INET, address, host, INET_ADDRSTRLEN)) return false;
    size_t header_length = httpd_req_get_hdr_value_len(req, "Host");
    if (header_length == 0 || header_length > 64) return false;
    char supplied[65];
    if (httpd_req_get_hdr_value_str(req, "Host", supplied, sizeof(supplied)) != ESP_OK)
        return false;
    return katmed_web_authority_matches(host, supplied,
                                         req->handle == web.redirect ? 80 : 443);
}

static bool same_origin(httpd_req_t *req)
{
    char host[INET_ADDRSTRLEN];
    if (!request_host(req, host)) return false;
    size_t length = httpd_req_get_hdr_value_len(req, "Origin");
    if (length == 0 || length > 64) return false;
    char supplied[65];
    if (httpd_req_get_hdr_value_str(req, "Origin", supplied, sizeof(supplied)) != ESP_OK)
        return false;
    return katmed_web_origin_matches(host, supplied);
}

static bool get_cookie(httpd_req_t *req, char value[KATMED_WEB_TOKEN_HEX + 1])
{
    size_t length = httpd_req_get_hdr_value_len(req, "Cookie");
    if (length == 0 || length > 512) return false;
    char header[513];
    if (httpd_req_get_hdr_value_str(req, "Cookie", header, sizeof(header)) != ESP_OK)
        return false;
    return katmed_web_cookie_token(header, value);
}

static KatmedWebSession *session_from_request(httpd_req_t *req,
                                              char token_out[KATMED_WEB_TOKEN_HEX + 1])
{
    if (!get_cookie(req, token_out)) return NULL;
    return katmed_web_auth_session(&web.auth, token_out, monotonic_ms());
}

static KatmedWebSession *require_session(httpd_req_t *req, bool mutate,
                                         char token_out[KATMED_WEB_TOKEN_HEX + 1])
{
    if (!request_host(req, (char[INET_ADDRSTRLEN]){0})) {
        error_reply(req, "403 Forbidden", "host");
        return NULL;
    }
    KatmedWebSession *session = session_from_request(req, token_out);
    if (!session) {
        error_reply(req, "401 Unauthorized", "authentication_required");
        return NULL;
    }
    if (mutate) {
        if (!same_origin(req)) {
            error_reply(req, "403 Forbidden", "origin");
            return NULL;
        }
        size_t len = httpd_req_get_hdr_value_len(req, "X-Katmed-CSRF");
        char csrf[KATMED_WEB_TOKEN_HEX + 1];
        if (len != KATMED_WEB_TOKEN_HEX ||
            httpd_req_get_hdr_value_str(req, "X-Katmed-CSRF", csrf, sizeof(csrf)) != ESP_OK ||
            !katmed_web_auth_equal(csrf, session->csrf, KATMED_WEB_TOKEN_HEX)) {
            error_reply(req, "403 Forbidden", "csrf");
            return NULL;
        }
    }
    return session;
}

static bool module_authorize(httpd_req_t *req, bool mutate, void *context)
{
    char token[KATMED_WEB_TOKEN_HEX + 1];
    (void)context;
    return require_session(req, mutate, token) != NULL;
}

static void wipe_json(cJSON *node)
{
    for (cJSON *item = node; item; item = item->next) {
        if (item->child) wipe_json(item->child);
        if (item->valuestring) wipe(item->valuestring, strlen(item->valuestring));
    }
}

static cJSON *read_json(httpd_req_t *req)
{
    size_t type_length = httpd_req_get_hdr_value_len(req, "Content-Type");
    char type[32];
    if (type_length != strlen("application/json") ||
        httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") != 0 ||
        req->content_len <= 0 || req->content_len > BODY_MAX) return NULL;
    size_t done = 0;
    while (done < req->content_len) {
        int result = httpd_req_recv(req, web.request_body + done, req->content_len - done);
        if (result <= 0) {
            wipe(web.request_body, sizeof(web.request_body));
            return NULL;
        }
        done += (size_t)result;
    }
    web.request_body[done] = '\0';
    cJSON *obj = cJSON_ParseWithLength(web.request_body, done);
    wipe(web.request_body, done + 1);
    return obj;
}

static bool keys_only(const cJSON *obj, const char *const *keys, size_t count)
{
    if (!cJSON_IsObject(obj)) return false;
    for (const cJSON *item = obj->child; item; item = item->next) {
        bool known = false;
        for (size_t i = 0; i < count; ++i)
            if (strcmp(item->string, keys[i]) == 0) known = true;
        if (!known) return false;
        for (const cJSON *other = item->next; other; other = other->next)
            if (strcmp(item->string, other->string) == 0) return false;
    }
    return true;
}

static const char *json_string(const cJSON *obj, const char *key, size_t max_len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring ||
        strnlen(item->valuestring, max_len + 1) > max_len) return NULL;
    return item->valuestring;
}

#ifdef KATMED_WEB_SECURITY_JS_GZIP
/* Only the two build-generated gzip members reach this path. Never allocate
 * their full decoded size: one ROM inflater and one 32 KiB history ring suffice.
 * The synchronous HTTPS server has one socket and runs handlers serially. */
typedef struct {
    tinfl_decompressor inflater;
    unsigned char dictionary[TINFL_LZ_DICT_SIZE];
} ScriptInflate;
_Static_assert(sizeof(ScriptInflate) <= 44u * 1024u, "script workspace exceeds bound");

static uint32_t script_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Validate before committing a 200 response, then replay the immutable flash
 * member into chunks with the same workspace. A send failure returns directly
 * to httpd, which closes the session; never append a successful terminator. */
static esp_err_t script_inflate(ScriptInflate *work, const unsigned char *start,
                                size_t length, httpd_req_t *req)
{
    if (length < 18 || start[0] != 0x1f || start[1] != 0x8b ||
        start[2] != 8 || start[3] != 0) return ESP_FAIL;
    size_t input = 10, output = 0, offset = 0;
    uint32_t crc = 0;
    uint32_t expected = script_le32(start + length - 4);
    /* Hard limit independent of the member trailer; covers the full catalog. */
    if (expected > 128u * 1024u) return ESP_FAIL;
    tinfl_init(&work->inflater);
    for (;;) {
        size_t consumed = length - 8 - input;
        size_t produced = TINFL_LZ_DICT_SIZE - offset;
        tinfl_status status = tinfl_decompress(&work->inflater, start + input,
            &consumed, work->dictionary, work->dictionary + offset, &produced, 0);
        input += consumed;
        if (status < TINFL_STATUS_DONE || produced > expected - output)
            return ESP_FAIL;
        crc = esp_rom_crc32_le(crc, work->dictionary + offset, produced);
        output += produced;
        if (req && produced) {
            esp_err_t sent = httpd_resp_send_chunk(req,
                (const char *)work->dictionary + offset, produced);
            if (sent != ESP_OK) return sent;
        }
        offset = (offset + produced) % TINFL_LZ_DICT_SIZE;
        if (status == TINFL_STATUS_DONE)
            return input == length - 8 && output == expected &&
                   crc == script_le32(start + length - 8) ? ESP_OK : ESP_FAIL;
        if (status != TINFL_STATUS_HAS_MORE_OUTPUT || (!consumed && !produced))
            return ESP_FAIL;
    }
}

/* Return an HTTP quality in thousandths, or -1 for malformed syntax. */
static int gzip_quality(const char *begin, const char *end)
{
    while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) --end;
    if (begin == end || (*begin != '0' && *begin != '1')) return -1;
    bool one = *begin++ == '1';
    if (begin == end) return one ? 1000 : 0;
    if (*begin++ != '.' || end - begin > 3) return -1;
    int quality = 0, scale = 100;
    for (; begin < end; ++begin, scale /= 10) {
        if (*begin < '0' || *begin > '9' || (one && *begin != '0')) return -1;
        quality += (*begin - '0') * scale;
    }
    return one ? 1000 : quality;
}

/* 2=gzip, 1=identity, 0=no acceptable representation, -1=bad/oversize header.
 * An absent or empty field preserves the historical raw-script contract. */
static int script_coding(httpd_req_t *req)
{
    size_t length = httpd_req_get_hdr_value_len(req, "Accept-Encoding");
    if (length == 0) return 1;
    if (length > 256) return -1;
    char value[257];
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", value, sizeof(value)) != ESP_OK)
        return -1;
    int gzip = -1, identity = -1, wildcard = -1;
    const char *part = value;
    while (*part) {
        const char *end = strchr(part, ',');
        if (!end) end = part + strlen(part);
        const char *semi = memchr(part, ';', (size_t)(end - part));
        const char *token_end = semi ? semi : end;
        while (part < token_end && (*part == ' ' || *part == '\t')) ++part;
        while (token_end > part && (token_end[-1] == ' ' || token_end[-1] == '\t'))
            --token_end;
        /* Empty HTTP list elements are ignored. The 256-byte field limit
         * bounds even an all-empty list to at most 257 elements. A parameter
         * without a coding token is still malformed. */
        if (part == token_end && !semi) {
            part = *end ? end + 1 : end;
            continue;
        }
        if (part == token_end) return -1;
        int quality = 1000;
        if (semi) {
            const char *q = semi + 1;
            while (q < end && (*q == ' ' || *q == '\t')) ++q;
            if (end - q < 2 || (*q != 'q' && *q != 'Q') || q[1] != '=') return -1;
            quality = gzip_quality(q + 2, end);
            if (quality < 0) return -1;
        }
        int *selected = NULL;
        if (token_end - part == 4 && strncasecmp(part, "gzip", 4) == 0)
            selected = &gzip;
        else if (token_end - part == 8 && strncasecmp(part, "identity", 8) == 0)
            selected = &identity;
        else if (token_end - part == 1 && *part == '*') selected = &wildcard;
        /* Duplicate entries conservatively retain the lowest quality. */
        if (selected && (*selected < 0 || quality < *selected)) *selected = quality;
        if (*end == '\0') break;
        part = end + 1;
    }
    if (gzip < 0) gzip = wildcard < 0 ? 0 : wildcard;
    if (identity < 0) identity = wildcard == 0 ? 0 : 1000;
    if (gzip > 0 && gzip >= identity) return 2;
    return identity > 0 ? 1 : 0;
}
#endif

static esp_err_t asset_reply(httpd_req_t *req, const unsigned char *start,
                              const unsigned char *end, const char *type,
                              bool gzip_encoded)
{
    char host[INET_ADDRSTRLEN];
    if (!request_host(req, host)) return error_reply(req, "403 Forbidden", "host");
#ifdef KATMED_WEB_SECURITY_JS_GZIP
    if (gzip_encoded) {
        httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
        int coding = script_coding(req);
        if (coding < 0) return error_reply(req, "400 Bad Request", "accept_encoding");
        if (coding == 0)
            return error_reply(req, "406 Not Acceptable", "encoding_unavailable");
        if (coding == 1) {
            ScriptInflate *work = malloc(sizeof(*work));
            if (!work) return error_reply(req, "503 Service Unavailable", "script_memory");
            esp_err_t result = script_inflate(work, start, (size_t)(end - start), NULL);
            if (result != ESP_OK) {
                free(work);
                return error_reply(req, "500 Internal Server Error", "script_data");
            }
            httpd_resp_set_type(req, type);
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
            httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
            httpd_resp_set_hdr(req, "Content-Security-Policy",
                "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
                "img-src 'self'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'");
            result = script_inflate(work, start, (size_t)(end - start), req);
            free(work);
            if (result != ESP_OK) return result;
            return httpd_resp_send_chunk(req, NULL, 0);
        }
    }
#endif
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    if (gzip_encoded) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    httpd_resp_set_hdr(req, "Content-Security-Policy",
        "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
        "img-src 'self'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'");
    return httpd_resp_send(req, (const char *)start, (ssize_t)(end - start));
}

static esp_err_t page_get(httpd_req_t *req)
{
    return asset_reply(req, _binary_index_html_start, _binary_index_html_end,
                       "text/html; charset=utf-8", false);
}
static esp_err_t style_get(httpd_req_t *req)
{
    return asset_reply(req, _binary_style_css_start, _binary_style_css_end,
                       "text/css; charset=utf-8", false);
}
static esp_err_t script_get(httpd_req_t *req)
{
    return asset_reply(req, _binary_app_js_start, _binary_app_js_end,
                       "application/javascript; charset=utf-8",
#ifdef KATMED_WEB_SECURITY_JS_GZIP
                       true);
#else
                       false);
#endif
}
static esp_err_t language_script_get(httpd_req_t *req)
{
    return asset_reply(req, _binary_i18n_js_start, _binary_i18n_js_end,
                       "application/javascript; charset=utf-8",
#ifdef KATMED_WEB_SECURITY_JS_GZIP
                       true);
#else
                       false);
#endif
}

static const char *const language_codes[KATMED_LANGUAGE_COUNT] = {
    "en", "de", "fr", "nl", "uk", "ar", "zh-Hans", "es", "ja", "bar", "eo", "tlh-Latn"
};

static esp_err_t language_get(httpd_req_t *req)
{
    uint8_t language = KATMED_LANG_EN;
    if (web.services.language_get &&
        !web.services.language_get(web.services.context, &language))
        return error_reply(req, "503 Service Unavailable", "unavailable");
    if (language >= KATMED_LANGUAGE_COUNT) language = KATMED_LANG_EN;
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return error_reply(req, "503 Service Unavailable", "unavailable");
    cJSON_AddStringToObject(obj, "language", language_codes[language]);
    return json_reply(req, "200 OK", obj);
}

static esp_err_t language_put(httpd_req_t *req)
{
    char token[KATMED_WEB_TOKEN_HEX + 1];
    if (!require_session(req, true, token)) return ESP_OK;
    cJSON *obj = read_json(req);
    const char *const keys[] = {"language"};
    const char *code = json_string(obj, "language", 16);
    int selected = -1;
    if (keys_only(obj, keys, 1) && code)
        for (unsigned i = 0; i < KATMED_LANGUAGE_COUNT; ++i)
            if (strcmp(code, language_codes[i]) == 0) selected = (int)i;
    if (obj) { wipe_json(obj); cJSON_Delete(obj); }
    if (selected < 0) return error_reply(req, "422 Unprocessable Entity", "invalid");
    if (!web.services.language_set ||
        !web.services.language_set(web.services.context, (uint8_t)selected))
        return error_reply(req, "503 Service Unavailable", "unavailable");
    return language_get(req);
}

static cJSON *bootstrap_object(httpd_req_t *req, KatmedWebSession *session)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    bool setup = katmed_web_auth_setup_required(&web.auth);
    cJSON_AddBoolToObject(obj, "setup_required", setup);
    cJSON_AddBoolToObject(obj, "authenticated", session != NULL && !setup);
    KatmedWebDeviceInfo device = {0};
    snprintf(device.device_name, sizeof(device.device_name), "KatmedESP");
    if (web.services.device_info &&
        !web.services.device_info(web.services.context, &device)) {
        cJSON_Delete(obj);
        return NULL;
    }
    if (!device.device_name[0] ||
        strnlen(device.device_name, sizeof(device.device_name)) == sizeof(device.device_name)) {
        cJSON_Delete(obj);
        return NULL;
    }
    cJSON_AddStringToObject(obj, "device_name", device.device_name);
    if (session && !setup) {
        cJSON_AddStringToObject(obj, "auth_method",
                                session->certificate ? "certificate" : "password");
        cJSON_AddStringToObject(obj, "csrf_token", session->csrf);
    } else {
        cJSON_AddNullToObject(obj, "auth_method");
        cJSON_AddNullToObject(obj, "csrf_token");
    }
    if (setup) {
        char nonce[KATMED_WEB_TOKEN_HEX + 1];
        if (!katmed_web_auth_setup_nonce(&web.auth, monotonic_ms(), nonce)) {
            cJSON_Delete(obj);
            return NULL;
        }
        cJSON_AddStringToObject(obj, "setup_nonce", nonce);
        wipe(nonce, sizeof(nonce));
    } else {
        cJSON_AddNullToObject(obj, "setup_nonce");
    }
    cJSON *cert = cJSON_AddObjectToObject(obj, "client_certificate");
    if (!cert) {
        cJSON_Delete(obj);
        return NULL;
    }
    cJSON_AddBoolToObject(cert, "enrolled", session && web.auth.record.cert_len > 0);
    (void)req;
    return obj;
}

static const char *device_name_state(const KatmedWebDeviceInfo *device)
{
    if (device->uncertain) return "uncertain";
    return device->assigned ? "assigned" : "initial";
}

static cJSON *device_info_object(void)
{
    KatmedWebDeviceInfo device = {0};
    snprintf(device.device_name, sizeof(device.device_name), "KatmedESP");
    if (web.services.device_info &&
        !web.services.device_info(web.services.context, &device)) return NULL;
    if (!device.device_name[0] ||
        strnlen(device.device_name, sizeof(device.device_name)) == sizeof(device.device_name))
        return NULL;
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddStringToObject(obj, "device_name", device.device_name);
    cJSON_AddStringToObject(obj, "hostname", device.device_name);
    cJSON_AddStringToObject(obj, "name_state", device_name_state(&device));
    return obj;
}

static void set_session_cookie(httpd_req_t *req, const char *token)
{
    char cookie[160];
    snprintf(cookie, sizeof(cookie),
             "katmed_session=%s; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=3600",
             token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

static esp_err_t auth_result_reply(httpd_req_t *req, KatmedWebAuthResult result)
{
    switch (result) {
    case KATMED_WEB_AUTH_CONFLICT:
        return error_reply(req, "409 Conflict", "already_setup");
    case KATMED_WEB_AUTH_STORAGE_ERROR:
        return error_reply(req, "503 Service Unavailable", "storage");
    case KATMED_WEB_AUTH_INVALID:
        return error_reply(req, "422 Unprocessable Entity", "invalid");
    case KATMED_WEB_AUTH_DENIED:
        return error_reply(req, "401 Unauthorized", "denied");
    default:
        return error_reply(req, "503 Service Unavailable", "internal");
    }
}

static esp_err_t bootstrap_get(httpd_req_t *req)
{
    char host[INET_ADDRSTRLEN];
    if (!request_host(req, host)) return error_reply(req, "403 Forbidden", "host");
    char token[KATMED_WEB_TOKEN_HEX + 1];
    KatmedWebSession *session = session_from_request(req, token);
    if (web.auth_mutex) xSemaphoreTake(web.auth_mutex, portMAX_DELAY);
    cJSON *obj = bootstrap_object(req, session);
    if (web.auth_mutex) xSemaphoreGive(web.auth_mutex);
    return json_reply(req, "200 OK", obj);
}

static esp_err_t info_get(httpd_req_t *req)
{
    char token[KATMED_WEB_TOKEN_HEX + 1];
    if (!require_session(req, false, token)) return ESP_OK;
    return json_reply(req, "200 OK", device_info_object());
}

static esp_err_t setup_post(httpd_req_t *req)
{
    if (!same_origin(req)) return error_reply(req, "403 Forbidden", "origin");
    if (!katmed_web_auth_setup_required(&web.auth))
        return error_reply(req, "409 Conflict", "already_setup");
    cJSON *obj = read_json(req);
    const char *const keys[] = {"password", "setup_nonce", "setup_code"};
    if (!keys_only(obj, keys, 3)) {
        if (obj) cJSON_Delete(obj);
        return error_reply(req, "422 Unprocessable Entity", "invalid");
    }
    const char *password = json_string(obj, "password", 128);
    const char *nonce = json_string(obj, "setup_nonce", KATMED_WEB_TOKEN_HEX);
    const char *code = json_string(obj, "setup_code", 6);
    KatmedWebSession *session = NULL;
    if (web.auth_mutex) xSemaphoreTake(web.auth_mutex, portMAX_DELAY);
    KatmedWebAuthResult result = password && nonce && code ?
        katmed_web_auth_setup(&web.auth, password, nonce, code, monotonic_ms(), &session) :
        KATMED_WEB_AUTH_INVALID;
    if (web.auth_mutex) xSemaphoreGive(web.auth_mutex);
    wipe_json(obj);
    cJSON_Delete(obj);
    if (result != KATMED_WEB_AUTH_OK) return auth_result_reply(req, result);
    set_session_cookie(req, session->token);
    return json_reply(req, "200 OK", bootstrap_object(req, session));
}

static esp_err_t login_post(httpd_req_t *req)
{
    if (!same_origin(req)) return error_reply(req, "403 Forbidden", "origin");
    if (katmed_web_auth_setup_required(&web.auth))
        return error_reply(req, "403 Forbidden", "setup_required");
    cJSON *obj = read_json(req);
    const char *method = json_string(obj, "method", 16);
    KatmedWebSession *session = NULL;
    KatmedWebAuthResult result = KATMED_WEB_AUTH_INVALID;
    if (method && strcmp(method, "password") == 0) {
        const char *const keys[] = {"method", "password"};
        const char *password = json_string(obj, "password", 128);
        if (keys_only(obj, keys, 2) && password)
            result = katmed_web_auth_login_password(&web.auth, password,
                                                     monotonic_ms(), &session);
    } else if (method && strcmp(method, "certificate") == 0) {
        const char *const keys[] = {"method"};
        uint8_t fingerprint[32];
        bool verified = certificate_on_request(req, fingerprint);
        if (keys_only(obj, keys, 1))
            result = katmed_web_auth_login_certificate(&web.auth, fingerprint,
                                                        verified, monotonic_ms(), &session);
        wipe(fingerprint, sizeof(fingerprint));
    }
    if (obj) {
        wipe_json(obj);
        cJSON_Delete(obj);
    }
    if (result != KATMED_WEB_AUTH_OK) return auth_result_reply(req, result);
    char old[KATMED_WEB_TOKEN_HEX + 1];
    if (get_cookie(req, old)) katmed_web_auth_logout(&web.auth, old);
    set_session_cookie(req, session->token);
    return json_reply(req, "200 OK", bootstrap_object(req, session));
}

static esp_err_t logout_post(httpd_req_t *req)
{
    char token[KATMED_WEB_TOKEN_HEX + 1];
    if (!require_session(req, true, token)) return ESP_OK;
    cJSON *obj = read_json(req);
    const char *const keys[] = {NULL};
    if (!keys_only(obj, keys, 0)) {
        if (obj) cJSON_Delete(obj);
        return error_reply(req, "422 Unprocessable Entity", "invalid");
    }
    cJSON_Delete(obj);
    katmed_web_auth_logout(&web.auth, token);
    httpd_resp_set_hdr(req, "Set-Cookie",
                       "katmed_session=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0");
    return reply(req, "200 OK", "{\"authenticated\":false}");
}

static bool parse_client_certificate(const char *pem, uint8_t out[KATMED_WEB_CERT_MAX],
                                     size_t *out_len, uint8_t fingerprint[32])
{
    if (!pem || strstr(pem, "PRIVATE KEY") ||
        !strstr(pem, "-----BEGIN CERTIFICATE-----")) return false;
    mbedtls_x509_crt cert;
    mbedtls_x509_crt_init(&cert);
    bool ok = false;
    uint32_t flags = 0;
    int result = mbedtls_x509_crt_parse(&cert, (const uint8_t *)pem, strlen(pem) + 1);
    if (result != 0 || cert.next || cert.raw.len == 0 ||
        cert.raw.len > KATMED_WEB_CERT_MAX ||
        !mbedtls_pk_can_do_psa(&cert.pk, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                               PSA_KEY_USAGE_VERIFY_HASH) ||
        mbedtls_pk_get_bitlen(&cert.pk) != 256 ||
        !cert.ext_key_usage.buf.p ||
        mbedtls_x509_crt_check_key_usage(&cert, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0 ||
        mbedtls_x509_crt_check_extended_key_usage(&cert, MBEDTLS_OID_CLIENT_AUTH,
                                                  MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH)) != 0 ||
        mbedtls_x509_crt_verify(&cert, &cert, NULL, NULL, &flags, NULL, NULL) != 0 ||
        flags != 0 ||
        sha256_bytes(cert.raw.p, cert.raw.len, fingerprint) != 0)
        goto done;
    memcpy(out, cert.raw.p, cert.raw.len);
    *out_len = cert.raw.len;
    ok = true;
done:
    mbedtls_x509_crt_free(&cert);
    return ok;
}

static esp_err_t client_cert_get(httpd_req_t *req)
{
    char token[KATMED_WEB_TOKEN_HEX + 1];
    if (!require_session(req, false, token)) return ESP_OK;
    return reply(req, "200 OK", web.auth.record.cert_len ?
                 "{\"enrolled\":true}" : "{\"enrolled\":false}");
}

static esp_err_t client_cert_post(httpd_req_t *req)
{
    char token[KATMED_WEB_TOKEN_HEX + 1];
    if (!require_session(req, true, token)) return ESP_OK;
    cJSON *obj = read_json(req);
    const char *const keys[] = {"certificate_pem", "password"};
    const char *pem = json_string(obj, "certificate_pem", 2000);
    const char *password = json_string(obj, "password", 128);
    if (!keys_only(obj, keys, 2) || !pem || !password) {
        if (obj) { wipe_json(obj); cJSON_Delete(obj); }
        return error_reply(req, "422 Unprocessable Entity", "invalid");
    }
    if (!katmed_web_auth_recheck_password(&web.auth, password, monotonic_ms())) {
        wipe_json(obj);
        cJSON_Delete(obj);
        return error_reply(req, "401 Unauthorized", "denied");
    }
    uint8_t der[KATMED_WEB_CERT_MAX];
    uint8_t fingerprint[32];
    size_t len = 0;
    bool valid = parse_client_certificate(pem, der, &len, fingerprint);
    wipe_json(obj);
    cJSON_Delete(obj);
    if (!valid) return error_reply(req, "422 Unprocessable Entity", "invalid_certificate");
    KatmedWebAuthResult result = katmed_web_auth_set_client(&web.auth, der, len, fingerprint);
    wipe(der, sizeof(der));
    wipe(fingerprint, sizeof(fingerprint));
    if (result != KATMED_WEB_AUTH_OK) return auth_result_reply(req, result);
    web.restart_due_ms = monotonic_ms() + 500;
    web.restart_pending = true;
    return reply(req, "200 OK", "{\"enrolled\":true}");
}

static esp_err_t client_cert_delete(httpd_req_t *req)
{
    char token[KATMED_WEB_TOKEN_HEX + 1];
    if (!require_session(req, true, token)) return ESP_OK;
    cJSON *obj = read_json(req);
    const char *const keys[] = {"password"};
    const char *password = json_string(obj, "password", 128);
    if (!keys_only(obj, keys, 1) || !password) {
        if (obj) { wipe_json(obj); cJSON_Delete(obj); }
        return error_reply(req, "422 Unprocessable Entity", "invalid");
    }
    bool verified = katmed_web_auth_recheck_password(&web.auth, password, monotonic_ms());
    wipe_json(obj);
    cJSON_Delete(obj);
    if (!verified) return error_reply(req, "401 Unauthorized", "denied");
    KatmedWebAuthResult result = katmed_web_auth_remove_client(&web.auth);
    if (result != KATMED_WEB_AUTH_OK) return auth_result_reply(req, result);
    web.restart_due_ms = monotonic_ms() + 500;
    web.restart_pending = true;
    return reply(req, "200 OK", "{\"enrolled\":false}");
}

static const char *clock_style_name(ClockStyle value)
{
    switch (value) {
    case DIGITAL_LARGE: return "digital_large";
    case DIGITAL_DATE: return "digital_date";
    case ANALOG: return "analog";
    default: return NULL;
    }
}

static const char *rotation_name(RotationMode value)
{
    switch (value) {
    case ROTATION_OFF: return "off";
    case ROTATION_NAME_ORDER: return "name_order";
    case ROTATION_RANDOM: return "random";
    default: return NULL;
    }
}

static const char *due_state_name(KatmedDueState value)
{
    switch (value) {
    case KATMED_DUE_NONE: return "none";
    case KATMED_DUE_ACTIVE: return "active";
    case KATMED_DUE_PAUSED_BOOT: return "paused_boot";
    default: return NULL;
    }
}

static const char *restore_status_name(KatmedRestoreStatus value)
{
    switch (value) {
    case KATMED_RESTORE_NONE: return "none";
    case KATMED_RESTORE_EXPIRED: return "expired";
    case KATMED_RESTORE_CONFLICT: return "conflict";
    default: return NULL;
    }
}

static int orientation_degrees(Orientation value)
{
    switch (value) {
    case ORIENTATION_0: return 0;
    case ORIENTATION_90: return 90;
    case ORIENTATION_180: return 180;
    case ORIENTATION_270: return 270;
    default: return -1;
    }
}

static cJSON *config_object(const KatmedConfigSnapshot *snapshot)
{
    if (!snapshot || !clock_style_name(snapshot->clock_style) ||
        !rotation_name(snapshot->rotation) || !due_state_name(snapshot->next_due_state) ||
        !restore_status_name(snapshot->restore_status) ||
        orientation_degrees(snapshot->orientation) < 0 ||
        strnlen(snapshot->revision, sizeof(snapshot->revision)) == sizeof(snapshot->revision))
        return NULL;
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddStringToObject(obj, "medication_hours", snapshot->medication_hours);
    cJSON_AddStringToObject(obj, "image_hours", snapshot->image_hours);
    if (snapshot->next_due_state == KATMED_DUE_NONE)
        cJSON_AddNullToObject(obj, "next_due_hhmm");
    else
        cJSON_AddStringToObject(obj, "next_due_hhmm", snapshot->next_due_hhmm);
    cJSON_AddStringToObject(obj, "next_due_state", due_state_name(snapshot->next_due_state));
    cJSON_AddStringToObject(obj, "restore_status",
                            restore_status_name(snapshot->restore_status));
    cJSON_AddStringToObject(obj, "clock_style", clock_style_name(snapshot->clock_style));
    cJSON_AddNumberToObject(obj, "orientation", orientation_degrees(snapshot->orientation));
    cJSON_AddBoolToObject(obj,"orientation_auto_supported",snapshot->orientation_auto_supported);
    cJSON_AddBoolToObject(obj,"orientation_automatic",snapshot->orientation_automatic);
    char brightness[4];snprintf(brightness,sizeof(brightness),"%u",snapshot->display.brightness);
    cJSON_AddStringToObject(obj,"brightness",brightness);
    cJSON_AddStringToObject(obj,"night_enabled",snapshot->display.night_enabled?"on":"off");
    char night_time[10];snprintf(night_time,sizeof(night_time),"%02u:%02u",snapshot->display.night_start/60u,snapshot->display.night_start%60u);
    cJSON_AddStringToObject(obj,"night_start",night_time);
    snprintf(night_time,sizeof(night_time),"%02u:%02u",snapshot->display.night_end/60u,snapshot->display.night_end%60u);
    cJSON_AddStringToObject(obj,"night_end",night_time);
    cJSON *copy=cJSON_AddObjectToObject(obj,"night_copy");
    cJSON_AddBoolToObject(copy,"local_saved",snapshot->night_copy.local_saved);
    cJSON_AddBoolToObject(copy,"active",snapshot->night_copy.active);
    cJSON_AddBoolToObject(copy,"finished",snapshot->night_copy.finished);
    cJSON_AddBoolToObject(copy,"membership_known",snapshot->night_copy.membership_known);
    cJSON_AddNumberToObject(copy,"expected",snapshot->night_copy.expected);
    cJSON_AddNumberToObject(copy,"acknowledged",snapshot->night_copy.acknowledged);
    cJSON_AddNumberToObject(copy,"rejected",snapshot->night_copy.rejected);
    cJSON_AddStringToObject(obj, "rotation", rotation_name(snapshot->rotation));
    cJSON_AddStringToObject(obj, "revision", snapshot->revision);
    return obj;
}

static esp_err_t bridge_error(httpd_req_t *req, KatmedConfigStatus status)
{
    switch (status) {
    case KATMED_CONFIG_INVALID:
        return error_reply(req, "422 Unprocessable Entity", "invalid");
    case KATMED_CONFIG_STALE:
        return error_reply(req, "409 Conflict", "stale");
    case KATMED_CONFIG_PENDING:
        return error_reply(req, "409 Conflict", "pending");
    case KATMED_CONFIG_TIME_UNAVAILABLE:
        return error_reply(req, "503 Service Unavailable", "time_unavailable");
    case KATMED_CONFIG_COMMIT_UNKNOWN:
        return error_reply(req, "503 Service Unavailable", "commit_unknown");
    default:
        return error_reply(req, "503 Service Unavailable", "unavailable");
    }
}

static esp_err_t config_get(httpd_req_t *req)
{
    char token[KATMED_WEB_TOKEN_HEX + 1];
    if (!require_session(req, false, token)) return ESP_OK;
    KatmedConfigRequest request = {.operation = KATMED_CONFIG_GET};
    KatmedConfigResponse response = {0};
    KatmedConfigStatus status = katmed_config_exchange(&request, &response,
                                                       BRIDGE_TIMEOUT_MS);
    if (status != KATMED_CONFIG_OK || response.status != KATMED_CONFIG_OK)
        return bridge_error(req, status != KATMED_CONFIG_OK ? status : response.status);
    return json_reply(req, "200 OK", config_object(&response.snapshot));
}

static esp_err_t runtime_setup_get(httpd_req_t *req) {
    char token[KATMED_WEB_TOKEN_HEX+1];if(!require_session(req,false,token))return ESP_OK;
    KatmedConfigRequest request={.operation=KATMED_CONFIG_SETUP_GET};KatmedConfigResponse response={0};
    KatmedConfigStatus status=katmed_config_exchange(&request,&response,BRIDGE_TIMEOUT_MS);
    if(status!=KATMED_CONFIG_OK)return bridge_error(req,status);
    cJSON *obj=cJSON_CreateObject();
    cJSON_AddBoolToObject(obj,"configured",response.setup.configured);
    cJSON_AddNumberToObject(obj,"mode",response.setup.mode);
    cJSON_AddBoolToObject(obj,"clock_required",response.setup.clock_required);
    cJSON_AddBoolToObject(obj,"setup_open",response.setup.setup_open);
    cJSON_AddStringToObject(obj,"revision",response.snapshot.revision);
    return json_reply(req,"200 OK",obj);
}

static esp_err_t runtime_setup_exit_post(httpd_req_t *req) {
    char token[KATMED_WEB_TOKEN_HEX+1];if(!require_session(req,true,token))return ESP_OK;
    cJSON *obj=read_json(req);
    const char *const keys[]={"revision"};
    if(!keys_only(obj,keys,1)){if(obj){wipe_json(obj);cJSON_Delete(obj);}return error_reply(req,"422 Unprocessable Entity","invalid");}
    const char *revision=json_string(obj,"revision",KATMED_CONFIG_REVISION_MAX-1);
    const bool valid=revision!=NULL;
    KatmedConfigRequest request={.operation=KATMED_CONFIG_SETUP_EXIT};
    if(revision)snprintf(request.revision,sizeof(request.revision),"%s",revision);
    wipe_json(obj);cJSON_Delete(obj);
    if(!valid)return error_reply(req,"422 Unprocessable Entity","invalid");
    KatmedConfigResponse response={0};
    KatmedConfigStatus status=katmed_config_exchange(&request,&response,BRIDGE_TIMEOUT_MS);
    if(status!=KATMED_CONFIG_OK)return bridge_error(req,status);
    cJSON *reply=cJSON_CreateObject();cJSON_AddBoolToObject(reply,"connection_will_close",true);
    return json_reply(req,"200 OK",reply);
}

static bool setup_hex(const char *text,uint8_t *out,size_t bytes) {
    if(!text || strlen(text)!=2*bytes)return false;
    for(size_t i=0;i<bytes;++i) {
        unsigned value=0;
        for(size_t j=0;j<2;++j) {char c=text[2*i+j];unsigned digit;
            if(c>='0'&&c<='9')digit=c-'0';else if(c>='a'&&c<='f')digit=c-'a'+10;else if(c>='A'&&c<='F')digit=c-'A'+10;else return false;
            value=value*16+digit;}
        out[i]=(uint8_t)value;
    }return true;
}

static esp_err_t runtime_setup_put(httpd_req_t *req) {
    char token[KATMED_WEB_TOKEN_HEX+1];if(!require_session(req,true,token))return ESP_OK;
    cJSON *obj=read_json(req);
    const char *const keys[]={"mode","group_action","group","secret","ssid","password","time_source","local_datetime","revision"};
    if(!keys_only(obj,keys,9)){if(obj){wipe_json(obj);cJSON_Delete(obj);}return error_reply(req,"422 Unprocessable Entity","invalid");}
    KatmedConfigRequest request={.operation=KATMED_CONFIG_SETUP_SAVE};
    const cJSON *mode=cJSON_GetObjectItemCaseSensitive(obj,"mode"),*action=cJSON_GetObjectItemCaseSensitive(obj,"group_action");
    const char *revision=json_string(obj,"revision",KATMED_CONFIG_REVISION_MAX-1);
    bool valid=cJSON_IsNumber(mode) && mode->valuedouble>=0 && mode->valuedouble<=2 && mode->valuedouble==mode->valueint &&
        cJSON_IsNumber(action) && action->valuedouble>=0 && action->valuedouble<=2 && action->valuedouble==action->valueint && revision;
    if(valid){request.setup.mode=mode->valueint;request.setup.group_action=action->valueint;snprintf(request.revision,sizeof(request.revision),"%s",revision);}
    struct {const char *key;char *out;size_t capacity;} strings[]={
        {"ssid",request.setup.ssid,sizeof(request.setup.ssid)}, {"password",request.setup.password,sizeof(request.setup.password)},
        {"time_source",request.setup.time_source,sizeof(request.setup.time_source)}, {"local_datetime",request.setup.local_datetime,sizeof(request.setup.local_datetime)}};
    for(size_t i=0;i<sizeof(strings)/sizeof(strings[0]);++i){
        const char *value=json_string(obj,strings[i].key,strings[i].capacity-1);
        if(!value)valid=false;else memcpy(strings[i].out,value,strlen(value)+1);
    }
    if(valid && request.setup.mode!=2 && request.setup.group_action==2)
        valid=setup_hex(json_string(obj,"group",32),request.setup.group,16) && setup_hex(json_string(obj,"secret",64),request.setup.secret,32);
    wipe_json(obj);cJSON_Delete(obj);
    KatmedConfigResponse response={0};
    KatmedConfigStatus status=valid ? katmed_config_exchange(&request,&response,BRIDGE_TIMEOUT_MS):KATMED_CONFIG_INVALID;
    wipe(&request,sizeof(request));
    if(status!=KATMED_CONFIG_OK)return bridge_error(req,status);
    cJSON *reply=cJSON_CreateObject();cJSON_AddBoolToObject(reply,"saved",true);cJSON_AddBoolToObject(reply,"connection_will_close",true);
    return json_reply(req,"200 OK",reply);
}

static void utc_json(cJSON *object,const char *key,int64_t milliseconds) {
    char text[24];struct tm utc;time_t seconds=milliseconds/1000;
    if(milliseconds>0 && gmtime_r(&seconds,&utc) && strftime(text,sizeof(text),"%Y-%m-%dT%H:%M:%SZ",&utc))cJSON_AddStringToObject(object,key,text);
    else cJSON_AddNullToObject(object,key);
}
static esp_err_t ha_status_get(httpd_req_t *req) {
    char token[KATMED_WEB_TOKEN_HEX+1];if(!require_session(req,false,token))return ESP_OK;
    KatmedConfigRequest request={.operation=KATMED_CONFIG_HA_GET};KatmedConfigResponse response={0};
    KatmedConfigStatus status=katmed_config_exchange(&request,&response,BRIDGE_TIMEOUT_MS);
    if(status!=KATMED_CONFIG_OK)return bridge_error(req,status);
    static const char *severity[]={"none","blue","yellow","red"};
    if(response.ha.alarm_severity>3)return error_reply(req,"503 Service Unavailable","invalid_snapshot");
    cJSON *obj=cJSON_CreateObject();cJSON_AddNumberToObject(obj,"schema_version",1);
    cJSON_AddStringToObject(obj,"device_id",response.ha.device_id);cJSON_AddStringToObject(obj,"boot_id",response.ha.boot_id);
    cJSON_AddBoolToObject(obj,"alarm_active",response.ha.alarm_active);cJSON_AddBoolToObject(obj,"boot_notice",response.ha.boot_notice);
    cJSON_AddStringToObject(obj,"alarm_severity",severity[response.ha.alarm_severity]);
    utc_json(obj,"next_due",response.ha.next_due_ms);utc_json(obj,"last_dose",response.ha.last_dose_ms);
    cJSON_AddBoolToObject(obj,"last_dose_known",response.ha.last_dose_known);cJSON_AddNumberToObject(obj,"emergency_button_sequence",response.ha.emergency_button_sequence);
    return json_reply(req,"200 OK",obj);
}

static KatmedConfigField config_field(const char *name, bool *found)
{
    static const struct { const char *name; KatmedConfigField field; } fields[] = {
        {"medication_hours", KATMED_CONFIG_MEDICATION_HOURS},
        {"image_hours", KATMED_CONFIG_IMAGE_HOURS},
        {"next_due_hhmm", KATMED_CONFIG_NEXT_DUE_HHMM},
        {"clock_style", KATMED_CONFIG_CLOCK_STYLE},
        {"orientation", KATMED_CONFIG_ORIENTATION},
        {"rotation", KATMED_CONFIG_ROTATION},
        {"brightness",KATMED_CONFIG_BRIGHTNESS},{"night_enabled",KATMED_CONFIG_NIGHT_ENABLED},
        {"night_start",KATMED_CONFIG_NIGHT_START},{"night_end",KATMED_CONFIG_NIGHT_END},
        {"copy_night",KATMED_CONFIG_COPY_NIGHT},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
        if (strcmp(name, fields[i].name) == 0) {
            *found = true;
            return fields[i].field;
        }
    *found = false;
    return KATMED_CONFIG_MEDICATION_HOURS;
}

static bool copy_bounded(char *dst, size_t cap, const char *source)
{
    if (!source) return false;
    size_t len = strnlen(source, cap);
    if (len >= cap) return false;
    memcpy(dst, source, len + 1);
    return true;
}

static esp_err_t config_put(httpd_req_t *req)
{
    char token[KATMED_WEB_TOKEN_HEX + 1];
    if (!require_session(req, true, token)) return ESP_OK;
    cJSON *obj = read_json(req);
    const char *const keys[] = {"field", "value", "revision", "preview_token"};
    const char *field_name = json_string(obj, "field", 32);
    const char *revision = json_string(obj, "revision", KATMED_CONFIG_REVISION_MAX - 1);
    const char *preview_token = json_string(obj, "preview_token",
                                            KATMED_CONFIG_PREVIEW_TOKEN_MAX - 1);
    bool found = false;
    KatmedConfigField field = field_name ? config_field(field_name, &found) :
                                             KATMED_CONFIG_MEDICATION_HOURS;
    char value[KATMED_CONFIG_VALUE_MAX] = {0};
    const cJSON *raw_value = cJSON_GetObjectItemCaseSensitive(obj, "value");
    bool value_valid = false;
    if (field == KATMED_CONFIG_ORIENTATION && cJSON_IsNumber(raw_value) &&
        raw_value->valuedouble == raw_value->valueint) {
        int degrees = raw_value->valueint;
        if (degrees == 0 || degrees == 90 || degrees == 180 || degrees == 270) {
            snprintf(value, sizeof(value), "%d", degrees);
            value_valid = true;
        }
    } else if (cJSON_IsString(raw_value)) {
        value_valid = copy_bounded(value, sizeof(value), raw_value->valuestring);
    }
    if (!keys_only(obj, keys, 4) || !revision || !found || !value_valid ||
        (field == KATMED_CONFIG_NEXT_DUE_HHMM && !preview_token) ||
        (field != KATMED_CONFIG_NEXT_DUE_HHMM && preview_token)) {
        if (obj) { wipe_json(obj); cJSON_Delete(obj); }
        return error_reply(req, "422 Unprocessable Entity", "invalid");
    }
    KatmedConfigRequest request = {.operation = KATMED_CONFIG_SAVE, .field = field};
    bool copied = copy_bounded(request.value, sizeof(request.value), value) &&
                  copy_bounded(request.revision, sizeof(request.revision), revision) &&
                  (!preview_token || copy_bounded(request.preview_token,
                                                  sizeof(request.preview_token), preview_token));
    wipe_json(obj);
    cJSON_Delete(obj);
    if (!copied) return error_reply(req, "422 Unprocessable Entity", "invalid");
    KatmedConfigResponse response = {0};
    KatmedConfigStatus status = katmed_config_exchange(&request, &response,
                                                       BRIDGE_TIMEOUT_MS);
    if (status != KATMED_CONFIG_OK || response.status != KATMED_CONFIG_OK)
        return bridge_error(req, status != KATMED_CONFIG_OK ? status : response.status);
    return json_reply(req, "200 OK", config_object(&response.snapshot));
}

static void local_datetime_text(const LocalDateTime *value, char out[48])
{
    snprintf(out, 48, "%04d-%02u-%02u %02u:%02u",
             value->year, value->month, value->day, value->hour, value->minute);
}

static esp_err_t preview_post(httpd_req_t *req)
{
    char token[KATMED_WEB_TOKEN_HEX + 1];
    if (!require_session(req, true, token)) return ESP_OK;
    cJSON *obj = read_json(req);
    const char *const keys[] = {"time", "revision"};
    const char *time = json_string(obj, "time", 5);
    const char *revision = json_string(obj, "revision", KATMED_CONFIG_REVISION_MAX - 1);
    if (!keys_only(obj, keys, 2) || !time || !revision || strlen(time) != 5) {
        if (obj) { wipe_json(obj); cJSON_Delete(obj); }
        return error_reply(req, "422 Unprocessable Entity", "invalid");
    }
    KatmedConfigRequest request = {.operation = KATMED_CONFIG_PREVIEW_TIME,
                                    .field = KATMED_CONFIG_NEXT_DUE_HHMM};
    bool copied = copy_bounded(request.value, sizeof(request.value), time) &&
                  copy_bounded(request.revision, sizeof(request.revision), revision);
    wipe_json(obj);
    cJSON_Delete(obj);
    if (!copied) return error_reply(req, "422 Unprocessable Entity", "invalid");
    KatmedConfigResponse response = {0};
    KatmedConfigStatus status = katmed_config_exchange(&request, &response,
                                                       BRIDGE_TIMEOUT_MS);
    if (status != KATMED_CONFIG_OK || response.status != KATMED_CONFIG_OK)
        return bridge_error(req, status != KATMED_CONFIG_OK ? status : response.status);
    cJSON *result = cJSON_CreateObject();
    if (!result) return error_reply(req, "503 Service Unavailable", "memory");
    char regular[48], effective[48];
    local_datetime_text(&response.preview.regular_local, regular);
    local_datetime_text(&response.preview.effective_local, effective);
    cJSON_AddStringToObject(result, "time", response.preview.time);
    cJSON_AddStringToObject(result, "regular_local", regular);
    cJSON_AddStringToObject(result, "effective_local", effective);
    cJSON_AddBoolToObject(result, "night_advanced", response.preview.night_advanced);
    cJSON_AddBoolToObject(result, "skipped_nonexistent_time",
                          response.preview.skipped_nonexistent_time);
    cJSON_AddStringToObject(result, "preview_token", response.preview.preview_token);
    return json_reply(req, "200 OK", result);
}

static esp_err_t redirect_get(httpd_req_t *req)
{
    char host[INET_ADDRSTRLEN];
    if (!web.https || !request_host(req, host))
        return error_reply(req, "403 Forbidden", "host");
    char location[64];
    snprintf(location, sizeof(location), "https://%s/", host);
    httpd_resp_set_status(req, "308 Permanent Redirect");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static bool register_routes(httpd_handle_t server)
{
    static const struct {
        const char *path;
        int method;
        esp_err_t (*handler)(httpd_req_t *);
    } routes[] = {
        {"/", HTTP_GET, page_get},
        {"/style.css", HTTP_GET, style_get},
        {"/app.js", HTTP_GET, script_get},
        {"/i18n.js", HTTP_GET, language_script_get},
        {"/api/language", HTTP_GET, language_get},
        {"/api/language", HTTP_PUT, language_put},
        {"/api/bootstrap", HTTP_GET, bootstrap_get},
        {"/api/info", HTTP_GET, info_get},
        {"/api/setup", HTTP_POST, setup_post},
        {"/api/login", HTTP_POST, login_post},
        {"/api/logout", HTTP_POST, logout_post},
        {"/api/runtime-setup", HTTP_GET, runtime_setup_get},
        {"/api/runtime-setup", HTTP_PUT, runtime_setup_put},
        {"/api/runtime-setup/exit", HTTP_POST, runtime_setup_exit_post},
        {"/api/ha/status", HTTP_GET, ha_status_get},
        {"/api/config", HTTP_GET, config_get},
        {"/api/config", HTTP_PUT, config_put},
        {"/api/config/preview-time", HTTP_POST, preview_post},
        {"/api/client-cert", HTTP_GET, client_cert_get},
        {"/api/client-cert", HTTP_POST, client_cert_post},
        {"/api/client-cert", HTTP_DELETE, client_cert_delete},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        httpd_uri_t uri = {.uri = routes[i].path, .method = routes[i].method,
                           .handler = routes[i].handler, .user_ctx = NULL};
        if (httpd_register_uri_handler(server, &uri) != ESP_OK) return false;
    }
    KatmedWebWifiServices wifi = web.services.wifi;
    wifi.context = web.services.context;
    wifi.authorize = module_authorize;
    KatmedWebResetServices reset = web.services.reset;
    reset.context = web.services.context;
    reset.authorize = module_authorize;
    const KatmedWebMediaServices media = {
        .context = web.services.context,
        .authorize = module_authorize,
    };
    return katmed_web_wifi_register(server, &wifi) &&
           katmed_web_reset_register(server, &reset) &&
           katmed_web_media_register(server, &media);
}

static bool start_listeners(void)
{
    web_socket_diag_sample(monotonic_ms(), true);
    if (!resources_allow(false)) {
        ESP_LOGW(WEB_TAG, "HTTPS resource admission rejected");
        return false;
    }
    if (web.https || web.redirect || web.tls_key_id != PSA_KEY_ID_NULL) {
        ESP_LOGE(WEB_TAG, "HTTPS listener from prior lifecycle remains active");
        return false;
    }
    httpd_ssl_config_t tls = HTTPD_SSL_CONFIG_DEFAULT();
    tls.httpd.max_open_sockets = 1;
    /* 21 local routes plus Wi-Fi(4), reset(3) and media(5). */
    tls.httpd.max_uri_handlers = 33;
    tls.httpd.backlog_conn = 1;
    tls.httpd.lru_purge_enable = true;
    tls.httpd.recv_wait_timeout = 2;
    tls.httpd.send_wait_timeout = 2;
    tls.tls_handshake_timeout_ms = 4000;
    tls.servercert = web.identity.cert;
    tls.servercert_len = web.identity.cert_len;
    tls.user_cb = tls_session_callback;
    if (web.auth.record.cert_len) {
        tls.cacert_pem = web.auth.record.client_cert;
        tls.cacert_len = web.auth.record.cert_len;
#ifdef CONFIG_ESP_TLS_SERVER_MIN_AUTH_MODE_OPTIONAL
        tls.client_cert_authmode_optional = true;
#else
        ESP_LOGE(WEB_TAG, "optional client certificate verification unavailable");
        return false;
#endif
    }
    if (web.identity.key_len == 32) {
        /* New identities store a raw P-256 scalar. prvtkey_pem is parsed as
         * PEM/DER by esp-tls, so provide its supported PSA key reference. */
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&attributes, 256);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH);
        psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
        psa_status_t status = psa_crypto_init();
        psa_key_id_t imported_key = PSA_KEY_ID_NULL;
        if (status == PSA_SUCCESS)
            status = psa_import_key(&attributes, web.identity.key, web.identity.key_len,
                                    &imported_key);
        psa_reset_key_attributes(&attributes);
        if (status != PSA_SUCCESS) {
            ESP_LOGE(WEB_TAG, "HTTPS server key import failed");
            return false;
        }
        web.tls_key_id = imported_key;
        web.tls_key_config.source = ESP_KEY_SOURCE_PSA;
        web.tls_key_config.psa.key_id = web.tls_key_id;
        tls.server_key = &web.tls_key_config;
    } else {
        /* Existing DER identities retain their original parse path. */
        tls.prvtkey_pem = web.identity.key;
        tls.prvtkey_len = web.identity.key_len;
    }
    if (httpd_ssl_start(&web.https, &tls) != ESP_OK || !web.https) {
        stop_listeners();
        ESP_LOGE(WEB_TAG, "HTTPS server start failed");
        return false;
    }
    if (!resources_allow(true) || !register_routes(web.https)) {
        ESP_LOGW(WEB_TAG, "HTTPS post-start admission or route registration failed");
        stop_listeners();
        return false;
    }
    httpd_config_t plain = HTTPD_DEFAULT_CONFIG();
    plain.server_port = 80;
    plain.ctrl_port = tls.httpd.ctrl_port + 1;
    plain.max_open_sockets = 1;
    plain.max_uri_handlers = 2;
    plain.stack_size = 3072;
    plain.backlog_conn = 1;
    plain.recv_wait_timeout = 2;
    plain.send_wait_timeout = 2;
    plain.uri_match_fn = katmed_web_plain_uri_match;
    if (httpd_start(&web.redirect, &plain) != ESP_OK || !web.redirect) {
        ESP_LOGW(WEB_TAG, "HTTP redirect start failed");
        stop_listeners();
        return false;
    }
    httpd_uri_t redirect = {.uri = "/*", .method = HTTP_GET,
                            .handler = redirect_get, .user_ctx = NULL};
    if (httpd_register_uri_handler(web.redirect, &redirect) != ESP_OK) {
        stop_listeners();
        return false;
    }
    if (!resources_allow(true)) {
        stop_listeners();
        return false;
    }
    web.started = true;
    web_socket_diag_sample(monotonic_ms(), true);
    return true;
}

static bool management_enabled=true;
void katmed_web_set_enabled(bool enabled) {
    management_enabled=enabled;
    if (!enabled) katmed_web_stop();
}

bool katmed_web_start(const KatmedWebServices *services)
{
    if (!management_enabled) return false;
    if (services) web.services = *services;
    if (web.started || web.nvs_open) return web.started;
    if (!resources_allow(false)) {
        ESP_LOGW(WEB_TAG, "HTTPS resource admission rejected");
        return false;
    }
    /* Fresh devices reach Web before any provisioned Mesh/Wi-Fi startup. */
    if (esp_netif_init() != ESP_OK) {
        ESP_LOGE(WEB_TAG, "TCP/IP initialization failed");
        return false;
    }
    web.certificate_fd = -1;
    if (nvs_open("katmed_web", NVS_READWRITE, &web.nvs) != ESP_OK) {
        ESP_LOGE(WEB_TAG, "web security storage unavailable");
        return false;
    }
    web.nvs_open = true;
    if (!load_auth(&web) ||
        !katmed_web_identity_load_or_create(web.nvs, &web.identity)) {
        ESP_LOGE(WEB_TAG, "web security identity unavailable");
        katmed_web_stop();
        return false;
    }
    web.auth_mutex = xSemaphoreCreateMutex();
    if (!web.auth_mutex || !start_listeners()) {
        katmed_web_stop();
        return false;
    }
    ESP_LOGI(WEB_TAG, "HTTPS configuration listener ready");
    return true;
}

void katmed_web_poll(uint64_t now_ms)
{
    web_socket_diag_sample(now_ms, false);
    if (!management_enabled) return;
    if (!web.started && !web.https && !web.redirect &&
        web.tls_key_id == PSA_KEY_ID_NULL && now_ms >= web.retry_due_ms) {
        bool ready = web.nvs_open ? start_listeners() : katmed_web_start(&web.services);
        if (!ready) web.retry_due_ms = now_ms + 30000;
    }
    if (!web.nvs_open) return;
    if (web.auth_mutex && xSemaphoreTake(web.auth_mutex, 0) == pdTRUE) {
        katmed_web_auth_tick(&web.auth, now_ms);
        xSemaphoreGive(web.auth_mutex);
    }
    if (!web.restart_pending || now_ms < web.restart_due_ms) return;
    if (web.services.prepare_listener_stop &&
        !web.services.prepare_listener_stop(web.services.context)) return;
    if (!stop_listeners()) {
        web.restart_pending = false;
        ESP_LOGE(WEB_TAG, "HTTPS listener restart blocked by failed stop");
        return;
    }
    web.restart_pending = false;
    if (!start_listeners()) {
        web.retry_due_ms = now_ms + 30000;
        ESP_LOGE(WEB_TAG, "HTTPS listener restart failed");
    }
}

bool katmed_web_pairing_code(char out[7])
{
    if (!out || !web.nvs_open || !web.auth_mutex ||
        xSemaphoreTake(web.auth_mutex, 0) != pdTRUE) return false;
    const char *code = katmed_web_auth_display_code(&web.auth);
    bool visible = code != NULL;
    if (visible) memcpy(out, code, 7);
    else out[0] = '\0';
    xSemaphoreGive(web.auth_mutex);
    return visible;
}

void katmed_web_stop(void)
{
    if (web.services.prepare_listener_stop &&
        !web.services.prepare_listener_stop(web.services.context)) return;
    if (!stop_listeners()) return;
    if (web.auth_mutex) {
        vSemaphoreDelete(web.auth_mutex);
        web.auth_mutex = NULL;
    }
    if (web.nvs_open) {
        nvs_close(web.nvs);
        web.nvs_open = false;
    }
    wipe(&web.auth, sizeof(web.auth));
    wipe(&web.identity, sizeof(web.identity));
    web.restart_pending = false;
}

httpd_handle_t katmed_web_acme_http_server(void)
{
    return web.started ? web.redirect : NULL;
}
