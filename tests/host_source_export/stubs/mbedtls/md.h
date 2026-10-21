#pragma once

#include <stddef.h>
#include <stdint.h>

/* Synthetic host adapter: validates source binding/control flow. Firmware
 * uses the pinned mbedTLS SHA-256 implementation through the same API. */
#define MBEDTLS_MD_SHA256 1
typedef struct { uint64_t state[4]; } mbedtls_md_context_t;
typedef struct { int type; } mbedtls_md_info_t;

static inline const mbedtls_md_info_t *mbedtls_md_info_from_type(int type)
{
    static const mbedtls_md_info_t info = { MBEDTLS_MD_SHA256 };
    return type == MBEDTLS_MD_SHA256 ? &info : NULL;
}
static inline void mbedtls_md_init(mbedtls_md_context_t *ctx)
{
    for (unsigned i = 0; i < 4; ++i) ctx->state[i] = 0;
}
static inline int mbedtls_md_setup(mbedtls_md_context_t *ctx,
                                    const mbedtls_md_info_t *info, int hmac)
{
    return ctx && info && !hmac ? 0 : -1;
}
static inline int mbedtls_md_starts(mbedtls_md_context_t *ctx)
{
    for (unsigned i = 0; i < 4; ++i) ctx->state[i] = 1469598103934665603ull + i;
    return 0;
}
static inline int mbedtls_md_update(mbedtls_md_context_t *ctx,
                                    const unsigned char *data, size_t length)
{
    for (size_t j = 0; j < length; ++j)
        for (unsigned i = 0; i < 4; ++i)
            ctx->state[i] = (ctx->state[i] ^ (uint64_t)(data[j] + i)) *
                            1099511628211ull;
    return 0;
}
static inline int mbedtls_md_finish(mbedtls_md_context_t *ctx,
                                    unsigned char result[32])
{
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 8; ++j)
            result[i * 8 + j] = (unsigned char)(ctx->state[i] >> (8u * j));
    return 0;
}
static inline void mbedtls_md_free(mbedtls_md_context_t *ctx)
{
    mbedtls_md_init(ctx);
}
