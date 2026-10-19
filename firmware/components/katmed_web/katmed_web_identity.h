#ifndef KATMED_WEB_IDENTITY_H
#define KATMED_WEB_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>
#include "nvs.h"

#define KATMED_WEB_KEY_MAX 256u
#define KATMED_WEB_SERVER_CERT_MAX 1024u

typedef struct {
    uint32_t magic;
    uint16_t key_len;
    uint16_t cert_len;
    uint8_t key[KATMED_WEB_KEY_MAX];
    uint8_t cert[KATMED_WEB_SERVER_CERT_MAX];
} KatmedWebIdentity;

/* Creates an ECDSA P-256 key and a per-device self-signed leaf once. Storage
 * is one atomic NVS blob; corrupt existing identity is a hard failure. */
bool katmed_web_identity_load_or_create(nvs_handle_t nvs, KatmedWebIdentity *out);

#endif
