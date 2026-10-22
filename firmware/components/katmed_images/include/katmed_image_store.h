#ifndef KATMED_IMAGE_STORE_H
#define KATMED_IMAGE_STORE_H

#include "katmed_images.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Upload bounds are enforced before a temporary file is created. The image
 * decoder remains responsible for rejecting unsupported or oversized JPEGs. */
#define KATMED_IMAGE_UPLOAD_MAX_BYTES (128u * 1024u)
#define KATMED_IMAGE_UPLOAD_MAX_WIDTH  800u
#define KATMED_IMAGE_UPLOAD_MAX_HEIGHT 800u

typedef struct {
    uint32_t token;
} KatmedImageUpload;

typedef struct {
    uint32_t token;
} KatmedImageReader;

typedef enum {
    KATMED_IMAGE_STORE_SEEK_SET = 0,
    KATMED_IMAGE_STORE_SEEK_CUR,
    KATMED_IMAGE_STORE_SEEK_END
} KatmedImageStoreWhence;

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

/* Mounts the dedicated user-image partition without formatting it. A missing,
 * corrupt, or unsupported filesystem is reported and left untouched. */
KatmedImageStatus katmed_image_store_mount(void);
bool katmed_image_store_quiescent(void);
void katmed_image_store_unmount(void);

/* Uploads use generated IDs, never caller-provided file paths. Begin creates
 * a private temporary file; commit validates JPEG bounds and atomically
 * publishes it; abort removes only that temporary file. */
KatmedImageStatus katmed_image_store_upload_begin(uint32_t expected_bytes,
                                                   KatmedImageUpload *out);
KatmedImageStatus katmed_image_store_upload_write(KatmedImageUpload upload,
                                                    const uint8_t *bytes,
                                                    size_t length);
KatmedImageStatus katmed_image_store_upload_commit(KatmedImageUpload upload,
                                                     KatmedUserImageInfo *out);
KatmedImageStatus katmed_image_store_upload_abort(KatmedImageUpload upload);

/* Returns a stable read-only reference to a standard file in the shared
 * filesystem. Never copies or overwrites a standard file. */
KatmedImageStatus katmed_image_store_import_catalog(uint32_t index,
                                                      uint32_t profile,
                                                      KatmedUserImageInfo *out);

/* The image/slide-show layer reads by generated ID. It never receives a path
 * from the web UI and does not duplicate filesystem access or validation. */
KatmedImageStatus katmed_image_store_open_read(uint32_t id,
                                                KatmedImageReader *out,
                                                KatmedUserImageInfo *info);
KatmedImageStatus katmed_image_store_read(KatmedImageReader reader,
                                          uint8_t *buffer, size_t wanted,
                                          size_t *actual);
KatmedImageStatus katmed_image_store_seek(KatmedImageReader reader,
                                          uint32_t offset,
                                          KatmedImageStoreWhence whence);
KatmedImageStatus katmed_image_store_close_read(KatmedImageReader reader);

KatmedImageStatus katmed_image_store_get(uint32_t id, KatmedUserImageInfo *out);
KatmedImageStatus katmed_image_store_list(uint32_t index, KatmedUserImageInfo *out);
KatmedImageStatus katmed_image_store_remove(uint32_t id);
KatmedImageStatus katmed_image_store_stats(KatmedImageStoreStats *out);
/* Removes only generated uploaded-image and abandoned-upload files. Never
 * formats or touches factory-images in the same filesystem. Idempotent. */
KatmedImageStatus katmed_image_store_clear_user_data(void);

#ifdef __cplusplus
}
#endif
#endif
