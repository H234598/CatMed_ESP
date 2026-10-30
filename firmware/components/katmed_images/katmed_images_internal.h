#ifndef KATMED_IMAGES_INTERNAL_H
#define KATMED_IMAGES_INTERNAL_H

#include "katmed_images.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KATMED_IMAGE_COUNT 72u
#define KATMED_IMAGE_MAX_PROFILES 2u
#define KATMED_IMAGE_READ_CHUNK 512u
#define KATMED_IMAGE_MAX_OPEN_FILES 4u

typedef struct {
    uint32_t offset;
    uint32_t length;
    uint16_t width;
    uint16_t height;
    uint8_t sha256[32];
} KatmedImageVariant;

typedef struct {
    uint64_t logical_id;
    KatmedImageVariant variants[KATMED_IMAGE_MAX_PROFILES];
} KatmedImageEntry;

typedef struct {
    uint32_t profile_count;
    uint32_t pack_bytes;
    uint8_t catalog_sha256[32];
    uint8_t pack_sha256[32];
    KatmedImageEntry images[KATMED_IMAGE_COUNT];
} KatmedGeneratedCatalog;

extern const KatmedGeneratedCatalog katmed_generated_catalog;

/* Returning fewer than `wanted` bytes is an I/O error, not a valid read. */
typedef bool (*KatmedStorageRead)(void *context, uint32_t offset, uint8_t *dst,
                                  size_t wanted, size_t *actual);
typedef struct {
    KatmedImageIoStage stage;
    KatmedImageStatus status;
} KatmedImageIoFailure;
/* Optional detail from the same read call; no global last-error race. */
typedef bool (*KatmedStorageReadDetailed)(void *context, uint32_t offset, uint8_t *dst,
    size_t wanted, size_t *actual, KatmedImageIoFailure *failure);
typedef struct {
    void *context;
    uint32_t partition_size;
    KatmedStorageRead read;
    KatmedStorageReadDetailed read_detailed;
} KatmedStorage;

KatmedImageStatus katmed_images_mount_storage(KatmedStorage storage);
KatmedImageStatus katmed_images_validate_catalog(const KatmedGeneratedCatalog *catalog,
                                                 uint32_t partition_size);
KatmedImageStatus katmed_images_read_variant(uint32_t index, uint32_t profile,
                                             uint32_t position, uint8_t *dst,
                                             size_t wanted, size_t *actual);
KatmedImageStatus katmed_images_validate_variant(uint32_t index, uint32_t profile);
const KatmedImageVariant *katmed_images_variant(uint32_t index, uint32_t profile);
uint32_t katmed_images_profile_for_orientation(Orientation orientation);
void katmed_images_record_error(KatmedImageStatus status);
void katmed_images_clear_error(void);
/* Changes on mount/remount/unmount, never a validation-success flag. */
uint32_t katmed_images_asset_generation(void);
void katmed_images_assets_changed(void);
uint32_t katmed_images_open_file_count(void);
void katmed_images_render_trace(KatmedRenderInnerPhase phase, bool begin,
                                uint8_t outcome);

#ifdef __cplusplus
}
#endif

#endif
