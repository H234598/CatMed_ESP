#define main validator_fixture_main
#include "check_validation.c"
#undef main
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include "cJSON.h"
#include "katmed_web_auth.h"
#include "katmed_web_policy.h"

#define BODY_MAX 8192u
#define BRIDGE_TIMEOUT_MS 1500u
#define portMAX_DELAY 1
#define pdTRUE 1
#define ESP_LOGE(...) ((void)0)
typedef struct { int operation; } KatmedConfigRequest;
typedef struct { int status; int64_t security_utc_ms; } KatmedConfigResponse;
#define KATMED_CONFIG_SECURITY_TIME 1
#define KATMED_CONFIG_OK 0
typedef struct { void *context; bool (*prepare_listener_stop)(void *); } Services;
typedef struct {
    int nvs; bool nvs_open, started, restart_pending, identifier_ready, server_quarantined;
    uint64_t restart_due_ms, retry_due_ms, server_probe_deadline_ms; unsigned server_pending_slot;
    char acme_identifier[254], request_body[BODY_MAX + 1];
    void *auth_mutex, *https, *redirect; int tls_key_id;
    KatmedWebAuth auth; Services services;
    KatmedServerRecord *server_identity, *server_pending, *server_previous;
    const char *server_result;
} TestWeb;
static TestWeb web;
static atomic_bool server_probe_active, server_probe_seen;
static pthread_mutex_t host_auth_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t race_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t race_condition = PTHREAD_COND_INITIALIZER;
static pthread_t anchor_updater_thread;
static bool anchor_updater_thread_ready, validation_paused, validation_release;
static bool anchor_update_go, anchor_update_waiting, anchor_update_acquired;
static bool anchor_update_done, anchor_race_enabled;
typedef struct {
    void *handle; const char *host, *origin, *csrf, *cookie, *body, *type;
    size_t content_len, offset; char status[40], response[128];
} httpd_req_t;
static bool management_enabled = true, fail_listener_once, synthetic_handshake_enabled = true;
static int64_t security_now;
static uint64_t monotonic_ms(void) { return 1000; }
static void wipe(void *p, size_t n) { memset(p, 0, n); }
static int xSemaphoreTake(void *mutex, int timeout)
{
    (void)mutex;
    (void)timeout;
    pthread_mutex_lock(&race_mutex);
    bool updater = anchor_updater_thread_ready && pthread_equal(pthread_self(), anchor_updater_thread);
    pthread_mutex_unlock(&race_mutex);
    if (updater) {
        int attempt = pthread_mutex_trylock(&host_auth_mutex);
        pthread_mutex_lock(&race_mutex);
        anchor_update_waiting = true;
        anchor_update_acquired = attempt == 0;
        pthread_cond_broadcast(&race_condition);
        pthread_mutex_unlock(&race_mutex);
        if (attempt == 0) return pdTRUE;
    }
    return pthread_mutex_lock(&host_auth_mutex) == 0 ? pdTRUE : 0;
}
static void xSemaphoreGive(void *mutex) { (void)mutex; pthread_mutex_unlock(&host_auth_mutex); }
void katmed_web_test_anchor_validation_pause(void)
{
    pthread_mutex_lock(&race_mutex);
    if (!anchor_race_enabled) { pthread_mutex_unlock(&race_mutex); return; }
    validation_paused = true;
    pthread_cond_broadcast(&race_condition);
    while (!validation_release) pthread_cond_wait(&race_condition, &race_mutex);
    pthread_mutex_unlock(&race_mutex);
}
static void web_socket_diag_sample(uint64_t now, bool force) { (void)now; (void)force; }
static int katmed_config_exchange(const KatmedConfigRequest *req, KatmedConfigResponse *res, int timeout)
{ (void)req; (void)timeout; res->status = security_now ? 0 : 1; res->security_utc_ms = security_now * 1000; return 0; }
static int httpd_req_to_sockfd(httpd_req_t *req) { (void)req; return 7; }
static int local_getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    (void)fd; struct sockaddr_in local = {.sin_family = AF_INET};
    inet_pton(AF_INET, "192.0.2.42", &local.sin_addr); memcpy(addr, &local, sizeof(local));
    *len = sizeof(local); return 0;
}
#define getsockname local_getsockname
static const char *header(httpd_req_t *r, const char *key)
{
    if (!strcmp(key,"Host")) return r->host;
    if (!strcmp(key,"Origin")) return r->origin;
    if (!strcmp(key,"Cookie")) return r->cookie;
    if (!strcmp(key,"X-Katmed-CSRF")) return r->csrf;
    if (!strcmp(key,"Content-Type")) return r->type;
    return NULL;
}
static size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *key)
{ const char *s=header(r,key); return s ? strlen(s) : 0; }
static int httpd_req_get_hdr_value_str(httpd_req_t *r,const char *key,char *out,size_t len)
{ const char *s=header(r,key); if(!s || strlen(s)>=len) return -1; strcpy(out,s); return ESP_OK; }
static int httpd_req_recv(httpd_req_t *r, char *out, size_t len)
{ if(len > r->content_len-r->offset) len=r->content_len-r->offset; memcpy(out,r->body+r->offset,len); r->offset+=len; return (int)len; }
static int error_reply(httpd_req_t *r,const char *status,const char *message)
{ snprintf(r->status,sizeof(r->status),"%s",status); snprintf(r->response,sizeof(r->response),"%s",message); return 0; }
static int reply(httpd_req_t *r,const char *status,const char *body)
{ snprintf(r->status,sizeof(r->status),"%s",status); snprintf(r->response,sizeof(r->response),"%s",body); return 0; }
static bool stop_listeners(void) { web.started=false; web.https=NULL; web.redirect=NULL; return true; }
static int start_listeners(void)
{
    if (fail_listener_once) { fail_listener_once=false; return -1; }
    if (web.server_identity && !handshake(web.server_identity->key,
        (unsigned char *)web.server_identity->chain, web.server_identity->chain_len,
        blobs[3].bytes, blobs[3].len, web.server_identity->dns)) return -1;
    if (synthetic_handshake_enabled && atomic_load_explicit(&server_probe_active, memory_order_acquire))
        atomic_store_explicit(&server_probe_seen, true, memory_order_release);
    web.https=(void *)1; web.redirect=(void *)2; web.started=true; return 0;
}
static int katmed_web_start(const Services *s) { (void)s; return start_listeners(); }
static bool synthetic_derive(void *ctx,const char *password,const uint8_t salt[16],uint8_t out[32])
{ (void)ctx; (void)salt; memset(out,0,32); out[0]=!strcmp(password,"synthetic-password") ? 7 : 8; return true; }
