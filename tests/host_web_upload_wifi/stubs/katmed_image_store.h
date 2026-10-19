#ifndef TEST_KATMED_IMAGE_STORE_H
#define TEST_KATMED_IMAGE_STORE_H

/* Checked-in host contract fixture matching A-owned katmed_image_store.h.
 * Keep types, status names, limits, and signatures synchronized with it. */
#include <stddef.h>
#include <stdint.h>

#define KATMED_IMAGE_UPLOAD_MAX_BYTES (128u * 1024u)
#define KATMED_IMAGE_UPLOAD_MAX_WIDTH 800u
#define KATMED_IMAGE_UPLOAD_MAX_HEIGHT 800u

typedef enum {
    KATMED_IMAGE_OK = 0,
    KATMED_IMAGE_UNAVAILABLE,
    KATMED_IMAGE_BAD_CATALOG,
    KATMED_IMAGE_BAD_RANGE,
    KATMED_IMAGE_BAD_HASH,
    KATMED_IMAGE_IO_ERROR,
    KATMED_IMAGE_BAD_ID,
    KATMED_IMAGE_NOT_FOUND,
    KATMED_IMAGE_NO_SPACE,
    KATMED_IMAGE_BUSY,
    KATMED_IMAGE_BAD_STATE
} KatmedImageStatus;

typedef struct { uint32_t token; } KatmedImageUpload;
typedef struct { uint32_t token; } KatmedImageReader;
typedef struct {
    uint32_t id;
    uint32_t bytes;
    uint16_t width;
    uint16_t height;
} KatmedUserImageInfo;
typedef struct {
    uint32_t total_bytes;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint32_t image_count;
} KatmedImageStoreStats;

KatmedImageStatus katmed_image_store_upload_begin(uint32_t expected_bytes,
                                                   KatmedImageUpload *out);
KatmedImageStatus katmed_image_store_upload_write(KatmedImageUpload upload,
                                                   const uint8_t *bytes,
                                                   size_t length);
KatmedImageStatus katmed_image_store_upload_commit(KatmedImageUpload upload,
                                                    KatmedUserImageInfo *out);
KatmedImageStatus katmed_image_store_upload_abort(KatmedImageUpload upload);
KatmedImageStatus katmed_image_store_import_catalog(uint32_t index, uint32_t profile,
                                                     KatmedUserImageInfo *out);
KatmedImageStatus katmed_image_store_open_read(uint32_t id, KatmedImageReader *out,
                                                KatmedUserImageInfo *info);
KatmedImageStatus katmed_image_store_read(KatmedImageReader reader, uint8_t *buffer,
                                           size_t wanted, size_t *actual);
KatmedImageStatus katmed_image_store_close_read(KatmedImageReader reader);
KatmedImageStatus katmed_image_store_list(uint32_t index,
                                          KatmedUserImageInfo *out);
KatmedImageStatus katmed_image_store_remove(uint32_t id);
KatmedImageStatus katmed_image_store_stats(KatmedImageStoreStats *out);

#endif
