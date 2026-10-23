#ifndef ESP_PLATFORM
#define _POSIX_C_SOURCE 200809L
#endif
#include "katmed_images_internal.h"
#include "katmed_image_store.h"
#include "katmed_images_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "katmed_source_export.h"
#endif
#ifdef ESP_PLATFORM
#include <inttypes.h>
#endif
#include "src/image/lv_image_decoder_private.h"
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <time.h>
#endif

typedef struct {
    uint8_t index;
    uint8_t profile;
    uint32_t position;
    uint32_t length;
    KatmedImageReader reader;
    KatmedImageStatus sticky;
    FILE *render_cache;
    bool user_image;
    bool used;
} ImageFile;

static lv_fs_drv_t fs_driver;
static bool registered;
static uint32_t open_files;
static ImageFile file_pool[KATMED_IMAGE_MAX_OPEN_FILES];
static lv_obj_t *active_image;
static lv_obj_t *active_fallback;
static lv_display_t *active_display;
static lv_image_decoder_info_f_t tjpgd_info;
static lv_image_decoder_open_f_t tjpgd_open;
static lv_image_decoder_get_area_cb_t tjpgd_get_area;
static lv_image_decoder_info_f_t bin_info;
static lv_image_decoder_open_f_t bin_open;
static lv_image_decoder_get_area_cb_t bin_get_area;
static KatmedImageTimingSample jpeg_decode_timing;
static uint32_t failed_presentations;

/* One disposable file for the active scene, never a multi-image or RAM cache.
 * RGB888 preserves TJpgD's pre-alpha-blend values exactly. The native BIN
 * decoder reads one row at a time (LV_BIN_DECODER_RAM_LOAD must remain off). */
#if LV_BIN_DECODER_RAM_LOAD
#error "The active background cache requires the native streaming BIN decoder"
#endif
static char render_cache_source[32] = "K:/active-00000000.bin";
static uint32_t render_cache_scene;
static char render_cache_file[64];
static bool render_cache_ready, render_cache_checked;
static uint32_t render_cache_bytes, render_cache_generation;
static uint8_t render_cache_digest[32];
#ifdef ESP_PLATFORM
static uint32_t render_cache_user_id;
static bool parse_user_path(const char *path, uint32_t *id);
#endif

static void clear_render_cache(void)
{
    render_cache_ready = render_cache_checked = false;
    lv_image_cache_drop(render_cache_source);
#ifdef ESP_PLATFORM
    if(render_cache_file[0] && katmed_source_writer_enter()) {
        (void)unlink(render_cache_file);
        katmed_source_writer_leave();
    }
#else
    if(render_cache_file[0]) (void)unlink(render_cache_file);
#endif
    render_cache_file[0] = '\0';
}

static bool hash_render_cache(FILE *file, uint8_t digest[32])
{
    KatmedImageHash hash;
    uint8_t bytes[KATMED_IMAGE_READ_CHUNK];
    if(fseek(file, 0, SEEK_SET) || !katmed_images_hash_start(&hash)) return false;
    uint32_t remaining = render_cache_bytes;
    while(remaining) {
        size_t wanted = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
        if(fread(bytes, 1, wanted, file) != wanted ||
           !katmed_images_hash_update(&hash, bytes, wanted)) {
            katmed_images_hash_free(&hash);
            return false;
        }
        remaining -= (uint32_t)wanted;
    }
    if(fgetc(file) != EOF || ferror(file)) {
        katmed_images_hash_free(&hash);
        return false;
    }
    return katmed_images_hash_finish(&hash, digest);
}

static FILE *begin_render_cache(const KatmedImageSelection *selection)
{
    clear_render_cache();
    /* Existing supported presentations are bounded to the board dimensions. */
    if(!selection->width || !selection->height || selection->width > 320 || selection->height > 320 ||
       (uint32_t)selection->width * selection->height > 240u * 240u)
        return NULL;
    render_cache_bytes = sizeof(lv_image_header_t) + (uint32_t)selection->width * selection->height * 3u;
#ifdef ESP_PLATFORM
    KatmedImageStoreStats stats;
    if(katmed_image_store_stats(&stats) != KATMED_IMAGE_OK ||
       stats.free_bytes < render_cache_bytes + 8192u) return NULL;
    strcpy(render_cache_file, "/katmed/.render-cache.bin");
    FILE *file = fopen(render_cache_file, "wb+");
#else
    strcpy(render_cache_file, "/tmp/katmed-render-XXXXXX");
    int fd = mkstemp(render_cache_file);
    FILE *file = fd < 0 ? NULL : fdopen(fd, "wb+");
    if(fd >= 0 && !file) close(fd);
#endif
    if(!file) { clear_render_cache(); return NULL; }
    lv_image_header_t header = {.magic=LV_IMAGE_HEADER_MAGIC, .cf=LV_COLOR_FORMAT_RGB888,
        .w=selection->width, .h=selection->height, .stride=selection->width * 3u};
    if(fwrite(&header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file); clear_render_cache(); return NULL;
    }
    render_cache_generation = katmed_images_asset_generation();
#ifdef ESP_PLATFORM
    render_cache_user_id = 0;
    (void)parse_user_path(selection->lvgl_path + 2, &render_cache_user_id);
#endif
    return file;
}

static bool cache_decoded_area(FILE *file, const lv_image_decoder_dsc_t *decoder,
                               const lv_area_t *area)
{
    const lv_draw_buf_t *buffer = decoder->decoded;
    if(!buffer || buffer->header.cf != LV_COLOR_FORMAT_RGB888 ||
       area->x1 < 0 || area->y1 < 0 || area->x2 >= decoder->header.w || area->y2 >= decoder->header.h)
        return false;
    size_t row_bytes = (size_t)lv_area_get_width(area) * 3u;
    for(int32_t y=area->y1; y<=area->y2; ++y) {
        long offset = sizeof(lv_image_header_t) + ((long)y * decoder->header.w + area->x1) * 3;
        const uint8_t *row = buffer->data + (size_t)(y-area->y1) * buffer->header.stride;
        if(fseek(file, offset, SEEK_SET) || fwrite(row, 1, row_bytes, file) != row_bytes) return false;
    }
    return true;
}

static bool finish_render_cache(FILE *file, bool complete)
{
    bool ok = complete && !fflush(file) &&
              render_cache_generation == katmed_images_asset_generation() &&
              hash_render_cache(file, render_cache_digest);
    if(fclose(file)) ok = false;
    if(!ok) { clear_render_cache(); return false; }
    ++render_cache_scene;
    snprintf(render_cache_source, sizeof(render_cache_source), "K:/active-%08" PRIx32 ".bin", render_cache_scene);
    render_cache_ready = true;
    render_cache_checked = false;
    return true;
}

static void record_failed_presentation(void)
{
    if(failed_presentations != UINT32_MAX) ++failed_presentations;
}

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

KatmedImageTimingSample katmed_images_jpeg_decode_timing(void) { return jpeg_decode_timing; }
uint32_t katmed_images_failed_presentations(void) { return failed_presentations; }

uint32_t katmed_images_open_file_count(void) { return open_files; }

static bool katmed_source(const lv_image_decoder_dsc_t *dsc)
{
    return dsc->src_type == LV_IMAGE_SRC_FILE && dsc->src != NULL &&
           strncmp((const char *)dsc->src, "K:/", 3) == 0 &&
           strcmp(lv_fs_get_ext(dsc->src), "jpg") == 0;
}

/* Observe the pinned decoder's results, without implementing JPEG decoding.
 * This also catches a late decoder allocation failure with no storage error. */
static lv_result_t monitored_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                                  lv_image_header_t *header)
{
    lv_result_t result = tjpgd_info(decoder, dsc, header);
    if(result != LV_RESULT_OK && katmed_source(dsc))
        katmed_images_record_error(KATMED_IMAGE_IO_ERROR);
    return result;
}

static lv_result_t monitored_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    lv_result_t result = tjpgd_open(decoder, dsc);
    if(result != LV_RESULT_OK && katmed_source(dsc))
        katmed_images_record_error(KATMED_IMAGE_IO_ERROR);
    return result;
}

static lv_result_t monitored_area(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                                  const lv_area_t *full, lv_area_t *area)
{
    /* TJpgD must consume prefix MCUs, but LVGL does not need the JPEG tail
     * below this partial-refresh clip. Keep the native decoder and complete
     * file validation; finish only after a whole MCU row covers the request. */
    if(katmed_source(dsc) && full->y2 >= 0 &&
       full->y2 < (lv_coord_t)dsc->header.h &&
       area->x2 == (lv_coord_t)dsc->header.w - 1 && area->y2 >= full->y2)
        return LV_RESULT_INVALID; /* Normal end of the requested area. */
    bool already_complete = area->x2 == (lv_coord_t)dsc->header.w - 1 &&
                            area->y2 == (lv_coord_t)dsc->header.h - 1;
    lv_result_t result = tjpgd_get_area(decoder, dsc, full, area);
    if(result != LV_RESULT_OK && !already_complete && katmed_source(dsc))
        katmed_images_record_error(KATMED_IMAGE_IO_ERROR);
    return result;
}

/* BIN can fail allocating its context/FILE/row before any driver read occurs.
 * Monitor only our active derived source, not another decoder's normal decline. */
static bool active_bin_source(const lv_image_decoder_dsc_t *dsc)
{
    return dsc->src_type == LV_IMAGE_SRC_FILE && dsc->src != NULL &&
           strcmp(dsc->src, render_cache_source) == 0;
}

static lv_result_t monitored_bin_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                                      lv_image_header_t *header)
{
    lv_result_t result = bin_info(decoder, dsc, header);
    if(result != LV_RESULT_OK && active_bin_source(dsc))
        katmed_images_record_error(KATMED_IMAGE_IO_ERROR);
    return result;
}

static lv_result_t monitored_bin_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    lv_result_t result = bin_open(decoder, dsc);
    if(result != LV_RESULT_OK && active_bin_source(dsc))
        katmed_images_record_error(KATMED_IMAGE_IO_ERROR);
    return result;
}

static lv_result_t monitored_bin_area(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                                      const lv_area_t *full, lv_area_t *area)
{
    /* Native BIN emits one requested row. INVALID immediately after the last
     * successful row is ordinary EOF; a first-row allocation failure is not. */
    bool complete = full->x1 >= 0 && full->y1 >= 0 && full->x1 <= full->x2 &&
                    full->y1 <= full->y2 && full->x2 < (int32_t)dsc->header.w &&
                    full->y2 < (int32_t)dsc->header.h &&
                    area->x1 == full->x1 && area->x2 == full->x2 &&
                    area->y1 == full->y2 && area->y2 == full->y2;
    lv_result_t result = bin_get_area(decoder, dsc, full, area);
    if(result != LV_RESULT_OK && !complete && active_bin_source(dsc))
        katmed_images_record_error(KATMED_IMAGE_IO_ERROR);
    return result;
}

static bool parse_path(const char *path, uint32_t *index, uint32_t *profile)
{
    /* LVGL removes "K:" before calling the driver and leaves the slash. */
    if(path == NULL || strlen(path) != 9u || path[0] != '/' ||
       path[1] < '0' || path[1] > '9' || path[2] < '0' || path[2] > '9' ||
       path[3] != '_' || path[4] < '0' || path[4] > '1' ||
       strcmp(path + 5, ".jpg") != 0) return false;
    *index = (uint32_t)(path[1] - '0') * 10u + (uint32_t)(path[2] - '0');
    *profile = (uint32_t)(path[4] - '0');
    return katmed_images_variant(*index, *profile) != NULL;
}

#ifdef ESP_PLATFORM
static bool parse_user_path(const char *path, uint32_t *id)
{
    if (path == NULL || id == NULL || strlen(path) != 14u || path[0] != '/' ||
        path[1] != 'u' || strcmp(path + 10, ".jpg") != 0) return false;
    uint32_t value = 0;
    for (size_t i = 2u; i < 10u; ++i) {
        unsigned char c = (unsigned char)path[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
        else return false;
        value = (value << 4u) | digit;
    }
    if (value == 0u) return false;
    *id = value;
    return true;
}
#endif

static void *fs_open(lv_fs_drv_t *driver, const char *path, lv_fs_mode_t mode)
{
    LV_UNUSED(driver);
    if(mode != LV_FS_MODE_RD) return NULL;
    if(strcmp(path, render_cache_source + 2) == 0) {
        if(!render_cache_ready || render_cache_generation != katmed_images_asset_generation()) {
            katmed_images_record_error(KATMED_IMAGE_IO_ERROR); return NULL;
        }
#ifdef ESP_PLATFORM
        if(!render_cache_checked && render_cache_user_id) {
            KatmedUserImageInfo info;
            if(katmed_image_store_get(render_cache_user_id, &info) != KATMED_IMAGE_OK) {
                render_cache_ready=false; katmed_images_record_error(KATMED_IMAGE_IO_ERROR); return NULL;
            }
        }
#endif
        ImageFile *slot = NULL;
        for(unsigned i=0; i<KATMED_IMAGE_MAX_OPEN_FILES; ++i)
            if(!file_pool[i].used) { slot=&file_pool[i]; break; }
        if(!slot) return NULL;
        FILE *stream = fopen(render_cache_file, "rb");
        if(!stream) { render_cache_ready=false; katmed_images_record_error(KATMED_IMAGE_IO_ERROR); return NULL; }
        if(!render_cache_checked) {
            uint8_t digest[32];
            if(!hash_render_cache(stream, digest) || memcmp(digest, render_cache_digest, sizeof(digest))) {
                fclose(stream); render_cache_ready=false;
                katmed_images_record_error(KATMED_IMAGE_IO_ERROR); return NULL;
            }
            render_cache_checked = true;
        }
        if(fseek(stream, 0, SEEK_SET)) {
            fclose(stream); katmed_images_record_error(KATMED_IMAGE_IO_ERROR); return NULL;
        }
        *slot = (ImageFile){.used=true, .render_cache=stream, .length=render_cache_bytes};
        ++open_files;
        return slot;
    }
    uint32_t index = 0, profile = 0;
#ifdef ESP_PLATFORM
    uint32_t user_id = 0;
    bool user_image = parse_user_path(path, &user_id);
    if(!user_image && !parse_path(path, &index, &profile)) return NULL;
#else
    if(!parse_path(path, &index, &profile)) return NULL;
#endif
    ImageFile *file = NULL;
    for(unsigned i = 0; i < KATMED_IMAGE_MAX_OPEN_FILES; ++i) {
        if(!file_pool[i].used) {
            file = &file_pool[i];
            break;
        }
    }
    if(file == NULL) {
        katmed_images_record_error(KATMED_IMAGE_UNAVAILABLE);
        return NULL;
    }
#ifdef ESP_PLATFORM
    if (!user_image && katmed_images_validate_variant(index, profile) != KATMED_IMAGE_OK)
        return NULL;
    if (!user_image) user_id = UINT32_C(0x80000000) | (index << 1u) | profile;
    KatmedUserImageInfo info;
    KatmedImageReader reader;
    if (katmed_image_store_open_read(user_id, &reader, &info) != KATMED_IMAGE_OK) return NULL;
    *file = (ImageFile){.position=0u, .length=info.bytes, .reader=reader,
                        .sticky=KATMED_IMAGE_OK, .user_image=true, .used=true};
#else
    if(katmed_images_validate_variant(index, profile) != KATMED_IMAGE_OK) return NULL;
    const KatmedImageVariant *variant = katmed_images_variant(index, profile);
    *file = (ImageFile){.index = (uint8_t)index, .profile = (uint8_t)profile,
                        .position = 0u, .length=variant->length,
                        .sticky = KATMED_IMAGE_OK, .used = true};
#endif
    ++open_files;
    return file;
}

static lv_fs_res_t fs_close(lv_fs_drv_t *driver, void *opaque)
{
    LV_UNUSED(driver);
    for(unsigned i = 0; i < KATMED_IMAGE_MAX_OPEN_FILES; ++i) {
        if(opaque == &file_pool[i] && file_pool[i].used) {
            if(file_pool[i].render_cache && fclose(file_pool[i].render_cache))
                katmed_images_record_error(KATMED_IMAGE_IO_ERROR);
#ifdef ESP_PLATFORM
            if (file_pool[i].user_image &&
                katmed_image_store_close_read(file_pool[i].reader) != KATMED_IMAGE_OK)
                katmed_images_record_error(KATMED_IMAGE_IO_ERROR);
#endif
            file_pool[i].used = false;
            --open_files;
            return LV_FS_RES_OK;
        }
    }
    return LV_FS_RES_INV_PARAM;
}

static lv_fs_res_t fs_read(lv_fs_drv_t *driver, void *opaque, void *buffer,
                           uint32_t wanted, uint32_t *actual)
{
    LV_UNUSED(driver);
    if(actual) *actual = 0;
    if(opaque == NULL || (wanted && buffer == NULL)) return LV_FS_RES_INV_PARAM;
    ImageFile *file = (ImageFile *)opaque;
    if(file->sticky != KATMED_IMAGE_OK) return LV_FS_RES_FS_ERR;
    if(file->render_cache) {
        if(!render_cache_ready || render_cache_generation != katmed_images_asset_generation() ||
           wanted > file->length - file->position) {
            katmed_images_record_error(KATMED_IMAGE_IO_ERROR); return LV_FS_RES_FS_ERR;
        }
        size_t got = fread(buffer, 1, wanted, file->render_cache);
        file->position += (uint32_t)got;
        if(actual) *actual = (uint32_t)got;
        if(got != wanted) { katmed_images_record_error(KATMED_IMAGE_IO_ERROR); return LV_FS_RES_FS_ERR; }
        return LV_FS_RES_OK;
    }
#ifdef ESP_PLATFORM
    if (file->user_image) {
        size_t read_count = 0;
        KatmedImageStatus status = katmed_image_store_read(
            file->reader, (uint8_t *)buffer, wanted, &read_count);
        if (status != KATMED_IMAGE_OK) {
            file->sticky = status;
            katmed_images_record_error(status);
            return LV_FS_RES_FS_ERR;
        }
        file->position += (uint32_t)read_count;
        if (actual) *actual = (uint32_t)read_count;
        return LV_FS_RES_OK;
    }
#endif
    const KatmedImageVariant *variant = katmed_images_variant(file->index, file->profile);
    if(variant == NULL || file->position > variant->length) {
        file->sticky = KATMED_IMAGE_BAD_RANGE;
        katmed_images_record_error(file->sticky);
        return LV_FS_RES_FS_ERR;
    }
    uint32_t available = variant->length - file->position;
    size_t take = wanted < available ? wanted : available;
    size_t read_count = 0;
    KatmedImageStatus status = katmed_images_read_variant(
        file->index, file->profile, file->position, (uint8_t *)buffer, take, &read_count);
    if(status != KATMED_IMAGE_OK || read_count != take) {
        file->sticky = status == KATMED_IMAGE_OK ? KATMED_IMAGE_IO_ERROR : status;
        katmed_images_record_error(file->sticky);
        return LV_FS_RES_FS_ERR;
    }
    file->position += (uint32_t)read_count;
    if(actual) *actual = (uint32_t)read_count;
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek(lv_fs_drv_t *driver, void *opaque, uint32_t position,
                           lv_fs_whence_t whence)
{
    LV_UNUSED(driver);
    if(opaque == NULL) return LV_FS_RES_INV_PARAM;
    ImageFile *file = (ImageFile *)opaque;
    if(file->sticky != KATMED_IMAGE_OK) return LV_FS_RES_FS_ERR;
    const KatmedImageVariant *variant = (file->user_image || file->render_cache) ? NULL :
        katmed_images_variant(file->index, file->profile);
    if(!file->user_image && !file->render_cache && variant == NULL) return LV_FS_RES_FS_ERR;
    uint32_t length = file->length;
    uint64_t next;
    if(whence == LV_FS_SEEK_SET) next = position;
    else if(whence == LV_FS_SEEK_CUR) next = (uint64_t)file->position + position;
    else if(whence == LV_FS_SEEK_END) next = (uint64_t)length + position;
    else next = UINT64_MAX;
    if(next > length) {
        file->sticky = KATMED_IMAGE_BAD_RANGE;
        katmed_images_record_error(file->sticky);
        return LV_FS_RES_INV_PARAM;
    }
    if(file->render_cache && fseek(file->render_cache, (long)next, SEEK_SET)) {
        katmed_images_record_error(KATMED_IMAGE_IO_ERROR); return LV_FS_RES_FS_ERR;
    }
#ifdef ESP_PLATFORM
    if (file->user_image) {
        KatmedImageStoreWhence store_whence = whence == LV_FS_SEEK_SET ? KATMED_IMAGE_STORE_SEEK_SET :
            whence == LV_FS_SEEK_CUR ? KATMED_IMAGE_STORE_SEEK_CUR : KATMED_IMAGE_STORE_SEEK_END;
        KatmedImageStatus status = katmed_image_store_seek(file->reader, position, store_whence);
        if (status != KATMED_IMAGE_OK) {
            file->sticky = status;
            katmed_images_record_error(status);
            return LV_FS_RES_INV_PARAM;
        }
    }
#endif
    file->position = (uint32_t)next;
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_tell(lv_fs_drv_t *driver, void *opaque, uint32_t *position)
{
    LV_UNUSED(driver);
    if(opaque == NULL || position == NULL) return LV_FS_RES_INV_PARAM;
    ImageFile *file = (ImageFile *)opaque;
    if(file->sticky != KATMED_IMAGE_OK) return LV_FS_RES_FS_ERR;
    *position = file->position;
    return LV_FS_RES_OK;
}

void katmed_images_register_lvgl_fs(void)
{
    if(registered) return;
    lv_fs_drv_init(&fs_driver);
    fs_driver.letter = 'K';
    fs_driver.cache_size = 0;
    fs_driver.open_cb = fs_open;
    fs_driver.close_cb = fs_close;
    fs_driver.read_cb = fs_read;
    fs_driver.seek_cb = fs_seek;
    fs_driver.tell_cb = fs_tell;
    lv_fs_drv_register(&fs_driver);
    lv_image_decoder_t *decoder = NULL;
    while((decoder = lv_image_decoder_get_next(decoder)) != NULL) {
        if(decoder->name != NULL && strcmp(decoder->name, "TJPGD") == 0 &&
           decoder->info_cb != NULL && decoder->open_cb != NULL && decoder->get_area_cb != NULL) {
            tjpgd_info = decoder->info_cb;
            tjpgd_open = decoder->open_cb;
            tjpgd_get_area = decoder->get_area_cb;
            lv_image_decoder_set_info_cb(decoder, monitored_info);
            lv_image_decoder_set_open_cb(decoder, monitored_open);
            lv_image_decoder_set_get_area_cb(decoder, monitored_area);
        }
        else if(decoder->name != NULL && strcmp(decoder->name, "BIN") == 0 &&
                decoder->info_cb != NULL && decoder->open_cb != NULL && decoder->get_area_cb != NULL) {
            bin_info = decoder->info_cb;
            bin_open = decoder->open_cb;
            bin_get_area = decoder->get_area_cb;
            lv_image_decoder_set_info_cb(decoder, monitored_bin_info);
            lv_image_decoder_set_open_cb(decoder, monitored_bin_open);
            lv_image_decoder_set_get_area_cb(decoder, monitored_bin_area);
        }
    }
    registered = true;
}

static KatmedImageStatus decode_check(const KatmedImageSelection *selection)
{
    uint64_t start = monotonic_us();
#ifdef ESP_PLATFORM
    bool writer = katmed_source_writer_enter();
#else
    bool writer = true;
#endif
    FILE *cache = writer ? begin_render_cache(selection) : NULL;
    lv_image_decoder_dsc_t decoder;
    if(lv_image_decoder_open(&decoder, selection->lvgl_path, NULL) != LV_RESULT_OK) {
        uint64_t end = monotonic_us();
        jpeg_decode_timing = (KatmedImageTimingSample){
            .elapsed_us = end >= start ? end - start : 0,
            .status = KATMED_IMAGE_IO_ERROR, .measured = true};
        if(cache) { fclose(cache); clear_render_cache(); }
#ifdef ESP_PLATFORM
        if(writer) katmed_source_writer_leave();
#endif
        return KATMED_IMAGE_IO_ERROR;
    }
    lv_area_t full = {0, 0, (lv_coord_t)selection->width - 1,
                      (lv_coord_t)selection->height - 1};
    lv_area_t area = {LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN};
    bool complete = false;
    for(uint32_t i = 0; i < 1280u; ++i) {
        if(lv_image_decoder_get_area(&decoder, &full, &area) != LV_RESULT_OK) break;
        if(cache && !cache_decoded_area(cache, &decoder, &area)) {
            fclose(cache); cache=NULL; clear_render_cache();
        }
        if(area.x2 == (lv_coord_t)selection->width - 1 &&
           area.y2 == (lv_coord_t)selection->height - 1) {
            complete = true;
            break;
        }
    }
    lv_image_decoder_close(&decoder);
    if(cache) (void)finish_render_cache(cache, complete);
#ifdef ESP_PLATFORM
    if(writer) katmed_source_writer_leave();
#endif
    KatmedImageStatus status = complete ? KATMED_IMAGE_OK : KATMED_IMAGE_IO_ERROR;
    uint64_t end = monotonic_us();
    jpeg_decode_timing = (KatmedImageTimingSample){
        .elapsed_us = end >= start ? end - start : 0,
        .status = status, .measured = true};
    return status;
}

static void show_fallback(lv_obj_t *image, lv_obj_t *label)
{
    lv_image_set_src(image, NULL);
    lv_obj_set_hidden(image, true);
    lv_obj_set_hidden(label, false);
}

static void image_event(lv_event_t *event)
{
    lv_obj_t *image = lv_event_get_target_obj(event);
    if(image != active_image) return;
    lv_event_code_t code = lv_event_get_code(event);
    if(code == LV_EVENT_DELETE) {
        clear_render_cache();
        active_image = NULL;
        active_fallback = NULL;
    }
}

static void fallback_event(lv_event_t *event)
{
    if(lv_event_get_target_obj(event) == active_fallback) active_fallback = NULL;
}

static void display_event(lv_event_t *event)
{
    if(lv_event_get_target(event) != active_display) return;
    if(lv_event_get_code(event) == LV_EVENT_REFR_START) render_cache_checked = false;
    if(lv_event_get_code(event) == LV_EVENT_DELETE) {
        active_display = NULL;
    }
    else if(lv_event_get_code(event) == LV_EVENT_REFR_READY && active_image != NULL &&
            active_fallback != NULL && katmed_images_last_error() != KATMED_IMAGE_OK &&
            !lv_obj_is_hidden(active_image)) {
        // LVGL 9.4 forbids invalidation during DRAW_POST/RENDER_READY.
        // REFR_READY runs after rendering and requires no deferred allocation.
        record_failed_presentation();
        show_fallback(active_image, active_fallback);
        clear_render_cache();
    }
}

static void prepare_fallback(lv_obj_t *image, lv_obj_t *label)
{
    if(active_image != NULL && active_image != image)
        lv_obj_remove_event_cb(active_image, image_event);
    if(active_image != image)
        lv_obj_add_event_cb(image, image_event, LV_EVENT_ALL, NULL);
    if(active_fallback != NULL && active_fallback != label)
        lv_obj_remove_event_cb(active_fallback, fallback_event);
    if(active_fallback != label)
        lv_obj_add_event_cb(label, fallback_event, LV_EVENT_DELETE, NULL);
    lv_display_t *display = lv_obj_get_display(image);
    if(active_display != display) {
        if(active_display != NULL)
            lv_display_remove_event_cb_with_user_data(active_display, display_event, NULL);
        lv_display_add_event_cb(display, display_event, LV_EVENT_ALL, NULL);
        active_display = display;
    }
    active_image = image;
    active_fallback = label;
    /* These allocations and styles happen before the decode. A late I/O
     * failure need only flip existing flags and release the image source. */
    /* The UI supplies localized fallback copy before the image request. */
    lv_obj_set_size(label, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(label, lv_color_hex(0x303438), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xf0f0f0), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

KatmedImageStatus katmed_images_present(lv_obj_t *image, lv_obj_t *fallback_label,
                                        uint32_t index, Orientation orientation)
{
    /* A failed selection has no decoder run. Do not expose the preceding
     * scene's duration as if it belonged to this request. */
    jpeg_decode_timing = (KatmedImageTimingSample){0};
    if(image == NULL || fallback_label == NULL) {
        record_failed_presentation();
        return KATMED_IMAGE_BAD_ID;
    }
    prepare_fallback(image, fallback_label);
    clear_render_cache();
    KatmedImageSelection selection;
    KatmedImageStatus status = katmed_images_select(index, orientation, &selection);
    if(status == KATMED_IMAGE_OK) status = decode_check(&selection);
    if(status == KATMED_IMAGE_OK) {
        const char *source = render_cache_ready ? render_cache_source : selection.lvgl_path;
        lv_image_set_src(image, source);
        const void *actual_source = lv_image_get_src(image);
        /* LVGL can retain the previous source when copying a new path fails.
         * A non-NULL source alone therefore does not prove this selection. */
        if(actual_source == NULL || lv_image_src_get_type(actual_source) != LV_IMAGE_SRC_FILE ||
           strcmp((const char *)actual_source, source) != 0)
            status = KATMED_IMAGE_IO_ERROR;
    }
    if(status == KATMED_IMAGE_OK) {
        lv_obj_set_hidden(image, false);
        lv_obj_set_hidden(fallback_label, true);
        katmed_images_clear_error();
    }
    else {
        record_failed_presentation();
        katmed_images_record_error(status);
        show_fallback(image, fallback_label);
        clear_render_cache();
    }
    return status;
}
