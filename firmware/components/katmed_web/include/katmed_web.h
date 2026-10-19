#ifndef KATMED_WEB_H
#define KATMED_WEB_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KATMED_WEB_DEVICE_NAME_MAX 24u

typedef struct {
    char device_name[KATMED_WEB_DEVICE_NAME_MAX];
    bool assigned;
    bool uncertain;
} KatmedWebDeviceInfo;

typedef struct {
    void *context;
    bool (*device_info)(void *context, KatmedWebDeviceInfo *out);
} KatmedWebServices;

/* Called once after board, store and mesh initialization. Failure is closed:
 * no HTTP server remains listening if HTTPS or resource admission fails. */
bool katmed_web_start(const KatmedWebServices *services);
/* Called from the main loop; never from the HTTP task. */
void katmed_web_poll(uint64_t monotonic_ms);
/* Main loop copies this outside the LVGL lock and displays it under that lock.
 * The six digits must never be included in HTTP replies or logs. */
bool katmed_web_pairing_code(char out[7]);
void katmed_web_stop(void);

/* ACME-only hand-off to the existing plain HTTP listener. The caller never
 * starts a second server; NULL means the listener is not ready. */
httpd_handle_t katmed_web_acme_http_server(void);

#ifdef __cplusplus
}
#endif
#endif
