#include "katmed_web_policy.h"

#include <stdio.h>
#include <string.h>

bool katmed_web_authority_matches(const char *local_ip, const char *host, unsigned port)
{
    if (!local_ip || !host || (port != 80 && port != 443)) return false;
    size_t ip_length = strnlen(local_ip, 16);
    size_t host_length = strnlen(host, 65);
    if (ip_length == 0 || ip_length >= 16 || host_length == 0 || host_length >= 65)
        return false;
    if (strcmp(host, local_ip) == 0) return true;
    char allowed[24];
    snprintf(allowed, sizeof(allowed), "%s:%u", local_ip, port);
    return strcmp(host, allowed) == 0;
}

bool katmed_web_origin_matches(const char *local_ip, const char *origin)
{
    if (!local_ip || !origin || strnlen(local_ip, 16) >= 16 ||
        strnlen(origin, 65) >= 65) return false;
    char allowed[32];
    snprintf(allowed, sizeof(allowed), "https://%s", local_ip);
    if (strcmp(origin, allowed) == 0) return true;
    snprintf(allowed, sizeof(allowed), "https://%s:443", local_ip);
    return strcmp(origin, allowed) == 0;
}

bool katmed_web_cookie_token(const char *header,
                             char out[KATMED_WEB_POLICY_TOKEN_HEX + 1])
{
    if (!header || !out || strnlen(header, 513) > 512) return false;
    const char *p = header;
    const char *found = NULL;
    while (*p) {
        while (*p == ' ' || *p == ';') ++p;
        if (strncmp(p, "katmed_session=", 15) == 0) {
            if (found) return false;
            found = p + 15;
        }
        p = strchr(p, ';');
        if (!p) break;
        ++p;
    }
    if (!found) return false;
    const char *end = strchr(found, ';');
    size_t len = end ? (size_t)(end - found) : strlen(found);
    if (len != KATMED_WEB_POLICY_TOKEN_HEX) return false;
    for (size_t i = 0; i < len; ++i)
        if (!((found[i] >= '0' && found[i] <= '9') ||
              (found[i] >= 'a' && found[i] <= 'f'))) return false;
    memcpy(out, found, len);
    out[len] = '\0';
    return true;
}
