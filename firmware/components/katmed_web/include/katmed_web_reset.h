#ifndef KATMED_WEB_RESET_H
#define KATMED_WEB_RESET_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_http_server.h"

typedef enum {
    KATMED_WEB_RESET_ACCEPTED = 0,
    KATMED_WEB_RESET_STALE,
    KATMED_WEB_RESET_FAILED,
} KatmedWebResetResult;

typedef struct {
    void *context;
    bool (*authorize)(httpd_req_t *req, bool mutate, void *context);
    /* Creates a short-lived one-use base64url/hex nonce only; must not mutate
       device data. The token is returned only to the authenticated browser. */
    bool (*prepare_confirmation)(void *context, char *out, size_t capacity,
                                 size_t *length);
    /* A stale/failed operation must leave user data intact. No reset is
       implemented by this web module; the injected owner controls the action. */
    KatmedWebResetResult (*confirm_and_reset)(void *context,
                                              const char *confirmation,
                                              size_t length);
} KatmedWebResetServices;

bool katmed_web_reset_register(httpd_handle_t server,
                               const KatmedWebResetServices *services);

#endif
