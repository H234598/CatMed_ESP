#include "katmed_images_hash.h"

bool katmed_images_hash_start(KatmedImageHash *ctx)
{
    if(ctx == NULL) return false;
    ctx->active = false;
    mbedtls_md_init(&ctx->sdk);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if(info == NULL || mbedtls_md_setup(&ctx->sdk, info, 0) != 0 ||
       mbedtls_md_starts(&ctx->sdk) != 0) {
        mbedtls_md_free(&ctx->sdk);
        return false;
    }
    ctx->active = true;
    return true;
}

void katmed_images_hash_free(KatmedImageHash *ctx)
{
    if(ctx != NULL && ctx->active) {
        mbedtls_md_free(&ctx->sdk);
        ctx->active = false;
    }
}

bool katmed_images_hash_update(KatmedImageHash *ctx, const uint8_t *data, size_t length)
{
    if(ctx == NULL || !ctx->active) return false;
    if((data == NULL && length != 0) ||
       (length != 0 && mbedtls_md_update(&ctx->sdk, data, length) != 0)) {
        katmed_images_hash_free(ctx);
        return false;
    }
    return true;
}

bool katmed_images_hash_finish(KatmedImageHash *ctx, uint8_t result[32])
{
    if(ctx == NULL || !ctx->active) return false;
    bool success = result != NULL && mbedtls_md_finish(&ctx->sdk, result) == 0;
    katmed_images_hash_free(ctx);
    return success;
}
