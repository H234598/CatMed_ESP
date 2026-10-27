#ifndef KATMED_WEB_POLICY_H
#define KATMED_WEB_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#define KATMED_WEB_POLICY_TOKEN_HEX 64u
#define KATMED_WEB_IDENTIFIER_MAX 254u

bool katmed_web_identifier_valid(const char *identifier);
bool katmed_web_resolve_authority(const char *local_ip, const char *identifier,
                                  const char *host, unsigned port,
                                  char out[KATMED_WEB_IDENTIFIER_MAX]);
bool katmed_web_same_origin(const char *authority, const char *origin);

bool katmed_web_authority_matches(const char *local_ip, const char *host, unsigned port);
bool katmed_web_origin_matches(const char *local_ip, const char *origin);
bool katmed_web_cookie_token(const char *header,
                             char out[KATMED_WEB_POLICY_TOKEN_HEX + 1]);

#endif
