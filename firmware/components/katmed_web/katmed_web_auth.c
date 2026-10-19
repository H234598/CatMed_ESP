#include "katmed_web_auth.h"

#include <string.h>

#define AUTH_MAGIC 0x4b574131u /* KWA1 */
#define AUTH_VERSION 1u
#define SESSION_IDLE_MS (15u * 60u * 1000u)
#define SESSION_MAX_MS (60u * 60u * 1000u)
#define SETUP_NONCE_MS (5u * 60u * 1000u)
#define LOCKOUT_MS (30u * 1000u)
#define SETUP_CODE_MS (5u * 60u * 1000u)

static bool locked(KatmedWebAuth *auth, uint64_t now_ms);
static void failure(KatmedWebAuth *auth, uint64_t now_ms);

static void wipe(void *ptr, size_t len)
{
    volatile uint8_t *p = ptr;
    while (len--) *p++ = 0;
}

bool katmed_web_auth_equal(const void *a, const void *b, size_t len)
{
    const uint8_t *x = a, *y = b;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= x[i] ^ y[i];
    return diff == 0;
}

static bool token(KatmedWebAuth *auth, char out[KATMED_WEB_TOKEN_HEX + 1])
{
    static const char digits[] = "0123456789abcdef";
    uint8_t bytes[32];
    if (!auth->random(auth->context, bytes, sizeof(bytes))) return false;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
    out[KATMED_WEB_TOKEN_HEX] = '\0';
    wipe(bytes, sizeof(bytes));
    return true;
}

static bool new_display_code(KatmedWebAuth *auth, uint64_t now_ms)
{
    uint32_t number;
    do {
        if (!auth->random(auth->context, (uint8_t *)&number, sizeof(number)))
            return false;
    } while (number >= UINT32_MAX - (UINT32_MAX % 1000000u));
    number %= 1000000u;
    for (int i = 5; i >= 0; --i) {
        auth->setup_code[i] = (char)('0' + number % 10u);
        number /= 10u;
    }
    auth->setup_code[6] = '\0';
    auth->setup_code_ms = now_ms;
    return true;
}

static bool password_shape(const char *password)
{
    if (!password) return false;
    const size_t len = strnlen(password, 129);
    return len >= 12 && len <= 128;
}

bool katmed_web_auth_init(KatmedWebAuth *auth, const KatmedWebAuthRecord *stored,
                          KatmedWebRandom random, KatmedWebDerive derive,
                          KatmedWebCommit commit, void *context, uint64_t now_ms)
{
    if (!auth || !random || !derive || !commit) return false;
    memset(auth, 0, sizeof(*auth));
    auth->random = random;
    auth->derive = derive;
    auth->commit = commit;
    auth->context = context;
    auth->storage_available = true;
    if (stored) {
        if (stored->magic != AUTH_MAGIC || stored->version != AUTH_VERSION ||
            stored->cert_len > KATMED_WEB_CERT_MAX) return false;
        auth->record = *stored;
    }
    auth->setup_nonce_ms = now_ms;
    return token(auth, auth->setup_nonce) &&
           (!katmed_web_auth_setup_required(auth) || new_display_code(auth, now_ms));
}

const char *katmed_web_auth_display_code(const KatmedWebAuth *auth)
{
    return katmed_web_auth_setup_required(auth) ? auth->setup_code : NULL;
}

bool katmed_web_auth_tick(KatmedWebAuth *auth, uint64_t now_ms)
{
    if (!katmed_web_auth_setup_required(auth)) return false;
    if (now_ms >= auth->setup_code_ms &&
        now_ms - auth->setup_code_ms < SETUP_CODE_MS) return false;
    return new_display_code(auth, now_ms);
}

bool katmed_web_auth_setup_required(const KatmedWebAuth *auth)
{
    return auth && auth->storage_available && auth->record.magic != AUTH_MAGIC;
}

bool katmed_web_auth_setup_nonce(KatmedWebAuth *auth, uint64_t now_ms,
                                  char out[KATMED_WEB_TOKEN_HEX + 1])
{
    if (!katmed_web_auth_setup_required(auth) || !out) return false;
    if (now_ms < auth->setup_nonce_ms || now_ms - auth->setup_nonce_ms > SETUP_NONCE_MS) {
        if (!token(auth, auth->setup_nonce)) return false;
        auth->setup_nonce_ms = now_ms;
    }
    memcpy(out, auth->setup_nonce, sizeof(auth->setup_nonce));
    return true;
}

static KatmedWebSession *new_session(KatmedWebAuth *auth, uint64_t now_ms,
                                     bool certificate)
{
    KatmedWebSession *slot = NULL;
    for (size_t i = 0; i < 2; ++i) {
        KatmedWebSession *s = &auth->sessions[i];
        if (!s->active || now_ms < s->last_ms ||
            now_ms - s->last_ms > SESSION_IDLE_MS ||
            now_ms < s->created_ms || now_ms - s->created_ms > SESSION_MAX_MS) {
            slot = s;
            break;
        }
    }
    if (!slot) return NULL;
    wipe(slot, sizeof(*slot));
    if (!token(auth, slot->token) || !token(auth, slot->csrf)) {
        wipe(slot, sizeof(*slot));
        return NULL;
    }
    slot->created_ms = slot->last_ms = now_ms;
    slot->certificate = certificate;
    slot->active = true;
    return slot;
}

KatmedWebAuthResult katmed_web_auth_setup(KatmedWebAuth *auth, const char *password,
                                          const char *nonce, const char *display_code,
                                          uint64_t now_ms,
                                          KatmedWebSession **session)
{
    if (session) *session = NULL;
    if (!auth || !session || !password_shape(password) || !nonce || !display_code ||
        strlen(nonce) != KATMED_WEB_TOKEN_HEX) return KATMED_WEB_AUTH_INVALID;
    if (!katmed_web_auth_setup_required(auth)) return KATMED_WEB_AUTH_CONFLICT;
    if (locked(auth, now_ms)) return KATMED_WEB_AUTH_DENIED;
    if (now_ms < auth->setup_nonce_ms ||
        now_ms - auth->setup_nonce_ms > SETUP_NONCE_MS ||
        !katmed_web_auth_equal(nonce, auth->setup_nonce, KATMED_WEB_TOKEN_HEX))
        return KATMED_WEB_AUTH_DENIED;
    if (now_ms < auth->setup_code_ms ||
        now_ms - auth->setup_code_ms > SETUP_CODE_MS ||
        strnlen(display_code, 7) != 6 ||
        !katmed_web_auth_equal(display_code, auth->setup_code, 6)) {
        failure(auth, now_ms);
        return KATMED_WEB_AUTH_DENIED;
    }
    KatmedWebAuthRecord next = {0};
    next.magic = AUTH_MAGIC;
    next.version = AUTH_VERSION;
    if (!auth->random(auth->context, next.salt, sizeof(next.salt)) ||
        !auth->derive(auth->context, password, next.salt, next.verifier)) {
        wipe(&next, sizeof(next));
        return KATMED_WEB_AUTH_STORAGE_ERROR;
    }
    if (!auth->commit(auth->context, &next)) {
        auth->storage_available = false;
        wipe(&next, sizeof(next));
        return KATMED_WEB_AUTH_STORAGE_ERROR;
    }
    auth->record = next;
    wipe(&next, sizeof(next));
    wipe(auth->setup_nonce, sizeof(auth->setup_nonce));
    wipe(auth->setup_code, sizeof(auth->setup_code));
    *session = new_session(auth, now_ms, false);
    return *session ? KATMED_WEB_AUTH_OK : KATMED_WEB_AUTH_STORAGE_ERROR;
}

static bool password_matches(KatmedWebAuth *auth, const char *password)
{
    uint8_t derived[32];
    /* Derive even for malformed but bounded credentials to reduce response
     * differences. The HTTP boundary already caps the full request. */
    if (!password || strnlen(password, 129) > 128 ||
        !auth->derive(auth->context, password, auth->record.salt, derived))
        return false;
    bool same = katmed_web_auth_equal(derived, auth->record.verifier, sizeof(derived));
    wipe(derived, sizeof(derived));
    return same;
}

static bool locked(KatmedWebAuth *auth, uint64_t now_ms)
{
    return auth->lockout_until_ms && now_ms < auth->lockout_until_ms;
}

static void failure(KatmedWebAuth *auth, uint64_t now_ms)
{
    if (++auth->failures >= 5) {
        auth->failures = 0;
        auth->lockout_until_ms = now_ms + LOCKOUT_MS;
    }
}

KatmedWebAuthResult katmed_web_auth_login_password(KatmedWebAuth *auth,
                                                    const char *password,
                                                    uint64_t now_ms,
                                                    KatmedWebSession **session)
{
    if (session) *session = NULL;
    if (!auth || !session || !auth->storage_available ||
        katmed_web_auth_setup_required(auth)) return KATMED_WEB_AUTH_DENIED;
    if (locked(auth, now_ms)) return KATMED_WEB_AUTH_DENIED;
    if (!password_matches(auth, password)) {
        failure(auth, now_ms);
        return KATMED_WEB_AUTH_DENIED;
    }
    auth->failures = 0;
    auth->lockout_until_ms = 0;
    *session = new_session(auth, now_ms, false);
    return *session ? KATMED_WEB_AUTH_OK : KATMED_WEB_AUTH_DENIED;
}

KatmedWebAuthResult katmed_web_auth_login_certificate(KatmedWebAuth *auth,
                                                       const uint8_t fingerprint[32],
                                                       bool tls_verified,
                                                       uint64_t now_ms,
                                                       KatmedWebSession **session)
{
    if (session) *session = NULL;
    if (!auth || !session || !fingerprint || !tls_verified ||
        !auth->storage_available || katmed_web_auth_setup_required(auth) ||
        auth->record.cert_len == 0 ||
        !katmed_web_auth_equal(fingerprint, auth->record.client_fingerprint, 32))
        return KATMED_WEB_AUTH_DENIED;
    *session = new_session(auth, now_ms, true);
    return *session ? KATMED_WEB_AUTH_OK : KATMED_WEB_AUTH_DENIED;
}

KatmedWebSession *katmed_web_auth_session(KatmedWebAuth *auth, const char *value,
                                          uint64_t now_ms)
{
    if (!auth || !auth->storage_available || !value ||
        strnlen(value, KATMED_WEB_TOKEN_HEX + 1) != KATMED_WEB_TOKEN_HEX)
        return NULL;
    for (size_t i = 0; i < 2; ++i) {
        KatmedWebSession *s = &auth->sessions[i];
        if (!s->active || !katmed_web_auth_equal(value, s->token, KATMED_WEB_TOKEN_HEX))
            continue;
        if (now_ms < s->last_ms || now_ms - s->last_ms > SESSION_IDLE_MS ||
            now_ms < s->created_ms || now_ms - s->created_ms > SESSION_MAX_MS ||
            (s->certificate && auth->record.cert_len == 0)) {
            wipe(s, sizeof(*s));
            return NULL;
        }
        s->last_ms = now_ms;
        return s;
    }
    return NULL;
}

void katmed_web_auth_logout(KatmedWebAuth *auth, const char *value)
{
    if (!auth || !value || strnlen(value, KATMED_WEB_TOKEN_HEX + 1) != KATMED_WEB_TOKEN_HEX)
        return;
    for (size_t i = 0; i < 2; ++i) {
        KatmedWebSession *s = &auth->sessions[i];
        if (s->active && katmed_web_auth_equal(value, s->token, KATMED_WEB_TOKEN_HEX))
            wipe(s, sizeof(*s));
    }
}

bool katmed_web_auth_recheck_password(KatmedWebAuth *auth, const char *password,
                                      uint64_t now_ms)
{
    if (!auth || !auth->storage_available || katmed_web_auth_setup_required(auth) ||
        locked(auth, now_ms)) return false;
    if (!password_matches(auth, password)) {
        failure(auth, now_ms);
        return false;
    }
    auth->failures = 0;
    return true;
}

void katmed_web_auth_revoke_certificate_sessions(KatmedWebAuth *auth)
{
    if (!auth) return;
    for (size_t i = 0; i < 2; ++i)
        if (auth->sessions[i].certificate) wipe(&auth->sessions[i], sizeof(auth->sessions[i]));
}

KatmedWebAuthResult katmed_web_auth_set_client(KatmedWebAuth *auth,
                                                const uint8_t *der, size_t len,
                                                const uint8_t fingerprint[32])
{
    if (!auth || !auth->storage_available || katmed_web_auth_setup_required(auth) ||
        !der || !fingerprint || len == 0 || len > KATMED_WEB_CERT_MAX)
        return KATMED_WEB_AUTH_INVALID;
    KatmedWebAuthRecord next = auth->record;
    memset(next.client_cert, 0, sizeof(next.client_cert));
    memcpy(next.client_cert, der, len);
    next.cert_len = (uint16_t)len;
    memcpy(next.client_fingerprint, fingerprint, 32);
    if (!auth->commit(auth->context, &next)) {
        auth->storage_available = false;
        wipe(&next, sizeof(next));
        return KATMED_WEB_AUTH_STORAGE_ERROR;
    }
    auth->record = next;
    wipe(&next, sizeof(next));
    katmed_web_auth_revoke_certificate_sessions(auth);
    return KATMED_WEB_AUTH_OK;
}

KatmedWebAuthResult katmed_web_auth_remove_client(KatmedWebAuth *auth)
{
    if (!auth || !auth->storage_available || katmed_web_auth_setup_required(auth))
        return KATMED_WEB_AUTH_DENIED;
    KatmedWebAuthRecord next = auth->record;
    next.cert_len = 0;
    memset(next.client_cert, 0, sizeof(next.client_cert));
    memset(next.client_fingerprint, 0, sizeof(next.client_fingerprint));
    if (!auth->commit(auth->context, &next)) {
        auth->storage_available = false;
        wipe(&next, sizeof(next));
        return KATMED_WEB_AUTH_STORAGE_ERROR;
    }
    auth->record = next;
    wipe(&next, sizeof(next));
    katmed_web_auth_revoke_certificate_sessions(auth);
    return KATMED_WEB_AUTH_OK;
}
