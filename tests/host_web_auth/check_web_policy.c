#include "katmed_web_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *ip = "192.168.4.1";
    assert(katmed_web_authority_matches(ip, ip, 443));
    assert(katmed_web_authority_matches(ip, "192.168.4.1:443", 443));
    assert(!katmed_web_authority_matches(ip, "evil.test", 443));
    assert(!katmed_web_authority_matches(ip, "192.168.4.1.evil.test", 443));
    assert(!katmed_web_authority_matches(ip, "192.168.4.1:80", 443));
    assert(!katmed_web_authority_matches(ip, "192.168.4.1\r\nLocation: https://evil", 443));
    assert(katmed_web_origin_matches(ip, "https://192.168.4.1"));
    assert(katmed_web_origin_matches(ip, "https://192.168.4.1:443"));
    assert(!katmed_web_origin_matches(ip, "http://192.168.4.1"));
    assert(!katmed_web_origin_matches(ip, "https://192.168.4.1.evil.test"));
    assert(!katmed_web_origin_matches(ip, "null"));

    char token[65];
    memset(token, 'a', 64);
    token[64] = 0;
    char header[180];
    snprintf(header, sizeof(header), "other=x; katmed_session=%s; tail=y", token);
    char parsed[65];
    assert(katmed_web_cookie_token(header, parsed));
    assert(strcmp(parsed, token) == 0);
    snprintf(header, sizeof(header), "katmed_session=%s; katmed_session=%s", token, token);
    assert(!katmed_web_cookie_token(header, parsed));
    snprintf(header, sizeof(header), "otherkatmed_session=%s", token);
    assert(!katmed_web_cookie_token(header, parsed));
    snprintf(header, sizeof(header), "katmed_session=%sX", token);
    assert(!katmed_web_cookie_token(header, parsed));
    puts("web host/origin/cookie policy: PASS");
    return 0;
}
