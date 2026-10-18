#include "katmed_images_hash.h"

bool katmed_images_hash_start(KatmedImageHash *ctx)
{
    if(ctx == NULL) return false;
    ctx->active = false;
    mbedtls_sha256_init(&ctx->sdk);
    if(mbedtls_sha256_starts(&ctx->sdk, 0) != 0) {
        mbedtls_sha256_free(&ctx->sdk);
        return false;
    }
    ctx->active = true;
    return true;
}

void katmed_images_hash_free(KatmedImageHash *ctx)
{
    if(ctx != NULL && ctx->active) {
        mbedtls_sha256_free(&ctx->sdk);
        ctx->active = false;
    }
}

bool katmed_images_hash_update(KatmedImageHash *ctx, const uint8_t *data, size_t length)
{
    if(ctx == NULL || !ctx->active) return false;
    if((data == NULL && length != 0) ||
       (length != 0 && mbedtls_sha256_update(&ctx->sdk, data, length) != 0)) {
        katmed_images_hash_free(ctx);
        return false;
    }
    return true;
}

bool katmed_images_hash_finish(KatmedImageHash *ctx, uint8_t result[32])
{
    if(ctx == NULL || !ctx->active) return false;
    bool success = result != NULL && mbedtls_sha256_finish(&ctx->sdk, result) == 0;
    katmed_images_hash_free(ctx);
    return success;
}
