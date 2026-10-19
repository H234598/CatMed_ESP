#include "katmed_web_wifi.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    const char ssid[] = "synthetic-network";
    const char password[] = "synthetic-password";
    const char hex[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    assert(katmed_web_wifi_credentials_valid(ssid, sizeof(ssid)-1, "", 0,
           KATMED_WEB_WIFI_CREDENTIAL_OPEN));
    assert(katmed_web_wifi_credentials_valid(ssid, sizeof(ssid)-1, "", 0,
           KATMED_WEB_WIFI_CREDENTIAL_KEEP));
    assert(katmed_web_wifi_credentials_valid(ssid, sizeof(ssid)-1, password,
           sizeof(password)-1, KATMED_WEB_WIFI_CREDENTIAL_REPLACE));
    assert(katmed_web_wifi_credentials_valid(ssid, sizeof(ssid)-1, hex, 64,
           KATMED_WEB_WIFI_CREDENTIAL_REPLACE));
    assert(!katmed_web_wifi_credentials_valid("", 0, password, sizeof(password)-1,
           KATMED_WEB_WIFI_CREDENTIAL_REPLACE));
    assert(!katmed_web_wifi_credentials_valid(ssid, sizeof(ssid)-1, "short", 5,
           KATMED_WEB_WIFI_CREDENTIAL_REPLACE));
    assert(!katmed_web_wifi_credentials_valid(ssid, sizeof(ssid)-1, "not-hex", 64,
           KATMED_WEB_WIFI_CREDENTIAL_REPLACE));
    assert(!katmed_web_wifi_credentials_valid(ssid, sizeof(ssid)-1, password,
           sizeof(password)-1, KATMED_WEB_WIFI_CREDENTIAL_KEEP));
    return 0;
}
