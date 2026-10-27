#include "katmed_web_policy.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

bool katmed_web_identifier_valid(const char *name)
{
    if (!name) return false;
    size_t n = strnlen(name, KATMED_WEB_IDENTIFIER_MAX);
    if (!n) return true; /* Optional, neutral default. */
    if (n >= KATMED_WEB_IDENTIFIER_MAX) return false;
    size_t label = 0; bool dot = false, letter = false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)name[i];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (c == '.') {
            if (!label || name[i-1] == '-') return false;
            label = 0; dot = true;
        } else {
            if (!alpha && !(c >= '0' && c <= '9') && c != '-') return false;
            if ((!label && c == '-') || ++label > 63) return false;
            letter |= alpha;
        }
    }
    return dot && letter && label && name[n-1] != '-';
}

bool katmed_web_resolve_authority(const char *ip, const char *identifier,
                                  const char *host, unsigned port,
                                  char out[KATMED_WEB_IDENTIFIER_MAX])
{
    if (!ip || !identifier || !host || !out || (port != 80 && port != 443) ||
        !katmed_web_identifier_valid(identifier)) return false;
    size_t len = strnlen(host, KATMED_WEB_IDENTIFIER_MAX + 6u);
    if (!len || len >= KATMED_WEB_IDENTIFIER_MAX + 6u) return false;
    const char *colon = strchr(host, ':');
    size_t n = colon ? (size_t)(colon-host) : len;
    if (!n || n >= KATMED_WEB_IDENTIFIER_MAX) return false;
    if (colon && strcmp(colon, port == 80 ? ":80" : ":443")) return false;
    bool local = strlen(ip) == n && !strncmp(host, ip, n);
    bool named = identifier[0] && strlen(identifier) == n && !strncasecmp(host, identifier, n);
    if (!local && !named) return false;
    const char *selected = local ? ip : identifier;
    for (size_t i=0; i<n; ++i) {
        char c=selected[i]; out[i]=(c>='A' && c<='Z') ? (char)(c+'a'-'A') : c;
    }
    out[n]=0;
    return true;
}

bool katmed_web_same_origin(const char *authority, const char *origin)
{
    if (!authority || !origin || strncmp(origin,"https://",8)) return false;
    /* authority was already selected from trusted local IP/configured name.
     * Require that same authority, never another independently allowed alias. */
    size_t n=strnlen(authority,KATMED_WEB_IDENTIFIER_MAX);
    if (!n || n>=KATMED_WEB_IDENTIFIER_MAX) return false;
    const char *value=origin+8;
    if (strncasecmp(value,authority,n)) return false;
    if (value[n] && strcmp(value+n,":443")) return false;
    return true;
}

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
