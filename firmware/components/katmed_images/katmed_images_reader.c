#ifndef ESP_PLATFORM
#define _POSIX_C_SOURCE 200809L
#endif
#include "katmed_images_internal.h"
#include "katmed_images_hash.h"
#include <inttypes.h>
#ifdef ESP_PLATFORM
#include "katmed_image_store.h"
#endif
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <time.h>
#endif

static KatmedStorage mounted_storage;
static bool mounted;
static KatmedImageStatus last_error = KATMED_IMAGE_UNAVAILABLE;
static KatmedImageTimingSample pack_hash_timing;
static KatmedImageTimingSample variant_hash_timing;

static uint64_t monotonic_us(void)
{
#ifdef ESP_PLATFORM
    int64_t now = esp_timer_get_time();
    return now < 0 ? 0 : (uint64_t)now;
#else
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
#endif
}

static KatmedImageTimingSample complete_timing(uint64_t start, KatmedImageStatus status)
{
    uint64_t end = monotonic_us();
    return (KatmedImageTimingSample){.elapsed_us = end >= start ? end - start : 0,
                                     .status = status, .measured = true};
}

KatmedImageTimingSample katmed_images_pack_hash_timing(void) { return pack_hash_timing; }
KatmedImageTimingSample katmed_images_variant_hash_timing(void) { return variant_hash_timing; }

void katmed_images_record_error(KatmedImageStatus status)
{
    if(status != KATMED_IMAGE_OK) last_error = status;
}

void katmed_images_clear_error(void) { last_error = KATMED_IMAGE_OK; }

KatmedImageStatus katmed_images_last_error(void) { return last_error; }
const uint8_t *katmed_images_catalog_sha256(void) { return katmed_generated_catalog.catalog_sha256; }
uint32_t katmed_images_count(void)
{
    uint32_t count = KATMED_IMAGE_COUNT;
#ifdef ESP_PLATFORM
    KatmedImageStoreStats stats;
    if (katmed_image_store_stats(&stats) == KATMED_IMAGE_OK &&
        UINT32_MAX - count >= stats.image_count) count += stats.image_count;
#endif
    return count;
}

uint32_t katmed_images_builtin_count(void) { return KATMED_IMAGE_COUNT; }
uint32_t katmed_images_builtin_profile_count(void)
{
    return katmed_generated_catalog.profile_count;
}

KatmedImageStatus katmed_images_builtin_variant_info(uint32_t index,
                                                      uint32_t profile,
                                                      KatmedImageVariantInfo *out)
{
    if (out == NULL) return KATMED_IMAGE_BAD_ID;
    const KatmedImageVariant *variant = katmed_images_variant(index, profile);
    if (variant == NULL) return KATMED_IMAGE_BAD_ID;
    *out = (KatmedImageVariantInfo){.bytes=variant->length,
                                    .width=variant->width,
                                    .height=variant->height};
    return KATMED_IMAGE_OK;
}

void katmed_images_unmount(void)
{
#ifdef ESP_PLATFORM
    katmed_image_store_unmount();
#endif
    memset(&mounted_storage, 0, sizeof(mounted_storage));
    mounted = false;
    last_error = KATMED_IMAGE_UNAVAILABLE;
    pack_hash_timing = (KatmedImageTimingSample){0};
    variant_hash_timing = (KatmedImageTimingSample){0};
}

static bool within(uint32_t offset, uint32_t length, uint32_t limit)
{
    return offset <= limit && length <= limit - offset;
}

static KatmedImageStatus read_exact(uint32_t offset, uint8_t *dst, size_t length)
{
    if(length > UINT32_MAX || !within(offset, (uint32_t)length, mounted_storage.partition_size))
        return KATMED_IMAGE_BAD_RANGE;
    if(length == 0) return KATMED_IMAGE_OK;
    if(dst == NULL || mounted_storage.read == NULL) return KATMED_IMAGE_IO_ERROR;
    size_t actual = 0;
    if(!mounted_storage.read(mounted_storage.context, offset, dst, length, &actual) || actual != length)
        return KATMED_IMAGE_IO_ERROR;
    return KATMED_IMAGE_OK;
}

static KatmedImageStatus hash_range(uint32_t start, uint32_t length, uint8_t hash[32])
{
    if(!within(start, length, mounted_storage.partition_size)) return KATMED_IMAGE_BAD_RANGE;
    KatmedImageHash sha;
    if(!katmed_images_hash_start(&sha)) return KATMED_IMAGE_IO_ERROR;
    uint8_t block[KATMED_IMAGE_READ_CHUNK];
    for(uint32_t position = 0; position < length;) {
        uint32_t remaining = length - position;
        size_t take = remaining < sizeof(block) ? remaining : sizeof(block);
        KatmedImageStatus status = read_exact(start + position, block, take);
        if(status != KATMED_IMAGE_OK || !katmed_images_hash_update(&sha, block, take)) {
            katmed_images_hash_free(&sha);
            return status != KATMED_IMAGE_OK ? status : KATMED_IMAGE_IO_ERROR;
        }
        position += (uint32_t)take;
    }
    return katmed_images_hash_finish(&sha, hash) ? KATMED_IMAGE_OK : KATMED_IMAGE_IO_ERROR;
}

const KatmedImageVariant *katmed_images_variant(uint32_t index, uint32_t profile)
{
    if(index >= KATMED_IMAGE_COUNT || profile >= katmed_generated_catalog.profile_count)
        return NULL;
    return &katmed_generated_catalog.images[index].variants[profile];
}

KatmedImageStatus katmed_images_validate_catalog(const KatmedGeneratedCatalog *cat, uint32_t partition_size)
{
    if(cat == NULL || cat->profile_count == 0 || cat->profile_count > KATMED_IMAGE_MAX_PROFILES ||
       cat->pack_bytes == 0 || cat->pack_bytes > partition_size) return KATMED_IMAGE_BAD_CATALOG;
    uint8_t nonzero = 0;
    for(unsigned i = 0; i < 32; ++i) nonzero |= cat->catalog_sha256[i];
    if(!nonzero) return KATMED_IMAGE_BAD_CATALOG;
    for(uint32_t i = 0; i < KATMED_IMAGE_COUNT; ++i) {
        uint64_t id = cat->images[i].logical_id;
        if(id == 0) return KATMED_IMAGE_BAD_CATALOG;
        for(uint32_t j = 0; j < i; ++j)
            if(cat->images[j].logical_id == id) return KATMED_IMAGE_BAD_CATALOG;
    }
    uint32_t end = 0;
    for(uint32_t p = 0; p < cat->profile_count; ++p) {
        uint16_t width = p == 0 && cat->profile_count == 1 ? 240 : p == 0 ? 172 : 320;
        uint16_t height = p == 0 && cat->profile_count == 1 ? 240 : p == 0 ? 320 : 172;
        for(uint32_t i = 0; i < KATMED_IMAGE_COUNT; ++i) {
            const KatmedImageVariant *v = &cat->images[i].variants[p];
            if(v->offset != end || (end & 3u) != 0u || v->length < 4u ||
               v->width != width || v->height != height ||
               !within(v->offset, v->length, cat->pack_bytes) ||
               !within(v->offset, v->length, partition_size)) return KATMED_IMAGE_BAD_CATALOG;
            if(v->length > UINT32_MAX - 3u - end) return KATMED_IMAGE_BAD_CATALOG;
            end = (end + v->length + 3u) & ~3u;
        }
    }
    return end == cat->pack_bytes ? KATMED_IMAGE_OK : KATMED_IMAGE_BAD_CATALOG;
}

KatmedImageStatus katmed_images_mount_storage(KatmedStorage storage)
{
    katmed_images_unmount();
    if(storage.read == NULL || storage.partition_size == 0) {
        last_error = KATMED_IMAGE_UNAVAILABLE;
        return last_error;
    }
    mounted_storage = storage;
    if(katmed_images_validate_catalog(&katmed_generated_catalog, storage.partition_size) != KATMED_IMAGE_OK) {
        last_error = KATMED_IMAGE_BAD_CATALOG;
        memset(&mounted_storage, 0, sizeof(mounted_storage));
        return last_error;
    }
    uint8_t hash[32];
    uint64_t start = monotonic_us();
    KatmedImageStatus result = hash_range(0, katmed_generated_catalog.pack_bytes, hash);
    if(result == KATMED_IMAGE_OK &&
       memcmp(hash, katmed_generated_catalog.pack_sha256, sizeof(hash)) != 0)
        result = KATMED_IMAGE_BAD_HASH;
    pack_hash_timing = complete_timing(start, result);
    if(result != KATMED_IMAGE_OK) {
        last_error = result;
        memset(&mounted_storage, 0, sizeof(mounted_storage));
        return result;
    }
    mounted = true;
    last_error = KATMED_IMAGE_OK;
    return KATMED_IMAGE_OK;
}

KatmedImageStatus katmed_images_read_variant(uint32_t index, uint32_t profile,
                                             uint32_t position, uint8_t *dst,
                                             size_t wanted, size_t *actual)
{
    if(actual) *actual = 0;
    if(!mounted) return KATMED_IMAGE_UNAVAILABLE;
    const KatmedImageVariant *v = katmed_images_variant(index, profile);
    if(v == NULL) return KATMED_IMAGE_BAD_ID;
    if(wanted > UINT32_MAX || position > v->length ||
       wanted > v->length - position || !within(v->offset, v->length, mounted_storage.partition_size) ||
       position > UINT32_MAX - v->offset) return KATMED_IMAGE_BAD_RANGE;
    KatmedImageStatus status = read_exact(v->offset + position, dst, wanted);
    if(status == KATMED_IMAGE_OK && actual) *actual = wanted;
    if(status != KATMED_IMAGE_OK) katmed_images_record_error(status);
    return status;
}

KatmedImageStatus katmed_images_validate_variant(uint32_t index, uint32_t profile)
{
    const KatmedImageVariant *v = katmed_images_variant(index, profile);
    if(!mounted) return KATMED_IMAGE_UNAVAILABLE;
    if(v == NULL) return KATMED_IMAGE_BAD_ID;
    uint8_t hash[32];
    uint64_t start = monotonic_us();
    KatmedImageStatus status = hash_range(v->offset, v->length, hash);
    if(status == KATMED_IMAGE_OK && memcmp(hash, v->sha256, sizeof(hash)) != 0)
        status = KATMED_IMAGE_BAD_HASH;
    variant_hash_timing = complete_timing(start, status);
    if(status != KATMED_IMAGE_OK) {
        katmed_images_record_error(status);
        return status;
    }
    return KATMED_IMAGE_OK;
}

uint32_t katmed_images_profile_for_orientation(Orientation orientation)
{
    if((unsigned)orientation > (unsigned)ORIENTATION_270) return UINT32_MAX;
    if(katmed_generated_catalog.profile_count == 1u) return 0u;
    return orientation == ORIENTATION_90 || orientation == ORIENTATION_270 ? 1u : 0u;
}

KatmedImageStatus katmed_images_select(uint32_t logical_index, Orientation orientation,
                                       KatmedImageSelection *out)
{
    if(out == NULL) return KATMED_IMAGE_BAD_ID;
    memset(out, 0, sizeof(*out));
    out->show_neutral_fallback = true;
    uint32_t profile = katmed_images_profile_for_orientation(orientation);
    if(profile == UINT32_MAX) {
        out->status = KATMED_IMAGE_BAD_ID;
        return out->status;
    }
    if(logical_index >= KATMED_IMAGE_COUNT) {
#ifdef ESP_PLATFORM
        KatmedUserImageInfo info;
        KatmedImageStatus status = katmed_image_store_list(
            logical_index - KATMED_IMAGE_COUNT, &info);
        if(status != KATMED_IMAGE_OK) {
            out->status = status;
            return status;
        }
        out->logical_id = (UINT64_C(1) << 63u) | info.id;
        out->width = info.width;
        out->height = info.height;
        int n = snprintf(out->lvgl_path, sizeof(out->lvgl_path), "K:/u%08" PRIx32 ".jpg",
                         info.id);
        if(n < 0 || (size_t)n >= sizeof(out->lvgl_path)) {
            out->status = KATMED_IMAGE_BAD_CATALOG;
            return out->status;
        }
        out->status = KATMED_IMAGE_OK;
        out->show_neutral_fallback = false;
        return KATMED_IMAGE_OK;
#else
        out->status = KATMED_IMAGE_BAD_ID;
        return out->status;
#endif
    }
    KatmedImageStatus status = katmed_images_validate_variant(logical_index, profile);
    out->status = status;
    if(status != KATMED_IMAGE_OK) return status;
    const KatmedImageVariant *v = katmed_images_variant(logical_index, profile);
    out->logical_id = katmed_generated_catalog.images[logical_index].logical_id;
    out->width = v->width;
    out->height = v->height;
    int n = snprintf(out->lvgl_path, sizeof(out->lvgl_path), "K:/%02u_%u.jpg",
                     (unsigned)logical_index, (unsigned)profile);
    if(n < 0 || (size_t)n >= sizeof(out->lvgl_path)) {
        out->status = KATMED_IMAGE_BAD_CATALOG;
        return out->status;
    }
    out->show_neutral_fallback = false;
    return KATMED_IMAGE_OK;
}
