#include "katmed_web_reset.h"

#include "cJSON.h"
#include "esp_err.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESET_BODY_MAX 192u
#define RESET_TOKEN_MAX 96u

static KatmedWebResetServices reset_services;

static esp_err_t reply(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t error_reply(httpd_req_t *req, const char *status,
                             const char *code, const char *message)
{
    char body[192];
    (void)snprintf(body, sizeof(body), "{\"error\":\"%s\",\"message\":\"%s\"}",
                   code, message);
    return reply(req, status, body);
}

static bool authorize(httpd_req_t *req)
{
    return reset_services.authorize &&
           reset_services.authorize(req, true, reset_services.context);
}

static bool token_is_safe_ascii(const char *token, size_t length)
{
    if (!token || length == 0 || length > RESET_TOKEN_MAX) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = token[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}

static esp_err_t reset_prepare(httpd_req_t *req)
{
    char token[RESET_TOKEN_MAX + 1] = {0};
    char response[RESET_TOKEN_MAX + 64];
    size_t length = 0;
    if (!authorize(req)) return ESP_OK;
    if (!reset_services.prepare_confirmation(reset_services.context, token,
                                              sizeof(token), &length) ||
        !token_is_safe_ascii(token, length)) {
        memset(token, 0, sizeof(token));
        return error_reply(req, "503 Service Unavailable", "reset_unavailable",
                           "Die Bestätigung kann derzeit nicht vorbereitet werden.");
    }
    token[length] = '\0';
    (void)snprintf(response, sizeof(response), "{\"confirmation\":\"%s\"}", token);
    memset(token, 0, sizeof(token));
    const esp_err_t result = reply(req, "200 OK", response);
    memset(response, 0, sizeof(response));
    return result;
}

static esp_err_t reset_confirm(httpd_req_t *req)
{
    char body[RESET_BODY_MAX + 1];
    size_t received = 0;
    cJSON *root = NULL;
    const cJSON *node;
    const char *token;
    size_t token_length;
    if (!authorize(req)) return ESP_OK;
    if (req->content_len <= 0 || (size_t)req->content_len > RESET_BODY_MAX) {
        (void)httpd_resp_set_hdr(req, "Connection", "close");
        return error_reply(req, "413 Content Too Large", "reset_confirmation",
                           "Die Bestätigung ist ungültig oder zu groß.");
    }
    while (received < (size_t)req->content_len) {
        const int count = httpd_req_recv(req, body + received,
                                         (size_t)req->content_len - received);
        if (count <= 0) {
            (void)httpd_resp_set_hdr(req, "Connection", "close");
            memset(body, 0, sizeof(body));
            return error_reply(req, "400 Bad Request", "reset_confirmation",
                               "Die Bestätigung wurde nicht vollständig übertragen.");
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    if (strstr(body, "\\u0000") != NULL) goto invalid;
    root = cJSON_ParseWithLength(body, received);
    node = cJSON_GetObjectItemCaseSensitive(root, "confirmation");
    if (!cJSON_IsObject(root) || !cJSON_IsString(node)) goto invalid;
    token = cJSON_GetStringValue(node);
    token_length = strlen(token);
    if (!token_is_safe_ascii(token, token_length)) goto invalid;
    const KatmedWebResetResult result = reset_services.confirm_and_reset(
        reset_services.context, token, token_length);
    cJSON_Delete(root);
    memset(body, 0, sizeof(body));
    if (result == KATMED_WEB_RESET_STALE)
        return error_reply(req, "409 Conflict", "reset_stale",
                           "Die Bestätigung ist abgelaufen oder wurde bereits verwendet. Es wurden keine Daten geändert.");
    if (result != KATMED_WEB_RESET_ACCEPTED)
        return error_reply(req, "503 Service Unavailable", "reset_failed",
                           "Der Reset konnte nicht sicher abgeschlossen werden. Die Daten bleiben erhalten.");
    return reply(req, "202 Accepted", "{\"accepted\":true}");
invalid:
    cJSON_Delete(root);
    memset(body, 0, sizeof(body));
    return error_reply(req, "422 Unprocessable Content", "reset_confirmation",
                       "Die Bestätigung ist ungültig. Es wurden keine Daten geändert.");
}

bool katmed_web_reset_register(httpd_handle_t server,
                               const KatmedWebResetServices *services)
{
    if (!server || !services || !services->context || !services->authorize ||
        !services->prepare_confirmation || !services->confirm_and_reset)
        return false;
    reset_services = *services;
    const httpd_uri_t routes[] = {
        {.uri = "/api/factory-reset/prepare", .method = HTTP_POST,
         .handler = reset_prepare, .user_ctx = NULL},
        {.uri = "/api/factory-reset/confirm", .method = HTTP_POST,
         .handler = reset_confirm, .user_ctx = NULL},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) return false;
    return true;
}
