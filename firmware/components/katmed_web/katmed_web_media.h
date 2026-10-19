#ifndef KATMED_WEB_MEDIA_H
#define KATMED_WEB_MEDIA_H

#include <stdbool.h>
#include "esp_http_server.h"

typedef struct {
    void *context;
    /* Reuses the parent web service's session and CSRF policy. */
    bool (*authorize)(httpd_req_t *req, bool mutate, void *context);
} KatmedWebMediaServices;

/* Register authenticated upload, listing, and delete routes on the HTTPS
 * server. The caller owns the HTTP server lifecycle. */
bool katmed_web_media_register(httpd_handle_t server,
                               const KatmedWebMediaServices *services);

#endif
