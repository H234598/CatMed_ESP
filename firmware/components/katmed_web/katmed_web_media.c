#include "katmed_web_media.h"

#include "katmed_image_store.h"
#include "esp_err.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define MEDIA_PAGE_SIZE 16u
#define MEDIA_CHUNK_BYTES 1024u

static KatmedWebMediaServices media_services;

static esp_err_t media_reply(httpd_req_t *req, const char *status,
                             const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t media_error(httpd_req_t *req, const char *status,
                             const char *code, const char *message)
{
    char body[192];
    (void)snprintf(body, sizeof(body), "{\"error\":\"%s\",\"message\":\"%s\"}",
                   code, message);
    return media_reply(req, status, body);
}

static esp_err_t store_error(httpd_req_t *req, KatmedImageStatus status)
{
    switch (status) {
    case KATMED_IMAGE_BAD_RANGE:
        return media_error(req, "422 Unprocessable Content", "invalid_image",
                           "Das Bildformat oder die Bildgröße wird nicht unterstützt.");
    case KATMED_IMAGE_BAD_ID:
    case KATMED_IMAGE_NOT_FOUND:
        return media_error(req, "404 Not Found", "image_not_found",
                           "Das Bild wurde nicht gefunden.");
    case KATMED_IMAGE_UNAVAILABLE:
    case KATMED_IMAGE_NO_SPACE:
        return media_error(req, "507 Insufficient Storage", "storage_unavailable",
                           "Der Bildspeicher ist voll oder derzeit nicht verfügbar.");
    case KATMED_IMAGE_BUSY:
        return media_error(req, "503 Service Unavailable", "storage_busy",
                           "Der Bildspeicher bearbeitet gerade eine andere Anfrage.");
    case KATMED_IMAGE_BAD_STATE:
        return media_error(req, "503 Service Unavailable", "storage_unavailable",
                           "Der Bildspeicher ist derzeit nicht verfügbar.");
    case KATMED_IMAGE_IO_ERROR:
        return media_error(req, "507 Insufficient Storage", "storage_error",
                           "Das Bild konnte nicht sicher gespeichert werden.");
    default:
        return media_error(req, "500 Internal Server Error", "storage_error",
                           "Der Bildspeicher konnte die Anfrage nicht abschließen.");
    }
}

static bool authorize(httpd_req_t *req, bool mutate)
{
    return media_services.authorize &&
           media_services.authorize(req, mutate, media_services.context);
}

static bool query_uint32(httpd_req_t *req, const char *key, uint32_t *out,
                         bool required)
{
    char query[48];
    char value[16];
    uint32_t parsed = 0;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return !required;
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK)
        return !required;
    if (!value[0]) return false;
    for (const char *p = value; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const uint32_t digit = (uint32_t)(*p - '0');
        if (parsed > (UINT32_MAX - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    *out = parsed;
    return true;
}

static esp_err_t images_list(httpd_req_t *req)
{
    uint32_t offset = 0;
    KatmedImageStoreStats stats = {0};
    char header[192];
    char item[128];
    bool first = true;
    uint32_t end;

    if (!authorize(req, false)) return ESP_OK;
    if (!query_uint32(req, "offset", &offset, false))
        return media_error(req, "400 Bad Request", "invalid_offset",
                           "Die Bildliste konnte nicht geladen werden.");
    const KatmedImageStatus stats_status = katmed_image_store_stats(&stats);
    if (stats_status != KATMED_IMAGE_OK) return store_error(req, stats_status);
    if (offset > stats.image_count) offset = stats.image_count;
    end = offset + MEDIA_PAGE_SIZE;
    if (end > stats.image_count) end = stats.image_count;

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    (void)snprintf(header, sizeof(header),
                   "{\"used_bytes\":%" PRIu32 ",\"free_bytes\":%" PRIu32
                   ",\"image_count\":%" PRIu32 ",\"offset\":%" PRIu32
                   ",\"images\":[",
                   stats.used_bytes, stats.free_bytes, stats.image_count, offset);
    if (httpd_resp_send_chunk(req, header, HTTPD_RESP_USE_STRLEN) != ESP_OK)
        return ESP_FAIL;

    for (uint32_t i = offset; i < end; ++i) {
        KatmedUserImageInfo info = {0};
        if (katmed_image_store_list(i, &info) != KATMED_IMAGE_OK) {
            (void)httpd_resp_send_chunk(req, "],\"next_offset\":false}",
                                        HTTPD_RESP_USE_STRLEN);
            (void)httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
        (void)snprintf(item, sizeof(item), "%s{\"id\":%" PRIu32
                       ",\"bytes\":%" PRIu32 ",\"width\":%u,\"height\":%u}",
                       first ? "" : ",", info.id, info.bytes,
                       (unsigned)info.width, (unsigned)info.height);
        if (httpd_resp_send_chunk(req, item, HTTPD_RESP_USE_STRLEN) != ESP_OK)
            return ESP_FAIL;
        first = false;
    }
    (void)snprintf(item, sizeof(item), "],\"next_offset\":%s}",
                   end < stats.image_count ? "true" : "false");
    if (httpd_resp_send_chunk(req, item, HTTPD_RESP_USE_STRLEN) != ESP_OK)
        return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t image_upload(httpd_req_t *req)
{
    char content_type[48];
    uint8_t buffer[MEDIA_CHUNK_BYTES];
    KatmedImageUpload upload = {0};
    KatmedUserImageInfo info = {0};
    KatmedImageStatus status;
    size_t remaining;

    if (!authorize(req, true)) return ESP_OK;
    if (req->content_len <= 0 ||
        (size_t)req->content_len > KATMED_IMAGE_UPLOAD_MAX_BYTES) {
        (void)httpd_resp_set_hdr(req, "Connection", "close");
        return media_error(req, "413 Content Too Large", "upload_size",
                           "JPEG-Datei muss zwischen 1 Byte und 128 KiB groß sein.");
    }
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type,
                                    sizeof(content_type)) != ESP_OK ||
        strcmp(content_type, "image/jpeg") != 0) {
        (void)httpd_resp_set_hdr(req, "Connection", "close");
        return media_error(req, "415 Unsupported Media Type", "image_type",
                           "Bitte eine JPEG-Datei hochladen.");
    }

    status = katmed_image_store_upload_begin((uint32_t)req->content_len, &upload);
    if (status != KATMED_IMAGE_OK) {
        (void)httpd_resp_set_hdr(req, "Connection", "close");
        return store_error(req, status);
    }
    remaining = (size_t)req->content_len;
    while (remaining > 0) {
        const size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const int received = httpd_req_recv(req, (char *)buffer, wanted);
        if (received <= 0) {
            (void)katmed_image_store_upload_abort(upload);
            return media_error(req, "400 Bad Request", "upload_interrupted",
                               "Der Upload wurde unterbrochen; vorhandene Bilder bleiben erhalten.");
        }
        status = katmed_image_store_upload_write(upload, buffer, (size_t)received);
        if (status != KATMED_IMAGE_OK) {
            (void)httpd_resp_set_hdr(req, "Connection", "close");
            (void)katmed_image_store_upload_abort(upload);
            return store_error(req, status);
        }
        remaining -= (size_t)received;
    }

    status = katmed_image_store_upload_commit(upload, &info);
    if (status != KATMED_IMAGE_OK) {
        (void)katmed_image_store_upload_abort(upload);
        return store_error(req, status);
    }
    char body[128];
    (void)snprintf(body, sizeof(body), "{\"id\":%" PRIu32 ",\"bytes\":%" PRIu32
                   ",\"width\":%u,\"height\":%u}", info.id, info.bytes,
                   (unsigned)info.width, (unsigned)info.height);
    return media_reply(req, "201 Created", body);
}

static esp_err_t image_remove(httpd_req_t *req)
{
    uint32_t id = 0;
    KatmedImageStatus status;
    if (!authorize(req, true)) return ESP_OK;
    if (!query_uint32(req, "id", &id, true) || id == 0)
        return media_error(req, "400 Bad Request", "invalid_id",
                           "Die Bildkennung ist ungültig.");
    status = katmed_image_store_remove(id);
    if (status != KATMED_IMAGE_OK) return store_error(req, status);
    return media_reply(req, "200 OK", "{\"removed\":true}");
}

bool katmed_web_media_register(httpd_handle_t server,
                               const KatmedWebMediaServices *services)
{
    if (!server || !services || !services->authorize) return false;
    media_services = *services;
    const httpd_uri_t routes[] = {
        {.uri = "/api/images", .method = HTTP_GET, .handler = images_list,
         .user_ctx = NULL},
        {.uri = "/api/images", .method = HTTP_POST, .handler = image_upload,
         .user_ctx = NULL},
        {.uri = "/api/images", .method = HTTP_DELETE, .handler = image_remove,
         .user_ctx = NULL},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) return false;
    }
    return true;
}
