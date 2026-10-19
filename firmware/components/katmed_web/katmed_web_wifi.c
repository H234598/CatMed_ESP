#include "katmed_web_wifi.h"

#include "cJSON.h"
#include "esp_err.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIFI_BODY_MAX 8192u
#define WIFI_QUERY_MAX 48u

static KatmedWebWifiServices wifi_services;

bool katmed_web_wifi_credentials_valid(const char *ssid, size_t ssid_length,
                                      const char *password, size_t password_length,
                                      KatmedWebWifiCredentialAction action)
{
    if (!ssid || ssid_length == 0 || ssid_length > 32 ||
        action < KATMED_WEB_WIFI_CREDENTIAL_KEEP ||
        action > KATMED_WEB_WIFI_CREDENTIAL_OPEN)
        return false;
    if (action == KATMED_WEB_WIFI_CREDENTIAL_OPEN)
        return password_length == 0;
    if (action == KATMED_WEB_WIFI_CREDENTIAL_KEEP)
        return password_length == 0;
    if (!password || password_length > 64) return false;
    if (password_length >= 8 && password_length <= 63) return true;
    if (password_length != 64) return false;
    for (size_t i = 0; i < password_length; ++i) {
        const char ch = password[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F')))
            return false;
    }
    return true;
}

static esp_err_t wifi_reply(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_error(httpd_req_t *req, const char *status,
                            const char *code, const char *message)
{
    char body[192];
    (void)snprintf(body, sizeof(body), "{\"error\":\"%s\",\"message\":\"%s\"}",
                   code, message);
    return wifi_reply(req, status, body);
}

static bool authorize(httpd_req_t *req, bool mutate)
{
    return wifi_services.authorize &&
           wifi_services.authorize(req, mutate, wifi_services.context);
}

static bool get_limit(size_t *limit)
{
    return wifi_services.profile_limit && limit &&
           wifi_services.profile_limit(wifi_services.context, limit);
}

static size_t escaped_length(const char *value)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\' || *p < 0x20) n += 6;
        else ++n;
    }
    return n;
}

static char *append_escaped(char *out, const char *value)
{
    static const char hex[] = "0123456789abcdef";
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') { *out++ = '\\'; *out++ = (char)*p; }
        else if (*p < 0x20) {
            *out++ = '\\'; *out++ = 'u'; *out++ = '0'; *out++ = '0';
            *out++ = hex[*p >> 4]; *out++ = hex[*p & 0xf];
        } else *out++ = (char)*p;
    }
    return out;
}

static esp_err_t wifi_get(httpd_req_t *req)
{
    bool configured = false, recovery = false;
    size_t limit = 0, count = 0, capacity = 0;
    KatmedWebWifiProfileView *profiles = NULL;
    char *body = NULL;
    char *cursor;
    size_t body_capacity = 128;
    if (!authorize(req, false)) return ESP_OK;
    if (!wifi_services.configured(wifi_services.context, &configured) ||
        !wifi_services.recovery_available(wifi_services.context, &recovery) ||
        !get_limit(&limit))
        return wifi_error(req, "503 Service Unavailable", "wifi_unavailable",
                          "Der WLAN-Status ist derzeit nicht verfügbar.");
    if (limit > 0) {
        profiles = calloc(limit, sizeof(*profiles));
        if (!profiles) return wifi_error(req, "503 Service Unavailable", "wifi_unavailable",
                                         "Der WLAN-Status ist derzeit nicht verfügbar.");
    }
    capacity = limit;
    if (!wifi_services.list_profiles(wifi_services.context, profiles, capacity, &count) ||
        count > limit) {
        free(profiles);
        return wifi_error(req, "503 Service Unavailable", "wifi_unavailable",
                          "Die gespeicherten WLAN-Profile sind derzeit nicht verfügbar.");
    }
    for (size_t i = 0; i < count; ++i) {
        const size_t n = escaped_length(profiles[i].ssid);
        if (n > SIZE_MAX - body_capacity - 128) { free(profiles); return ESP_ERR_NO_MEM; }
        body_capacity += n + 128;
    }
    body = malloc(body_capacity);
    if (!body) { free(profiles); return ESP_ERR_NO_MEM; }
    cursor = body;
    cursor += (size_t)snprintf(cursor, body_capacity,
        "{\"configured\":%s,\"recovery_available\":%s,\"profile_limit\":%zu,\"profiles\":[",
        configured ? "true" : "false", recovery ? "true" : "false", limit);
    for (size_t i = 0; i < count; ++i) {
        if (i) *cursor++ = ',';
        cursor += (size_t)snprintf(cursor, body_capacity - (size_t)(cursor - body),
                                   "{\"id\":%u,\"ssid\":\"", profiles[i].id);
        cursor = append_escaped(cursor, profiles[i].ssid);
        cursor += (size_t)snprintf(cursor, body_capacity - (size_t)(cursor - body),
            "\",\"priority\":%u,\"secured\":%s}", profiles[i].priority,
            profiles[i].secured ? "true" : "false");
    }
    (void)snprintf(cursor, body_capacity - (size_t)(cursor - body), "]}");
    free(profiles);
    const esp_err_t result = wifi_reply(req, "200 OK", body);
    free(body);
    return result;
}

static bool read_body(httpd_req_t *req, char **out, size_t *length)
{
    if (!out || !length || req->content_len <= 0 ||
        (size_t)req->content_len > WIFI_BODY_MAX) return false;
    char *body = malloc((size_t)req->content_len + 1u);
    if (!body) return false;
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        const int count = httpd_req_recv(req, body + received,
                                         (size_t)req->content_len - received);
        if (count <= 0) { free(body); return false; }
        received += (size_t)count;
    }
    body[received] = '\0';
    *out = body;
    *length = received;
    return true;
}

static const cJSON *field(const cJSON *object, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static bool parse_uint(const cJSON *node, uint32_t *value)
{
    if (!cJSON_IsNumber(node) || node->valuedouble < 0 ||
        node->valuedouble > UINT32_MAX || node->valuedouble != (double)(uint32_t)node->valuedouble)
        return false;
    *value = (uint32_t)node->valuedouble;
    return true;
}

static bool parse_profiles(const cJSON *root, size_t limit,
                           KatmedWebWifiProfileUpdate **out, size_t *count_out)
{
    const cJSON *array = field(root, "profiles");
    if (!cJSON_IsArray(array)) return false;
    const int count = cJSON_GetArraySize(array);
    if (count < 0 || (size_t)count > limit) return false;
    KatmedWebWifiProfileUpdate *profiles = count ? calloc((size_t)count, sizeof(*profiles)) : NULL;
    if (count && !profiles) return false;
    for (int i = 0; i < count; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(array, i);
        const cJSON *id_node = field(entry, "id");
        const cJSON *ssid_node = field(entry, "ssid");
        const cJSON *priority_node = field(entry, "priority");
        const cJSON *credential_node = field(entry, "credential");
        const cJSON *password_node = field(entry, "password");
        const char *ssid, *credential, *password;
        uint32_t id = 0, priority = 0;
        if (!cJSON_IsObject(entry) || !cJSON_IsString(ssid_node) ||
            !cJSON_IsString(credential_node) || !cJSON_IsString(password_node) ||
            !parse_uint(priority_node, &priority) || priority == 0 ||
            (id_node && !parse_uint(id_node, &id))) goto bad;
        ssid = cJSON_GetStringValue(ssid_node);
        credential = cJSON_GetStringValue(credential_node);
        password = cJSON_GetStringValue(password_node);
        const size_t ssid_length = strlen(ssid), password_length = strlen(password);
        KatmedWebWifiCredentialAction action;
        if (strcmp(credential, "keep") == 0) action = KATMED_WEB_WIFI_CREDENTIAL_KEEP;
        else if (strcmp(credential, "replace") == 0) action = KATMED_WEB_WIFI_CREDENTIAL_REPLACE;
        else if (strcmp(credential, "open") == 0) action = KATMED_WEB_WIFI_CREDENTIAL_OPEN;
        else goto bad;
        if (!katmed_web_wifi_credentials_valid(ssid, ssid_length, password,
                                                password_length, action)) goto bad;
        profiles[i].id = id;
        profiles[i].priority = priority;
        profiles[i].credential_action = action;
        memcpy(profiles[i].ssid, ssid, ssid_length);
        if (action == KATMED_WEB_WIFI_CREDENTIAL_REPLACE)
            memcpy(profiles[i].password, password, password_length);
        for (int j = 0; j < i; ++j) {
            if ((id && profiles[j].id == id) || strcmp(profiles[j].ssid, ssid) == 0)
                goto bad;
        }
    }
    *out = profiles;
    *count_out = (size_t)count;
    return true;
bad:
    if (profiles) { memset(profiles, 0, (size_t)count * sizeof(*profiles)); free(profiles); }
    return false;
}

static esp_err_t wifi_profiles_save(httpd_req_t *req)
{
    char content_type[48];
    char *body = NULL;
    size_t body_length = 0, limit = 0, count = 0;
    KatmedWebWifiProfileUpdate *profiles = NULL;
    cJSON *root = NULL;
    if (!authorize(req, true)) return ESP_OK;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type,
                                    sizeof(content_type)) != ESP_OK ||
        strcmp(content_type, "application/json") != 0) {
        (void)httpd_resp_set_hdr(req, "Connection", "close");
        return wifi_error(req, "415 Unsupported Media Type", "wifi_input_type",
                          "Die WLAN-Profile müssen als JSON übertragen werden.");
    }
    if (!get_limit(&limit) || !read_body(req, &body, &body_length)) {
        (void)httpd_resp_set_hdr(req, "Connection", "close");
        return wifi_error(req, "413 Content Too Large", "wifi_input_size",
                          "Die WLAN-Profileingabe ist zu groß oder unvollständig.");
    }
    if (strstr(body, "\\u0000") != NULL) goto invalid;
    root = cJSON_ParseWithLength(body, body_length);
    if (!cJSON_IsObject(root) || !parse_profiles(root, limit, &profiles, &count)) goto invalid;
    if (!wifi_services.save_profiles_atomic(wifi_services.context, profiles, count)) {
        cJSON_Delete(root); free(body); if (profiles) { memset(profiles, 0, count * sizeof(*profiles)); free(profiles); }
        return wifi_error(req, "503 Service Unavailable", "wifi_save_failed",
                          "Die WLAN-Profile konnten nicht dauerhaft gespeichert werden.");
    }
    cJSON_Delete(root); free(body); if (profiles) { memset(profiles, 0, count * sizeof(*profiles)); free(profiles); }
    return wifi_reply(req, "200 OK", "{\"saved\":true,\"reconnect_required\":true}");
invalid:
    cJSON_Delete(root); free(body); if (profiles) { memset(profiles, 0, count * sizeof(*profiles)); free(profiles); }
    return wifi_error(req, "422 Unprocessable Content", "wifi_input",
                      "WLAN-Profile, Priorität oder Schlüssel haben ein ungültiges Format.");
}

static esp_err_t wifi_profile_delete(httpd_req_t *req)
{
    char query[WIFI_QUERY_MAX], id_text[16];
    uint32_t id = 0;
    if (!authorize(req, true)) return ESP_OK;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id_text, sizeof(id_text)) != ESP_OK ||
        !id_text[0])
        return wifi_error(req, "400 Bad Request", "wifi_id", "WLAN-Profilkennung fehlt.");
    for (const char *p = id_text; *p; ++p) {
        if (*p < '0' || *p > '9' || id > (UINT32_MAX - (uint32_t)(*p - '0')) / 10u)
            return wifi_error(req, "400 Bad Request", "wifi_id", "WLAN-Profilkennung ist ungültig.");
        id = id * 10u + (uint32_t)(*p - '0');
    }
    if (!id || !wifi_services.delete_profile_atomic(wifi_services.context, id))
        return wifi_error(req, "503 Service Unavailable", "wifi_delete_failed",
                          "Das WLAN-Profil konnte nicht entfernt werden.");
    return wifi_reply(req, "200 OK", "{\"removed\":true}");
}

static esp_err_t wifi_reconnect(httpd_req_t *req)
{
    if (!authorize(req, true)) return ESP_OK;
    if (!wifi_services.schedule_reconnect(wifi_services.context))
        return wifi_error(req, "503 Service Unavailable", "wifi_reconnect_failed",
                          "Die WLAN-Neuverbindung konnte nicht geplant werden.");
    return wifi_reply(req, "202 Accepted",
                      "{\"scheduled\":true,\"reevaluate_priority\":true,\"address_may_change\":true}");
}

bool katmed_web_wifi_register(httpd_handle_t server,
                              const KatmedWebWifiServices *services)
{
    if (!server || !services || !services->context || !services->authorize ||
        !services->configured || !services->recovery_available || !services->profile_limit ||
        !services->list_profiles || !services->save_profiles_atomic ||
        !services->delete_profile_atomic || !services->schedule_reconnect)
        return false;
    wifi_services = *services;
    const httpd_uri_t routes[] = {
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get, .user_ctx = NULL},
        {.uri = "/api/wifi/profiles", .method = HTTP_PUT, .handler = wifi_profiles_save, .user_ctx = NULL},
        {.uri = "/api/wifi/profiles", .method = HTTP_DELETE, .handler = wifi_profile_delete, .user_ctx = NULL},
        {.uri = "/api/wifi/reconnect", .method = HTTP_POST, .handler = wifi_reconnect, .user_ctx = NULL},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) return false;
    return true;
}
