#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "katmed_web_acme_route.h"

typedef struct {
    const char *uri;
    const char *body;
} Handler;

static Handler handlers[2];
static size_t count;

/* Match the IDF 6.1 registration and first-match dispatch rules. */
static int register_handler(const char *uri, const char *body)
{
    for (size_t i = 0; i < count; ++i)
        if (katmed_web_plain_uri_match(handlers[i].uri, uri, strlen(uri))) return 0;
    if (count == 2) return 0;
    handlers[count++] = (Handler){uri, body};
    return 1;
}

static const char *dispatch(const char *uri)
{
    for (size_t i = 0; i < count; ++i)
        if (katmed_web_plain_uri_match(handlers[i].uri, uri, strlen(uri)))
            return handlers[i].body;
    return NULL;
}

int main(void)
{
    const char *path = "/.well-known/acme-challenge/token123";
    const char *body = "token123.proof";
    assert(register_handler("/*", "redirect"));
    assert(strcmp(dispatch("/config"), "redirect") == 0);
    assert(register_handler(path, body));
    assert(strcmp(dispatch(path), body) == 0);
    assert(strcmp(dispatch("/other"), "redirect") == 0);
    assert(dispatch("/.well-known/acme-challenge/unknown") == NULL);
    assert(!register_handler(path, body));
    assert(!register_handler("/third", "unwanted"));
    assert(katmed_web_acme_host_matches("katmed-probe.invalid", "KATMED-PROBE.INVALID:80"));
    assert(!katmed_web_acme_host_matches("katmed-probe.invalid", "evil.invalid"));
    assert(!katmed_web_acme_host_matches("katmed-probe.invalid", "katmed-probe.invalid:81"));
    handlers[1] = (Handler){0};
    count = 1;
    assert(dispatch(path) == NULL);
    assert(strcmp(dispatch("/config"), "redirect") == 0);
    return 0;
}
