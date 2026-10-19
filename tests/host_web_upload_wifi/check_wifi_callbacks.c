#include "katmed_web_wifi.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    char ssid[33];
    char password[65];
    bool fail_write;
    bool recovery;
    unsigned sequence;
    unsigned reconnect_sequence;
} FakeWifi;

static bool fake_save(void *context, const char *ssid, size_t ssid_length,
                      const char *password, size_t password_length)
{
    FakeWifi *fake = context;
    char next_ssid[33] = {0};
    char next_password[65] = {0};
    memcpy(next_ssid, ssid, ssid_length);
    memcpy(next_password, password, password_length);
    if (fake->fail_write) return false;
    memcpy(fake->ssid, next_ssid, sizeof(fake->ssid));
    memcpy(fake->password, next_password, sizeof(fake->password));
    fake->sequence++;
    return true;
}

static bool fake_reconnect(void *context)
{
    FakeWifi *fake = context;
    fake->reconnect_sequence = ++fake->sequence;
    return true;
}

static bool fake_recovery(void *context, bool *out)
{
    FakeWifi *fake = context;
    *out = fake->recovery;
    return true;
}

int main(void)
{
    FakeWifi fake = {.ssid = "old-network", .password = "old-password",
                     .fail_write = true, .recovery = true};
    KatmedWebWifiServices services = {
        .context = &fake,
        .save_durable = fake_save,
        .schedule_reconnect = fake_reconnect,
        .recovery_available = fake_recovery,
    };
    const char next_ssid[] = "new-network";
    const char next_password[] = "new-secret";

    assert(katmed_web_wifi_credentials_valid(next_ssid, sizeof(next_ssid) - 1,
                                              next_password, sizeof(next_password) - 1));
    assert(!katmed_web_wifi_credentials_valid("", 0, next_password,
                                               sizeof(next_password) - 1));
    assert(!katmed_web_wifi_credentials_valid(next_ssid, sizeof(next_ssid) - 1,
                                               "short", 5));
    assert(katmed_web_wifi_credentials_valid(next_ssid, sizeof(next_ssid) - 1,
                                              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                              64));
    assert(!katmed_web_wifi_credentials_valid(next_ssid, sizeof(next_ssid) - 1,
                                               "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg",
                                               64));

    assert(!katmed_web_wifi_save_durable(&services, next_ssid,
                                         sizeof(next_ssid) - 1, next_password,
                                         sizeof(next_password) - 1));
    assert(strcmp(fake.ssid, "old-network") == 0);
    assert(strcmp(fake.password, "old-password") == 0);
    assert(fake.sequence == 0 && fake.reconnect_sequence == 0);

    fake.fail_write = false;
    assert(katmed_web_wifi_save_durable(&services, next_ssid,
                                        sizeof(next_ssid) - 1, next_password,
                                        sizeof(next_password) - 1));
    assert(strcmp(fake.ssid, next_ssid) == 0);
    assert(strcmp(fake.password, next_password) == 0);
    assert(fake.sequence == 1 && fake.reconnect_sequence == 0);
    assert(katmed_web_wifi_schedule_reconnect(&services));
    assert(fake.reconnect_sequence == 2);

    bool available = false;
    assert(katmed_web_wifi_recovery_status(&services, &available));
    assert(available);
    fake.recovery = false;
    assert(katmed_web_wifi_recovery_status(&services, &available));
    assert(!available);
    return 0;
}
