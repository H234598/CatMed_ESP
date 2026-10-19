#ifndef KATMED_WEB_WIFI_H
#define KATMED_WEB_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_http_server.h"

typedef struct {
    void *context;
    bool (*authorize)(httpd_req_t *req, bool mutate, void *context);
    bool (*configured)(void *context, bool *out);
    bool (*save_durable)(void *context, const char *ssid, size_t ssid_length,
                         const char *password, size_t password_length);
    /* Schedules the deliberate reconnect after the HTTP response can finish. */
    bool (*schedule_reconnect)(void *context);
    bool (*recovery_available)(void *context, bool *out);
} KatmedWebWifiServices;

bool katmed_web_wifi_credentials_valid(const char *ssid, size_t ssid_length,
                                      const char *password, size_t password_length);
bool katmed_web_wifi_save_durable(const KatmedWebWifiServices *services,
                                  const char *ssid, size_t ssid_length,
                                  const char *password, size_t password_length);
bool katmed_web_wifi_schedule_reconnect(const KatmedWebWifiServices *services);
bool katmed_web_wifi_recovery_status(const KatmedWebWifiServices *services,
                                     bool *available);

bool katmed_web_wifi_register(httpd_handle_t server,
                              const KatmedWebWifiServices *services);

#endif
