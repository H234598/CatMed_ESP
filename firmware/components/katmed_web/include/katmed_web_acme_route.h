#ifndef KATMED_WEB_ACME_ROUTE_H
#define KATMED_WEB_ACME_ROUTE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The catch-all redirect yields only the ACME HTTP-01 path to an exact
 * handler. The same matcher is used by IDF during registration and dispatch. */
bool katmed_web_plain_uri_match(const char *pattern, const char *uri, size_t length);
bool katmed_web_acme_host_matches(const char *identifier, const char *host);

#ifdef __cplusplus
}
#endif
#endif
