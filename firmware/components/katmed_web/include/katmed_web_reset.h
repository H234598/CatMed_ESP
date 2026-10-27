#ifndef KATMED_WEB_RESET_H
#define KATMED_WEB_RESET_H

#include <stdbool.h>
#include "esp_http_server.h"

typedef enum {
    KATMED_WEB_RESET_IDLE,
    KATMED_WEB_RESET_PREPARING,
    KATMED_WEB_RESET_PREPARED,
    KATMED_WEB_RESET_WARNING,
    KATMED_WEB_RESET_UNAVAILABLE,
    KATMED_WEB_RESET_STOP_PENDING,
    KATMED_WEB_RESET_COMMITTED,
} KatmedWebResetStatus;

typedef enum {
    KATMED_WEB_RESET_CANCELLED,
    KATMED_WEB_RESET_CANCEL_TOO_LATE,
    KATMED_WEB_RESET_CANCEL_UNAVAILABLE,
    KATMED_WEB_RESET_CANCEL_FAILED,
} KatmedWebResetCancelResult;

typedef struct {
    void *context;
    bool (*authorize)(httpd_req_t *req, bool mutate, void *context);
    /* HTTP only requests a display tile. It never receives a reset token and
     * has no confirmation or journal entry point. */
    bool (*offer_on_display)(void *context);
    KatmedWebResetStatus (*status)(void *context);
    KatmedWebResetCancelResult (*cancel_on_display)(void *context);
} KatmedWebResetServices;

bool katmed_web_reset_register(httpd_handle_t server,
                               const KatmedWebResetServices *services);

#endif
