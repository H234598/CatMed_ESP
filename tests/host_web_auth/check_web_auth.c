#include "katmed_web_auth.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    KatmedWebAuthRecord durable;
    unsigned random_counter;
    unsigned commits;
    bool fail_commit;
} Fixture;

static bool random_bytes(void *ctx, uint8_t *out, size_t len)
{
    Fixture *f = ctx;
    for (size_t i = 0; i < len; ++i) out[i] = (uint8_t)++f->random_counter;
    return true;
}

/* Deterministic host adapter. Production injects Mbed TLS PBKDF2-HMAC-SHA256.
 * These tests exercise the actual session/setup/commit state machine. */
static bool derive(void *ctx, const char *password, const uint8_t salt[16],
                   uint8_t out[32])
{
    (void)ctx;
    size_t len = strlen(password);
    for (size_t i = 0; i < 32; ++i)
        out[i] = (uint8_t)(salt[i % 16] ^ password[i % len] ^ (uint8_t)i);
    return true;
}

static bool commit(void *ctx, const KatmedWebAuthRecord *record)
{
    Fixture *f = ctx;
    ++f->commits;
    if (f->fail_commit) return false;
    f->durable = *record;
    return true;
}

int main(void)
{
    Fixture f = {0};
    KatmedWebAuth auth;
    assert(katmed_web_auth_init(&auth, NULL, random_bytes, derive, commit, &f, 0));
    assert(katmed_web_auth_setup_required(&auth));
    char nonce[65];
    assert(katmed_web_auth_setup_nonce(&auth, 1, nonce));
    char code[7];
    strcpy(code, katmed_web_auth_display_code(&auth));
    KatmedWebSession *session = NULL;
    assert(katmed_web_auth_login_password(&auth, "password123456", 1, &session) ==
           KATMED_WEB_AUTH_DENIED);
    assert(katmed_web_auth_setup(&auth, "short", nonce, code, 2, &session) ==
           KATMED_WEB_AUTH_INVALID);
    assert(katmed_web_auth_setup(&auth, "password123456", nonce, "000000", 2, &session) ==
           KATMED_WEB_AUTH_DENIED);
    assert(katmed_web_auth_setup(&auth, "password123456", nonce, code, 2, &session) ==
           KATMED_WEB_AUTH_OK);
    assert(session && f.commits == 1 && !katmed_web_auth_setup_required(&auth));
    char original[65];
    strcpy(original, session->token);
    assert(katmed_web_auth_setup(&auth, "another-password", nonce, code, 3, &session) ==
           KATMED_WEB_AUTH_CONFLICT);
    assert(katmed_web_auth_session(&auth, original, 4));
    katmed_web_auth_logout(&auth, original);
    assert(!katmed_web_auth_session(&auth, original, 5));

    for (int i = 0; i < 5; ++i)
        assert(katmed_web_auth_login_password(&auth, "bad-password", 100 + i, &session) ==
               KATMED_WEB_AUTH_DENIED);
    assert(katmed_web_auth_login_password(&auth, "password123456", 105, &session) ==
           KATMED_WEB_AUTH_DENIED);
    assert(katmed_web_auth_login_password(&auth, "password123456", 30105, &session) ==
           KATMED_WEB_AUTH_OK);
    assert(session);
    strcpy(original, session->token);
    assert(!katmed_web_auth_session(&auth, original, 30105 + 15u * 60u * 1000u + 1));

    uint8_t der[3] = {1, 2, 3};
    uint8_t fp[32] = {4};
    assert(katmed_web_auth_set_client(&auth, der, sizeof(der), fp) == KATMED_WEB_AUTH_OK);
    KatmedWebAuth restored;
    assert(katmed_web_auth_init(&restored, &f.durable, random_bytes, derive, commit,
                                &f, 999999));
    assert(!katmed_web_auth_setup_required(&restored));
    assert(katmed_web_auth_login_password(&restored, "password123456", 1000000,
                                          &session) == KATMED_WEB_AUTH_OK);
    assert(katmed_web_auth_login_certificate(&restored, fp, true, 1000001,
                                             &session) == KATMED_WEB_AUTH_OK);
    assert(katmed_web_auth_login_certificate(&auth, fp, false, 1000000, &session) ==
           KATMED_WEB_AUTH_DENIED);
    uint8_t unknown[32] = {5};
    assert(katmed_web_auth_login_certificate(&auth, unknown, true, 1000000, &session) ==
           KATMED_WEB_AUTH_DENIED);
    assert(katmed_web_auth_login_certificate(&auth, fp, true, 1000000, &session) ==
           KATMED_WEB_AUTH_OK);
    strcpy(original, session->token);
    assert(katmed_web_auth_remove_client(&auth) == KATMED_WEB_AUTH_OK);
    assert(!katmed_web_auth_session(&auth, original, 1000001));
    assert(f.durable.cert_len == 0);
    assert(katmed_web_auth_init(&restored, &f.durable, random_bytes, derive, commit,
                                &f, 1000002));
    assert(katmed_web_auth_login_certificate(&restored, fp, true, 1000003,
                                             &session) == KATMED_WEB_AUTH_DENIED);

    f.fail_commit = true;
    assert(katmed_web_auth_set_client(&auth, der, sizeof(der), fp) ==
           KATMED_WEB_AUTH_STORAGE_ERROR);
    assert(!auth.storage_available);
    assert(katmed_web_auth_login_password(&auth, "password123456", 1000002, &session) ==
           KATMED_WEB_AUTH_DENIED);

    Fixture empty = {.fail_commit = true};
    KatmedWebAuth first;
    assert(katmed_web_auth_init(&first, NULL, random_bytes, derive, commit, &empty, 0));
    assert(katmed_web_auth_setup_nonce(&first, 1, nonce));
    strcpy(code, katmed_web_auth_display_code(&first));
    assert(katmed_web_auth_setup(&first, "password123456", nonce, code, 2, &session) ==
           KATMED_WEB_AUTH_STORAGE_ERROR);
    assert(!first.storage_available && empty.durable.magic == 0);
    puts("web auth state machine: PASS");
    return 0;
}
