#include "katmed_images_internal.h"
#include "katmed_image_store.h"
#include "katmed_security_layout.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

/* Compatibility view for the existing bounded streaming validator: offsets
 * refer to the generated sequence of individual files, not a stored blob.
 * LVGL itself opens both standard files and uploads through image_store. */
static KatmedImageReader active_reader;
static uint32_t active_id;
static SemaphoreHandle_t validation_lock;

static void close_validation_reader(void)
{
    if (active_id != 0u) (void)katmed_image_store_close_read(active_reader);
    active_id = 0u;
}

static bool file_read_locked(void *context, uint32_t offset, uint8_t *dst,
                      size_t wanted, size_t *actual)
{
    (void)context;
    *actual = 0;
    while (*actual < wanted) {
        const KatmedImageVariant *v = NULL;
        uint32_t id = 0;
        for (uint32_t p = 0; p < katmed_generated_catalog.profile_count && v == NULL; ++p) {
            for (uint32_t i = 0; i < KATMED_IMAGE_COUNT; ++i) {
                const KatmedImageVariant *candidate = katmed_images_variant(i, p);
                const uint32_t padded = (candidate->length + 3u) & ~3u;
                if (offset >= candidate->offset && offset - candidate->offset < padded) {
                    v = candidate;
                    id = UINT32_C(0x80000000) | (i << 1u) | p;
                    break;
                }
            }
        }
        if (v == NULL) { close_validation_reader(); return false; }
        uint32_t position = offset - v->offset;
        size_t remaining = wanted - *actual;
        if (position >= v->length) {
            size_t take = ((v->length + 3u) & ~3u) - position;
            if (take > remaining) take = remaining;
            memset(dst + *actual, 0xff, take);
            offset += (uint32_t)take;
            *actual += take;
            continue;
        }
        if (active_id != id) {
            close_validation_reader();
            KatmedUserImageInfo info;
            if (katmed_image_store_open_read(id, &active_reader, &info) != KATMED_IMAGE_OK)
                return false;
            active_id = id;
            if (info.bytes != v->length) { close_validation_reader(); return false; }
        }
        if (katmed_image_store_seek(active_reader, position, KATMED_IMAGE_STORE_SEEK_SET) != KATMED_IMAGE_OK) {
            close_validation_reader(); return false;
        }
        size_t take = v->length - position;
        if (take > remaining) take = remaining;
        size_t got = 0;
        if (katmed_image_store_read(active_reader, dst + *actual, take, &got) != KATMED_IMAGE_OK || got != take) {
            close_validation_reader(); return false;
        }
        offset += (uint32_t)take;
        *actual += take;
        if (position + take == v->length) close_validation_reader();
    }
    return true;
}

/* HTTP catalogue references and the serialized LVGL thread can validate at
 * the same time. Keep the one bounded validation reader/cache atomic. */
static bool file_read(void *context, uint32_t offset, uint8_t *dst,
                      size_t wanted, size_t *actual)
{
    if (validation_lock == NULL || xSemaphoreTake(validation_lock, portMAX_DELAY) != pdTRUE)
        return false;
    bool ok = file_read_locked(context, offset, dst, wanted, actual);
    (void)xSemaphoreGive(validation_lock);
    return ok;
}

KatmedImageStatus katmed_images_mount_partition(void)
{
    if (validation_lock == NULL) validation_lock = xSemaphoreCreateMutex();
    if (validation_lock == NULL) return KATMED_IMAGE_UNAVAILABLE;
    close_validation_reader();
    KatmedImageStatus status = katmed_image_store_mount();
    if (status != KATMED_IMAGE_OK) return status;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
        KATMED_USER_IMAGE_PARTITION_LABEL);
    if (partition == NULL) return KATMED_IMAGE_UNAVAILABLE;
    KatmedStorage storage = {.context=NULL, .partition_size=partition->size, .read=file_read};
    status = katmed_images_mount_storage(storage);
    close_validation_reader();
    return status;
}
