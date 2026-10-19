#include "katmed_web_media.h"
#include "katmed_image_store.h"
#include "esp_err.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define ROUTE_CAPACITY 4u
#define IMAGE_CAPACITY 8u

typedef struct {
    httpd_uri_t routes[ROUTE_CAPACITY];
    size_t count;
} FakeServer;

typedef struct {
    KatmedUserImageInfo info;
    char bytes[64];
} FakeImage;

static struct {
    FakeImage images[IMAGE_CAPACITY];
    uint32_t image_count;
    uint32_t next_id;
    bool full;
    bool write_fail;
    bool pending;
    char pending_bytes[64];
    size_t pending_size;
    size_t expected;
    unsigned begin_count;
    unsigned write_count;
    unsigned commit_count;
    unsigned abort_count;
} store;

static void response_append(httpd_req_t *req, const char *data, ssize_t length)
{
    size_t amount = length == HTTPD_RESP_USE_STRLEN ? strlen(data) : (size_t)length;
    assert(req->response_size + amount < sizeof(req->response));
    memcpy(req->response + req->response_size, data, amount);
    req->response_size += amount;
    req->response[req->response_size] = '\0';
}

esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    (void)snprintf(req->response_status, sizeof(req->response_status), "%s", status);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{ (void)req; (void)type; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value)
{
    if (strcmp(name, "Connection") == 0 && strcmp(value, "close") == 0)
        req->connection_close = true;
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *req, const char *data, ssize_t length)
{
    if (data) response_append(req, data, length);
    req->response_complete = true;
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *data, ssize_t length)
{
    if (!data && length == 0) req->response_complete = true;
    else response_append(req, data, length);
    return ESP_OK;
}
int httpd_req_recv(httpd_req_t *req, char *buffer, size_t wanted)
{
    if (req->body_offset >= req->fail_after || req->body_offset >= req->body_size)
        return -1;
    size_t amount = req->body_size - req->body_offset;
    if (amount > wanted) amount = wanted;
    if (req->body_offset + amount > req->fail_after)
        amount = req->fail_after - req->body_offset;
    if (amount == 0) return -1;
    memcpy(buffer, req->body + req->body_offset, amount);
    req->body_offset += amount;
    return (int)amount;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name,
                                      char *value, size_t size)
{
    if (strcmp(name, "Content-Type") != 0 || !req->content_type) return ESP_ERR_NOT_FOUND;
    const size_t length = strlen(req->content_type);
    if (length + 1 > size) return ESP_ERR_HTTPD_RESULT_TRUNC;
    memcpy(value, req->content_type, length + 1);
    return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *route)
{
    FakeServer *server = handle;
    assert(server->count < ROUTE_CAPACITY);
    server->routes[server->count++] = *route;
    return ESP_OK;
}
esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *value, size_t size)
{
    if (!req->query || !req->query[0]) return ESP_ERR_NOT_FOUND;
    const size_t length = strlen(req->query);
    if (length + 1 > size) return ESP_ERR_HTTPD_RESULT_TRUNC;
    memcpy(value, req->query, length + 1);
    return ESP_OK;
}
esp_err_t httpd_query_key_value(const char *query, const char *key,
                               char *value, size_t size)
{
    const size_t key_length = strlen(key);
    const char *part = query;
    while (*part) {
        const char *end = strchr(part, '&');
        const size_t length = end ? (size_t)(end - part) : strlen(part);
        if (length > key_length && memcmp(part, key, key_length) == 0 &&
            part[key_length] == '=') {
            const size_t value_length = length - key_length - 1;
            if (value_length + 1 > size) return ESP_ERR_HTTPD_RESULT_TRUNC;
            memcpy(value, part + key_length + 1, value_length);
            value[value_length] = '\0';
            return ESP_OK;
        }
        if (!end) break;
        part = end + 1;
    }
    return ESP_ERR_NOT_FOUND;
}

KatmedImageStatus katmed_image_store_stats(KatmedImageStoreStats *out)
{
    out->total_bytes = 4096;
    out->used_bytes = store.image_count * 32u;
    out->free_bytes = out->total_bytes - out->used_bytes;
    out->image_count = store.image_count;
    return KATMED_IMAGE_OK;
}
KatmedImageStatus katmed_image_store_list(uint32_t index, KatmedUserImageInfo *out)
{
    if (index >= store.image_count) return KATMED_IMAGE_NOT_FOUND;
    *out = store.images[index].info;
    return KATMED_IMAGE_OK;
}
KatmedImageStatus katmed_image_store_upload_begin(uint32_t expected,
                                                   KatmedImageUpload *out)
{
    (void)out;
    store.begin_count++;
    if (store.full) return KATMED_IMAGE_NO_SPACE;
    store.pending = true;
    store.pending_size = 0;
    store.expected = expected;
    return KATMED_IMAGE_OK;
}
KatmedImageStatus katmed_image_store_upload_write(KatmedImageUpload upload,
                                                   const uint8_t *bytes,
                                                   size_t length)
{
    (void)upload;
    assert(store.pending);
    if (store.write_fail) return KATMED_IMAGE_NO_SPACE;
    assert(store.pending_size + length <= sizeof(store.pending_bytes));
    memcpy(store.pending_bytes + store.pending_size, bytes, length);
    store.pending_size += length;
    store.write_count++;
    return KATMED_IMAGE_OK;
}
KatmedImageStatus katmed_image_store_upload_commit(KatmedImageUpload upload,
                                                    KatmedUserImageInfo *out)
{
    (void)upload;
    store.commit_count++;
    if (!store.pending || store.pending_size != store.expected ||
        store.pending_size < 2 || memcmp(store.pending_bytes, "OK", 2) != 0)
        return KATMED_IMAGE_BAD_RANGE;
    if (store.image_count >= IMAGE_CAPACITY) return KATMED_IMAGE_NO_SPACE;
    FakeImage *image = &store.images[store.image_count++];
    image->info = (KatmedUserImageInfo){.id = ++store.next_id,
        .bytes = (uint32_t)store.pending_size, .width = 20, .height = 20};
    memcpy(image->bytes, store.pending_bytes, store.pending_size);
    *out = image->info;
    store.pending = false;
    return KATMED_IMAGE_OK;
}
KatmedImageStatus katmed_image_store_upload_abort(KatmedImageUpload upload)
{
    (void)upload;
    store.abort_count++;
    store.pending = false;
    store.pending_size = 0;
    return KATMED_IMAGE_OK;
}
KatmedImageStatus katmed_image_store_remove(uint32_t id)
{
    for (uint32_t i = 0; i < store.image_count; ++i) {
        if (store.images[i].info.id == id) {
            memmove(&store.images[i], &store.images[i + 1],
                    (store.image_count - i - 1) * sizeof(store.images[0]));
            store.image_count--;
            return KATMED_IMAGE_OK;
        }
    }
    return KATMED_IMAGE_NOT_FOUND;
}

static bool authorize(httpd_req_t *req, bool mutate, void *context)
{
    (void)context;
    if (!req->authenticated) {
        (void)httpd_resp_set_status(req, "401 Unauthorized");
        (void)httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    if (mutate && !req->csrf_ok) {
        (void)httpd_resp_set_status(req, "403 Forbidden");
        (void)httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    return true;
}

static httpd_uri_func route(FakeServer *server, httpd_method_t method)
{
    for (size_t i = 0; i < server->count; ++i)
        if (server->routes[i].method == method) return server->routes[i].handler;
    assert(false);
    return NULL;
}

static httpd_req_t request(httpd_method_t method, const char *content_type,
                           const char *query, const char *body, size_t fail_after)
{
    httpd_req_t req = {0};
    req.content_type = content_type;
    req.query = query;
    req.body = (const unsigned char *)body;
    req.body_size = body ? strlen(body) : 0;
    req.content_len = (int)req.body_size;
    req.fail_after = fail_after;
    req.authenticated = true;
    req.csrf_ok = true;
    (void)method;
    return req;
}

static void assert_old_image_preserved(FakeServer *server)
{
    httpd_req_t req = request(HTTP_GET, NULL, NULL, NULL, SIZE_MAX);
    assert(route(server, HTTP_GET)(&req) == ESP_OK);
    assert(strstr(req.response, "\"image_count\":1") != NULL);
    assert(strstr(req.response, "\"id\":1") != NULL);
    assert(store.image_count == 1 && strcmp(store.images[0].bytes, "OLD") == 0);
}

int main(void)
{
    FakeServer server = {0};
    store.images[0].info = (KatmedUserImageInfo){.id = 1, .bytes = 3,
        .width = 10, .height = 10};
    memcpy(store.images[0].bytes, "OLD", 4);
    store.image_count = 1;
    store.next_id = 1;
    KatmedWebMediaServices services = {.authorize = authorize};
    assert(katmed_web_media_register(&server, &services));
    assert(server.count == 3);

    httpd_req_t req = request(HTTP_POST, "image/jpeg", NULL, "OK-new", SIZE_MAX);
    req.authenticated = false;
    assert(route(&server, HTTP_POST)(&req) == ESP_OK);
    assert(strcmp(req.response_status, "401 Unauthorized") == 0);
    assert(store.begin_count == 0);
    req = request(HTTP_POST, "image/jpeg", NULL, "OK-new", SIZE_MAX);
    req.csrf_ok = false;
    assert(route(&server, HTTP_POST)(&req) == ESP_OK);
    assert(strcmp(req.response_status, "403 Forbidden") == 0);
    assert(store.begin_count == 0);

    req = request(HTTP_POST, "application/octet-stream", NULL, "OK-new", SIZE_MAX);
    assert(route(&server, HTTP_POST)(&req) == ESP_OK);
    assert(strcmp(req.response_status, "415 Unsupported Media Type") == 0);
    assert(req.connection_close && store.begin_count == 0);
    req = request(HTTP_POST, "image/jpeg", NULL, "x", SIZE_MAX);
    req.content_len = 131073;
    assert(route(&server, HTTP_POST)(&req) == ESP_OK);
    assert(strcmp(req.response_status, "413 Content Too Large") == 0);
    assert(req.connection_close && store.begin_count == 0);

    req = request(HTTP_POST, "image/jpeg", NULL, "OK-new", 2);
    assert(route(&server, HTTP_POST)(&req) == ESP_OK);
    assert(strcmp(req.response_status, "400 Bad Request") == 0);
    assert(store.abort_count == 1);
    assert_old_image_preserved(&server);

    char large_body[2048];
    memset(large_body, 'x', sizeof(large_body));
    large_body[0] = 'O';
    large_body[1] = 'K';
    req = request(HTTP_POST, "image/jpeg", NULL, large_body, SIZE_MAX);
    req.body_size = sizeof(large_body);
    req.content_len = (int)sizeof(large_body);
    store.write_fail = true;
    assert(route(&server, HTTP_POST)(&req) == ESP_OK);
    assert(strcmp(req.response_status, "507 Insufficient Storage") == 0);
    assert(req.connection_close && req.body_offset < req.body_size);
    assert(store.abort_count == 2);
    assert_old_image_preserved(&server);
    store.write_fail = false;

    req = request(HTTP_POST, "image/jpeg", NULL, "NO-new", SIZE_MAX);
    assert(route(&server, HTTP_POST)(&req) == ESP_OK);
    assert(strcmp(req.response_status, "422 Unprocessable Content") == 0);
    assert(store.abort_count == 3);
    assert_old_image_preserved(&server);

    store.full = true;
    req = request(HTTP_POST, "image/jpeg", NULL, "OK-new", SIZE_MAX);
    assert(route(&server, HTTP_POST)(&req) == ESP_OK);
    assert(strcmp(req.response_status, "507 Insufficient Storage") == 0);
    assert(req.connection_close);
    assert_old_image_preserved(&server);
    store.full = false;

    req = request(HTTP_POST, "image/jpeg", NULL, "OK-new", SIZE_MAX);
    assert(route(&server, HTTP_POST)(&req) == ESP_OK);
    assert(strcmp(req.response_status, "201 Created") == 0);
    assert(strstr(req.response, "\"id\":2") != NULL);
    assert(store.image_count == 2);

    req = request(HTTP_DELETE, NULL, "id=4294967296", NULL, SIZE_MAX);
    assert(route(&server, HTTP_DELETE)(&req) == ESP_OK);
    assert(strcmp(req.response_status, "400 Bad Request") == 0);
    req = request(HTTP_DELETE, NULL, "id=../secret", NULL, SIZE_MAX);
    assert(route(&server, HTTP_DELETE)(&req) == ESP_OK);
    assert(strcmp(req.response_status, "400 Bad Request") == 0);
    req = request(HTTP_DELETE, NULL, "id=2", NULL, SIZE_MAX);
    assert(route(&server, HTTP_DELETE)(&req) == ESP_OK);
    assert(strcmp(req.response_status, "200 OK") == 0);
    assert(store.image_count == 1 && strcmp(store.images[0].bytes, "OLD") == 0);

    FakeServer after_restart = {0};
    assert(katmed_web_media_register(&after_restart, &services));
    assert_old_image_preserved(&after_restart);
    return 0;
}
