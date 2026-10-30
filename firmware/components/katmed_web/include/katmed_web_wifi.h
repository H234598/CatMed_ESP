#ifndef KATMED_WEB_WIFI_H
#define KATMED_WEB_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"
#include "katmed_config_bridge_diag.h"

typedef enum {
    KATMED_WEB_WIFI_CREDENTIAL_KEEP = 0,
    KATMED_WEB_WIFI_CREDENTIAL_REPLACE,
    KATMED_WEB_WIFI_CREDENTIAL_OPEN,
} KatmedWebWifiCredentialAction;

typedef struct {
    uint32_t id;
    char ssid[33];
    uint32_t priority; /* 1 is highest; equal values retain list order. */
    bool secured;
} KatmedWebWifiProfileView;

typedef struct {
    uint32_t id; /* 0 means new profile; otherwise an existing opaque ID. */
    char ssid[33];
    uint32_t priority;
    KatmedWebWifiCredentialAction credential_action;
    char password[65]; /* Empty unless action is REPLACE; never returned or logged. */
} KatmedWebWifiProfileUpdate;

/* Filled by the status callback used by this WLAN GET, never from shared state. */
typedef struct {
    int bridge;
    unsigned bridge_diag;
    bool ready_valid;
    bool ready;
    bool available;
    KatmedConfigBridgeTrace trace;
} KatmedWebWifiStatusDiagnostic;

typedef struct {
    void *context;
    bool (*authorize)(httpd_req_t *req, bool mutate, void *context);
    bool (*configured)(void *context, bool *out, KatmedWebWifiStatusDiagnostic *diagnostic);
    bool (*recovery_available)(void *context, bool *out, KatmedWebWifiStatusDiagnostic *diagnostic);
    /* Derived from actual NVS/RAM budget; C does not invent a profile count. */
    bool (*profile_limit)(void *context, size_t *out, KatmedWebWifiStatusDiagnostic *diagnostic);
    /* Metadata only. Never return credentials through this callback. The load
       adapter migrates legacy single-network state as priority 1; stored
       runtime profiles override explicitly selected private build defaults. */
    bool (*list_profiles)(void *context, KatmedWebWifiProfileView *out,
                          size_t capacity, size_t *count);
    /* Validates IDs, retains KEEP credentials, and atomically replaces the set. */
    bool (*save_profiles_atomic)(void *context,
                                 const KatmedWebWifiProfileUpdate *profiles,
                                 size_t count);
    bool (*delete_profile_atomic)(void *context, uint32_t id);
    /* Explicit user command only. Runtime tries profiles by priority, with
       stable order for ties, falls back after unreachable/bad-auth failures,
       and does not disturb a stable connection automatically. */
    bool (*schedule_reconnect)(void *context);
} KatmedWebWifiServices;

bool katmed_web_wifi_credentials_valid(const char *ssid, size_t ssid_length,
                                      const char *password, size_t password_length,
                                      KatmedWebWifiCredentialAction action);
bool katmed_web_wifi_register(httpd_handle_t server,
                              const KatmedWebWifiServices *services);

#endif
