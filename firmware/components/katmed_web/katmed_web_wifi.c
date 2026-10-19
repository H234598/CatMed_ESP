#include "katmed_web_wifi.h"

#include "cJSON.h"
#include "esp_err.h"
#include <stdio.h>
#include <string.h>

#define WIFI_BODY_MAX 512u

static KatmedWebWifiServices wifi_services;

bool katmed_web_wifi_credentials_valid(const char *ssid, size_t ssid_length,
                                      const char *password, size_t password_length)
{
    if (!ssid || !password || ssid_length == 0 || ssid_length > 32 ||
        password_length > 64)
        return false;
    if (password_length == 0 || (password_length >= 8 && password_length <= 63))
        return true;
    if (password_length != 64) return false;
    for (size_t i = 0; i < password_length; ++i) {
        const char ch = password[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F')))
            return false;
    }
    return true;
}

bool katmed_web_wifi_save_durable(const KatmedWebWifiServices *services,
                                  const char *ssid, size_t ssid_length,
                                  const char *password, size_t password_length)
{
    return services && services->save_durable && services->context &&
           katmed_web_wifi_credentials_valid(ssid, ssid_length, password,
                                             password_length) &&
           services->save_durable(services->context, ssid, ssid_length,
                                  password, password_length);
}

bool katmed_web_wifi_schedule_reconnect(const KatmedWebWifiServices *services)
{
    return services && services->schedule_reconnect && services->context &&
           services->schedule_reconnect(services->context);
}

bool katmed_web_wifi_recovery_status(const KatmedWebWifiServices *services,
                                     bool *available)
{
    return services && services->recovery_available && services->context &&
           available && services->recovery_available(services->context, available);
}

static esp_err_t wifi_reply(httpd_req_t *req, const char *status,
                            const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_error(httpd_req_t *req, const char *status,
                            const char *code, const char *message)
{
    char body[160];
    (void)snprintf(body, sizeof(body), "{\"error\":\"%s\",\"message\":\"%s\"}",
                   code, message);
    return wifi_reply(req, status, body);
}

static bool authorize(httpd_req_t *req, bool mutate)
{
    return wifi_services.authorize &&
           wifi_services.authorize(req, mutate, wifi_services.context);
}

static esp_err_t wifi_get(httpd_req_t *req)
{
    bool configured = false;
    bool recovery = false;
    char body[96];
    if (!authorize(req, false)) return ESP_OK;
    if (!wifi_services.configured(wifi_services.context, &configured) ||
        !katmed_web_wifi_recovery_status(&wifi_services, &recovery))
        return wifi_error(req, "503 Service Unavailable", "wifi_unavailable",
                          "Der WLAN-Status ist derzeit nicht verfügbar.");
    (void)snprintf(body, sizeof(body),
                   "{\"configured\":%s,\"recovery_available\":%s}",
                   configured ? "true" : "false", recovery ? "true" : "false");
    return wifi_reply(req, "200 OK", body);
}

static esp_err_t wifi_save(httpd_req_t *req)
{
    char body[WIFI_BODY_MAX + 1u];
    char content_type[48];
    size_t received = 0;
    cJSON *root = NULL;
    const cJSON *ssid_node;
    const cJSON *password_node;
    const char *ssid;
    const char *password;
    size_t ssid_length;
    size_t password_length;

    if (!authorize(req, true)) return ESP_OK;
    if (req->content_len <= 0 || (size_t)req->content_len > WIFI_BODY_MAX) {
        (void)httpd_resp_set_hdr(req, "Connection", "close");
        return wifi_error(req, "413 Content Too Large", "wifi_input_size",
                          "Die WLAN-Eingabe ist zu groß.");
    }
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type,
                                    sizeof(content_type)) != ESP_OK ||
        strcmp(content_type, "application/json") != 0) {
        (void)httpd_resp_set_hdr(req, "Connection", "close");
        return wifi_error(req, "415 Unsupported Media Type", "wifi_input_type",
                          "Die WLAN-Daten müssen als JSON übertragen werden.");
    }
    while (received < (size_t)req->content_len) {
        const int count = httpd_req_recv(req, body + received,
                                         (size_t)req->content_len - received);
        if (count <= 0)
            return wifi_error(req, "400 Bad Request", "wifi_input",
                              "Die WLAN-Eingabe wurde nicht vollständig übertragen.");
        received += (size_t)count;
    }
    body[received] = '\0';
    if (strstr(body, "\\u0000") != NULL)
        return wifi_error(req, "422 Unprocessable Content", "wifi_input",
                          "SSID oder Schlüssel enthält ungültige Zeichen.");
    root = cJSON_ParseWithLength(body, received);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return wifi_error(req, "422 Unprocessable Content", "wifi_input",
                          "SSID und WLAN-Schlüssel müssen als Text angegeben werden.");
    }
    ssid_node = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    password_node = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(ssid_node) || !cJSON_IsString(password_node)) {
        cJSON_Delete(root);
        return wifi_error(req, "422 Unprocessable Content", "wifi_input",
                          "SSID und WLAN-Schlüssel müssen als Text angegeben werden.");
    }
    ssid = cJSON_GetStringValue(ssid_node);
    password = cJSON_GetStringValue(password_node);
    ssid_length = strlen(ssid);
    password_length = strlen(password);
    if (!katmed_web_wifi_credentials_valid(ssid, ssid_length, password,
                                           password_length)) {
        cJSON_Delete(root);
        return wifi_error(req, "422 Unprocessable Content", "wifi_input",
                          "SSID oder WLAN-Schlüssel hat eine ungültige Länge.");
    }
    if (!katmed_web_wifi_save_durable(&wifi_services, ssid, ssid_length,
                                      password, password_length)) {
        cJSON_Delete(root);
        return wifi_error(req, "503 Service Unavailable", "wifi_save_failed",
                          "Die WLAN-Daten konnten nicht dauerhaft gespeichert werden.");
    }
    cJSON_Delete(root);
    return wifi_reply(req, "200 OK",
                      "{\"saved\":true,\"reconnect_required\":true,"
                      "\"address_may_change\":true}");
}

static esp_err_t wifi_reconnect(httpd_req_t *req)
{
    if (!authorize(req, true)) return ESP_OK;
    if (!katmed_web_wifi_schedule_reconnect(&wifi_services))
        return wifi_error(req, "503 Service Unavailable", "wifi_reconnect_failed",
                          "Die WLAN-Neuverbindung konnte nicht geplant werden.");
    return wifi_reply(req, "202 Accepted",
                      "{\"scheduled\":true,\"address_may_change\":true}");
}

bool katmed_web_wifi_register(httpd_handle_t server,
                              const KatmedWebWifiServices *services)
{
    if (!server || !services || !services->authorize || !services->configured ||
        !services->save_durable || !services->schedule_reconnect ||
        !services->recovery_available)
        return false;
    wifi_services = *services;
    const httpd_uri_t routes[] = {
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get,
         .user_ctx = NULL},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_save,
         .user_ctx = NULL},
        {.uri = "/api/wifi/reconnect", .method = HTTP_POST, .handler = wifi_reconnect,
         .user_ctx = NULL},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) return false;
    }
    return true;
}
