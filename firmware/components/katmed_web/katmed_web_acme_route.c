#include "katmed_web_acme_route.h"

#include <string.h>

static const char challenge_prefix[] = "/.well-known/acme-challenge/";

bool katmed_web_plain_uri_match(const char *pattern, const char *uri, size_t length)
{
    if (pattern == NULL || uri == NULL) return false;
    if (strcmp(pattern, "/*") == 0) {
        if (length >= sizeof(challenge_prefix) - 1 &&
            strncmp(uri, challenge_prefix, sizeof(challenge_prefix) - 1) == 0)
            return false;
        return length != 0 && uri[0] == '/';
    }
    return strlen(pattern) == length && strncmp(pattern, uri, length) == 0;
}

bool katmed_web_acme_host_matches(const char *identifier, const char *host)
{
    if (identifier == NULL || host == NULL) return false;
    size_t length = strlen(identifier);
    size_t host_length = strlen(host);
    if (length == 0 || length > 253 ||
        (host_length != length && host_length != length + 3)) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)identifier[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '.')) return false;
        unsigned char supplied = (unsigned char)host[i];
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (supplied >= 'A' && supplied <= 'Z') supplied += 'a' - 'A';
        if (ch != supplied) return false;
    }
    return host[length] == '\0' || strcmp(host + length, ":80") == 0;
}
