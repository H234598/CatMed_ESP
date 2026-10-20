#include "katmed_web_reset.h"
#include "esp_err.h"

static KatmedWebResetServices reset_services;

static esp_err_t reply(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static bool authorize(httpd_req_t *req, bool mutate)
{
    return reset_services.authorize &&
           reset_services.authorize(req, mutate, reset_services.context);
}

static esp_err_t reset_prepare(httpd_req_t *req)
{
    if (!authorize(req, true)) return ESP_OK;
    if (!reset_services.offer_on_display(reset_services.context))
        return reply(req, "409 Conflict", "{\"error\":\"reset_unavailable\"}");
    return reply(req, "202 Accepted", "{\"display\":\"preparing\"}");
}

static esp_err_t reset_status(httpd_req_t *req)
{
    if (!authorize(req, false)) return ESP_OK;
    switch (reset_services.status(reset_services.context)) {
    case KATMED_WEB_RESET_PREPARING:
        return reply(req, "200 OK", "{\"display\":\"preparing\"}");
    case KATMED_WEB_RESET_PREPARED:
        return reply(req, "200 OK", "{\"display\":\"press_reset\"}");
    case KATMED_WEB_RESET_WARNING:
        return reply(req, "200 OK", "{\"display\":\"confirm_on_display\"}");
    default:
        return reply(req, "200 OK", "{\"display\":\"idle\"}");
    }
}

static esp_err_t reset_cancel(httpd_req_t *req)
{
    if (!authorize(req, true)) return ESP_OK;
    if (!reset_services.cancel_on_display(reset_services.context))
        return reply(req, "503 Service Unavailable", "{\"error\":\"reset_cancel_failed\"}");
    return reply(req, "200 OK", "{\"display\":\"idle\"}");
}

bool katmed_web_reset_register(httpd_handle_t server,
                               const KatmedWebResetServices *services)
{
    if (!server || !services || !services->context || !services->authorize ||
        !services->offer_on_display || !services->status ||
        !services->cancel_on_display) return false;
    reset_services = *services;
    const httpd_uri_t routes[] = {
        {.uri = "/api/factory-reset/prepare", .method = HTTP_POST,
         .handler = reset_prepare, .user_ctx = NULL},
        {.uri = "/api/factory-reset/status", .method = HTTP_GET,
         .handler = reset_status, .user_ctx = NULL},
        {.uri = "/api/factory-reset/cancel", .method = HTTP_POST,
         .handler = reset_cancel, .user_ctx = NULL},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) return false;
    return true;
}
