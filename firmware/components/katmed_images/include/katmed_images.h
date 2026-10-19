#ifndef KATMED_IMAGES_H
#define KATMED_IMAGES_H

#include "katmed_types.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

typedef struct {
    KatmedImageStatus status;
    bool show_neutral_fallback;
    uint64_t logical_id;
    uint16_t width;
    uint16_t height;
    /* Owned by this value; valid until it is overwritten by another selection. */
    char lvgl_path[24];
} KatmedImageSelection;

/* Snapshot of one completed operation on the serialized UI thread. A failed
 * operation is measured too; measured=false means it has not run yet. Values
 * are elapsed microseconds, not a wall-clock timestamp or a device benchmark. */
typedef struct {
    uint64_t elapsed_us;
    KatmedImageStatus status;
    bool measured;
} KatmedImageTimingSample;

/* Must be called before UI uses the image catalogue. It verifies the entire
 * read-only image pack using a 512-byte buffer; no image is copied into RAM. */
KatmedImageStatus katmed_images_mount_partition(void);
void katmed_images_unmount(void);
KatmedImageStatus katmed_images_select(uint32_t logical_index, Orientation orientation,
                                       KatmedImageSelection *out);
const uint8_t *katmed_images_catalog_sha256(void);
uint32_t katmed_images_count(void);
KatmedImageStatus katmed_images_last_error(void);
KatmedImageTimingSample katmed_images_pack_hash_timing(void);
KatmedImageTimingSample katmed_images_variant_hash_timing(void);
KatmedImageTimingSample katmed_images_jpeg_decode_timing(void);
/* Saturating count of failed completed presentations, including a later
 * transition to the neutral fallback after a successful initial draw. */
uint32_t katmed_images_failed_presentations(void);

/* Register once after lv_init(), on the serialized LVGL/UI thread. The driver
 * has four fixed file slots. A selection's path is a regular LVGL JPEG source;
 * the pinned LVGL TJpgD decoder reads only MCU tiles. */
void katmed_images_register_lvgl_fs(void);

/* The caller owns `image` and `fallback_label`. The latter covers the image
 * area in neutral grey when selection or decoding fails. Neither function
 * touches AppState, alarm rendering, or medication deadlines. One active
 * image/fallback pair is tracked. Late draw errors select the already-created
 * fallback at LV_EVENT_REFR_READY, outside LVGL's rendering phase. */
KatmedImageStatus katmed_images_present(lv_obj_t *image, lv_obj_t *fallback_label,
                                        uint32_t logical_index, Orientation orientation);

#ifdef __cplusplus
}
#endif
#endif
