#ifndef KATMED_WEB_H
#define KATMED_WEB_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "katmed_web_wifi.h"
#include "katmed_web_reset.h"
#include "katmed_web_policy.h"
#include "katmed_language.h"
#include "katmed_config_bridge.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KATMED_WEB_DEVICE_NAME_MAX 24u

typedef struct {
    uint64_t generation;
    uint64_t origin_revision;
    uint8_t sha256[32];
    char dns_san[KATMED_WEB_IDENTIFIER_MAX];
    /* Persisted configured host for the HTTPS Origin, empty if unconfigured. */
    char authority[KATMED_WEB_IDENTIFIER_MAX];
} KatmedWebBootstrapSnapshot;

typedef struct {
    char device_name[KATMED_WEB_DEVICE_NAME_MAX];
    bool assigned;
    bool uncertain;
} KatmedWebDeviceInfo;

typedef struct {
    void *context;
    /* Main-loop lifecycle barrier before destroying/replacing HTTP handles. */
    bool (*prepare_listener_stop)(void *context);
    bool (*device_info)(void *context, KatmedWebDeviceInfo *out);
    KatmedConfigStatus (*language_get)(void *context, uint8_t *out);
    KatmedConfigStatus (*language_set)(void *context, uint8_t language, uint8_t *committed);
    /* Runtime data adapters. katmed_web_start supplies the shared HTTPS
     * session/CSRF authorize callback before registering these routes. */
    KatmedWebWifiServices wifi;
    KatmedWebResetServices reset;
} KatmedWebServices;

/* Called once after board, store and mesh initialization. Failure is closed:
 * no HTTP server remains listening if HTTPS or resource admission fails. */
void katmed_web_set_enabled(bool enabled);
esp_err_t katmed_web_start(const KatmedWebServices *services);
/* Called from the main loop; never from the HTTP task. */
void katmed_web_poll(uint64_t monotonic_ms);
/* Main loop copies this outside the LVGL lock and displays it under that lock.
 * The six digits must never be included in HTTP replies or logs. */
bool katmed_web_pairing_code(char out[7]);
/* Firmware-local copy of the active bootstrap listener's public leaf metadata.
 * Safe from another task. A false return clears any supplied output. Re-read
 * before using a retained generation across a listener transition. */
bool katmed_web_bootstrap_snapshot(KatmedWebBootstrapSnapshot *out);
/* Validate a previously copied listener and Origin without copying strings. */
bool katmed_web_bootstrap_snapshot_matches(uint64_t generation, uint64_t origin_revision);
/* False leaves retained HTTPS/key/identity state intact for a later retry. */
bool katmed_web_stop(void);

/* ACME-only hand-off to the existing plain HTTP listener. The caller never
 * starts a second server; NULL means the listener is not ready. */
httpd_handle_t katmed_web_acme_http_server(void);
/* Desired optional KatmedAcmeConfig.identifier, copied from the sole persisted
 * Web configuration owner. Empty disables production ACME naming. */
bool katmed_web_acme_identifier(char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
