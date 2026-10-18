#ifndef KATMED_IMAGES_HASH_H
#define KATMED_IMAGES_HASH_H

#include "mbedtls/sha256.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle adapter only. All hashing is performed by ESP-IDF's mbedTLS.
 * Start a fresh/released context; finish and free are both terminal operations.
 * Each error after start releases the SDK context, including invalid input. */
typedef struct {
    mbedtls_sha256_context sdk;
    bool active;
} KatmedImageHash;

bool katmed_images_hash_start(KatmedImageHash *ctx);
bool katmed_images_hash_update(KatmedImageHash *ctx, const uint8_t *data, size_t length);
bool katmed_images_hash_finish(KatmedImageHash *ctx, uint8_t result[32]);
void katmed_images_hash_free(KatmedImageHash *ctx);

#ifdef __cplusplus
}
#endif
#endif
