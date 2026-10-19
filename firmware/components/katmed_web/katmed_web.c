#include "katmed_web.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_https_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "katmed_config_bridge.h"
#include "katmed_web_auth.h"
#include "katmed_web_identity.h"
#include "katmed_web_policy.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern const unsigned char _binary_index_html_start[];
extern const unsigned char _binary_index_html_end[];
extern const unsigned char _binary_style_css_start[];
extern const unsigned char _binary_style_css_end[];
extern const unsigned char _binary_app_js_start[];
extern const unsigned char _binary_app_js_end[];

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
    char request_body[BODY_MAX + 1];
    SemaphoreHandle_t auth_mutex;
} WebContext;

static WebContext web = {.certificate_fd = -1};

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
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
        (const unsigned char *)password, strlen(password), salt, 16,
        KATMED_WEB_PBKDF2_ROUNDS, 32, out) == 0;
}

static bool commit_auth(void *context, const KatmedWebAuthRecord *record)
{
    WebContext *w = context;
    return w->nvs_open && nvs_set_blob(w->nvs, "auth", record, sizeof(*record)) == ESP_OK &&
           nvs_commit(w->nvs) == ESP_OK;
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
    if (mbedtls_sha256(peer->raw.p, peer->raw.len,
                           web.certificate_fingerprint, 0) != 0) return;
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

static void stop_listeners(void)
{
    if (web.redirect) {
        httpd_stop(web.redirect);
        web.redirect = NULL;
    }
    if (web.https) {
        httpd_ssl_stop(web.https);
        web.https = NULL;
    }
    web.started = false;
    web.certificate_fd = -1;
    web.certificate_verified = false;
    wipe(web.certificate_fingerprint, sizeof(web.certificate_fingerprint));
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
    struct sockaddr_in local = {0};
    socklen_t length = sizeof(local);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&local, &length) != 0 ||
        local.sin_family != AF_INET ||
        !inet_ntop(AF_INET, &local.sin_addr, host, INET_ADDRSTRLEN)) return false;
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

static esp_err_t asset_reply(httpd_req_t *req, const unsigned char *start,
                              const unsigned char *end, const char *type)
{
    char host[INET_ADDRSTRLEN];
    if (!request_host(req, host)) return error_reply(req, "403 Forbidden", "host");
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
        "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
        "img-src 'self'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'");
    return httpd_resp_send(req, (const char *)start, (ssize_t)(end - start));
}

static esp_err_t page_get(httpd_req_t *req)
{
    return asset_reply(req, _binary_index_html_start, _binary_index_html_end,
                       "text/html; charset=utf-8");
}
static esp_err_t style_get(httpd_req_t *req)
{
    return asset_reply(req, _binary_style_css_start, _binary_style_css_end,
                       "text/css; charset=utf-8");
}
static esp_err_t script_get(httpd_req_t *req)
{
    return asset_reply(req, _binary_app_js_start, _binary_app_js_end,
                       "application/javascript; charset=utf-8");
}

static cJSON *bootstrap_object(httpd_req_t *req, KatmedWebSession *session)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;
    bool setup = katmed_web_auth_setup_required(&web.auth);
    cJSON_AddBoolToObject(obj, "setup_required", setup);
    cJSON_AddBoolToObject(obj, "authenticated", session != NULL && !setup);
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
        mbedtls_pk_get_type(&cert.pk) != MBEDTLS_PK_ECKEY ||
        mbedtls_pk_get_bitlen(&cert.pk) != 256 ||
        !cert.ext_key_usage.buf.p ||
        mbedtls_x509_crt_check_key_usage(&cert, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0 ||
        mbedtls_x509_crt_check_extended_key_usage(&cert, MBEDTLS_OID_CLIENT_AUTH,
                                                  MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH)) != 0 ||
        mbedtls_x509_crt_verify(&cert, &cert, NULL, NULL, &flags, NULL, NULL) != 0 ||
        flags != 0 ||
        mbedtls_sha256(cert.raw.p, cert.raw.len, fingerprint, 0) != 0)
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

static KatmedConfigField config_field(const char *name, bool *found)
{
    static const struct { const char *name; KatmedConfigField field; } fields[] = {
        {"medication_hours", KATMED_CONFIG_MEDICATION_HOURS},
        {"image_hours", KATMED_CONFIG_IMAGE_HOURS},
        {"next_due_hhmm", KATMED_CONFIG_NEXT_DUE_HHMM},
        {"clock_style", KATMED_CONFIG_CLOCK_STYLE},
        {"orientation", KATMED_CONFIG_ORIENTATION},
        {"rotation", KATMED_CONFIG_ROTATION},
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
        {"/api/bootstrap", HTTP_GET, bootstrap_get},
        {"/api/setup", HTTP_POST, setup_post},
        {"/api/login", HTTP_POST, login_post},
        {"/api/logout", HTTP_POST, logout_post},
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
    return true;
}

static bool start_listeners(void)
{
    if (!resources_allow(false)) {
        ESP_LOGW(WEB_TAG, "HTTPS resource admission rejected");
        return false;
    }
    httpd_ssl_config_t tls = HTTPD_SSL_CONFIG_DEFAULT();
    tls.httpd.max_open_sockets = 1;
    tls.httpd.max_uri_handlers = 14;
    tls.httpd.backlog_conn = 1;
    tls.httpd.lru_purge_enable = true;
    tls.httpd.recv_wait_timeout = 2;
    tls.httpd.send_wait_timeout = 2;
    tls.tls_handshake_timeout_ms = 4000;
    tls.servercert = web.identity.cert;
    tls.servercert_len = web.identity.cert_len;
    tls.prvtkey_pem = web.identity.key;
    tls.prvtkey_len = web.identity.key_len;
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
    if (httpd_ssl_start(&web.https, &tls) != ESP_OK || !web.https) {
        web.https = NULL;
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
    plain.max_uri_handlers = 1;
    plain.stack_size = 3072;
    plain.backlog_conn = 1;
    plain.recv_wait_timeout = 2;
    plain.send_wait_timeout = 2;
    plain.uri_match_fn = httpd_uri_match_wildcard;
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
    return true;
}

bool katmed_web_start(void)
{
    if (web.started || web.nvs_open) return web.started;
    if (!resources_allow(false)) {
        ESP_LOGW(WEB_TAG, "HTTPS resource admission rejected");
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
    if (!web.started && now_ms >= web.retry_due_ms) {
        bool ready = web.nvs_open ? start_listeners() : katmed_web_start();
        if (!ready) web.retry_due_ms = now_ms + 30000;
    }
    if (!web.nvs_open) return;
    if (web.auth_mutex && xSemaphoreTake(web.auth_mutex, 0) == pdTRUE) {
        katmed_web_auth_tick(&web.auth, now_ms);
        xSemaphoreGive(web.auth_mutex);
    }
    if (!web.restart_pending || now_ms < web.restart_due_ms) return;
    web.restart_pending = false;
    stop_listeners();
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
    stop_listeners();
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
