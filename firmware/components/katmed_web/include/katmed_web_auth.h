#ifndef KATMED_WEB_AUTH_H
#define KATMED_WEB_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KATMED_WEB_CERT_MAX 1024u
#define KATMED_WEB_TOKEN_HEX 64u
#define KATMED_WEB_PBKDF2_ROUNDS 50000u

/* One NVS blob. The two-byte certificate length is authoritative; unused
 * bytes must be zero on write. No plaintext password or client private key. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t cert_len;
    uint8_t salt[16];
    uint8_t verifier[32];
    uint8_t client_fingerprint[32];
    uint8_t client_cert[KATMED_WEB_CERT_MAX];
} KatmedWebAuthRecord;

typedef bool (*KatmedWebRandom)(void *context, uint8_t *out, size_t length);
typedef bool (*KatmedWebDerive)(void *context, const char *password,
                               const uint8_t salt[16], uint8_t out[32]);
typedef bool (*KatmedWebCommit)(void *context, const KatmedWebAuthRecord *record);

typedef struct {
    char token[KATMED_WEB_TOKEN_HEX + 1];
    char csrf[KATMED_WEB_TOKEN_HEX + 1];
    uint64_t created_ms;
    uint64_t last_ms;
    bool active;
    bool certificate;
} KatmedWebSession;

typedef struct {
    KatmedWebAuthRecord record;
    KatmedWebSession sessions[2];
    char setup_nonce[KATMED_WEB_TOKEN_HEX + 1];
    uint64_t setup_nonce_ms;
    char setup_code[7]; /* Display-only possession proof. Never serialize/log. */
    uint64_t setup_code_ms;
    uint64_t lockout_until_ms;
    uint8_t failures;
    bool storage_available;
    KatmedWebRandom random;
    KatmedWebDerive derive;
    KatmedWebCommit commit;
    void *context;
} KatmedWebAuth;

typedef enum {
    KATMED_WEB_AUTH_OK,
    KATMED_WEB_AUTH_DENIED,
    KATMED_WEB_AUTH_CONFLICT,
    KATMED_WEB_AUTH_STORAGE_ERROR,
    KATMED_WEB_AUTH_INVALID
} KatmedWebAuthResult;

bool katmed_web_auth_init(KatmedWebAuth *auth, const KatmedWebAuthRecord *stored,
                          KatmedWebRandom random, KatmedWebDerive derive,
                          KatmedWebCommit commit, void *context, uint64_t now_ms);
bool katmed_web_auth_setup_required(const KatmedWebAuth *auth);
bool katmed_web_auth_setup_nonce(KatmedWebAuth *auth, uint64_t now_ms,
                                  char out[KATMED_WEB_TOKEN_HEX + 1]);
/* Main/UI thread only: caller must hold the LVGL lock when presenting it. */
const char *katmed_web_auth_display_code(const KatmedWebAuth *auth);
bool katmed_web_auth_tick(KatmedWebAuth *auth, uint64_t now_ms);
KatmedWebAuthResult katmed_web_auth_setup(KatmedWebAuth *auth, const char *password,
                                          const char *nonce, const char *display_code,
                                          uint64_t now_ms,
                                          KatmedWebSession **session);
KatmedWebAuthResult katmed_web_auth_login_password(KatmedWebAuth *auth,
                                                    const char *password,
                                                    uint64_t now_ms,
                                                    KatmedWebSession **session);
KatmedWebAuthResult katmed_web_auth_login_certificate(KatmedWebAuth *auth,
                                                       const uint8_t fingerprint[32],
                                                       bool tls_verified,
                                                       uint64_t now_ms,
                                                       KatmedWebSession **session);
KatmedWebSession *katmed_web_auth_session(KatmedWebAuth *auth, const char *token,
                                          uint64_t now_ms);
void katmed_web_auth_logout(KatmedWebAuth *auth, const char *token);
bool katmed_web_auth_recheck_password(KatmedWebAuth *auth, const char *password,
                                      uint64_t now_ms);
KatmedWebAuthResult katmed_web_auth_set_client(KatmedWebAuth *auth,
                                                const uint8_t *der, size_t der_len,
                                                const uint8_t fingerprint[32]);
KatmedWebAuthResult katmed_web_auth_remove_client(KatmedWebAuth *auth);
void katmed_web_auth_revoke_certificate_sessions(KatmedWebAuth *auth);
bool katmed_web_auth_equal(const void *a, const void *b, size_t len);

#endif
