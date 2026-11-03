#define _POSIX_C_SOURCE 200809L
#include "katmed_images_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef DRAW_LINES
#define DRAW_LINES 16u
#endif

typedef struct {
    FILE *file;
    uint32_t bytes;
    uint64_t reads;
    uint64_t fail_at_read;
    bool corrupt;
    size_t largest_read;
} Pack;

typedef struct {
    unsigned begin[KATMED_RENDER_INNER_COUNT];
    unsigned end[KATMED_RENDER_INNER_COUNT];
    uint8_t last[KATMED_RENDER_INNER_COUNT];
} RenderEvents;

static void observe_render(void *context, KatmedRenderInnerPhase phase,
                           bool begin, uint8_t outcome)
{
    RenderEvents *events=context;
    assert((unsigned)phase<KATMED_RENDER_INNER_COUNT);
    if (begin) ++events->begin[phase];
    else { ++events->end[phase]; events->last[phase]=outcome; }
}

static bool read_pack(void *context, uint32_t offset, uint8_t *dst,
                      size_t wanted, size_t *actual)
{
    Pack *pack = context;
    *actual = 0;
    ++pack->reads;
    if((pack->fail_at_read && pack->reads >= pack->fail_at_read) ||
       offset > pack->bytes || wanted > pack->bytes - offset ||
       fseek(pack->file, (long)offset, SEEK_SET) != 0) return false;
    *actual = fread(dst, 1, wanted, pack->file);
    if(pack->corrupt && *actual) dst[0] ^= 1u;
    if(wanted > pack->largest_read) pack->largest_read = wanted;
    return *actual == wanted;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static uint64_t monotonic_us(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

int main(int argc, char **argv)
{
    assert(argc == 4 || argc == 5);
    int width = atoi(argv[1]);
    int height = atoi(argv[2]);
    assert((width == 240 && height == 240) || (width == 172 && height == 320) ||
           (width == 320 && height == 172));
    assert(!katmed_images_pack_hash_timing().measured);
    assert(!katmed_images_variant_hash_timing().measured);
    assert(!katmed_images_jpeg_decode_timing().measured);

    Pack pack = {0};
    pack.file = fopen(argv[3], "rb");
    assert(pack.file != NULL);
    assert(fseek(pack.file, 0, SEEK_END) == 0);
    long size = ftell(pack.file);
    assert(size > 0 && size <= UINT32_MAX);
    pack.bytes = (uint32_t)size;
    KatmedStorage storage = {&pack, pack.bytes, read_pack, NULL};
    assert(katmed_images_mount_storage(storage) == KATMED_IMAGE_OK);
    RenderEvents events={0};
    katmed_images_set_render_trace(observe_render,&events);
    KatmedImageTimingSample sample = katmed_images_pack_hash_timing();
    assert(sample.measured && sample.status == KATMED_IMAGE_OK && sample.elapsed_us > 0);
    assert(pack.largest_read <= KATMED_IMAGE_READ_CHUNK);

    Orientation orientation = width > height && width != 240 ? ORIENTATION_90 : ORIENTATION_0;
    KatmedImageSelection selection;
    assert(katmed_images_select(0, orientation, &selection) == KATMED_IMAGE_OK);
    assert(events.begin[KATMED_RENDER_INNER_VARIANT_HASH]==1 &&
           events.end[KATMED_RENDER_INNER_VARIANT_HASH]==1 &&
           events.last[KATMED_RENDER_INNER_VARIANT_HASH]==KATMED_IMAGE_OK);
    sample = katmed_images_variant_hash_timing();
    assert(sample.measured && sample.status == KATMED_IMAGE_OK && sample.elapsed_us > 0);
    pack.corrupt = true;
    assert(katmed_images_select(0, orientation, &selection) == KATMED_IMAGE_BAD_HASH);
    sample = katmed_images_variant_hash_timing();
    assert(sample.measured && sample.status == KATMED_IMAGE_BAD_HASH);
    pack.corrupt = false;

    static uint8_t pixels[320u * 120u * 2u];
    lv_init();
    lv_display_t *display = lv_display_create(width, height);
    assert(display != NULL);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, (uint32_t)width * DRAW_LINES * 2u,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    katmed_images_register_lvgl_fs();
    lv_obj_t *image = lv_image_create(lv_screen_active());
    lv_obj_t *fallback = lv_label_create(lv_screen_active());
    assert(image && fallback);
    assert(katmed_images_present(image, fallback, 0, orientation) == KATMED_IMAGE_OK);
    assert(events.begin[KATMED_RENDER_INNER_IMAGE_PRESENT]==1 &&
           events.end[KATMED_RENDER_INNER_IMAGE_PRESENT]==1 &&
           events.begin[KATMED_RENDER_INNER_IMAGE_SELECT]==1 &&
           events.end[KATMED_RENDER_INNER_IMAGE_SELECT]==1 &&
           events.begin[KATMED_RENDER_INNER_JPEG_DECODE]==1 &&
           events.end[KATMED_RENDER_INNER_JPEG_DECODE]==1 &&
           events.last[KATMED_RENDER_INNER_IMAGE_PRESENT]==KATMED_IMAGE_OK);
    sample = katmed_images_jpeg_decode_timing();
    assert(sample.measured && sample.status == KATMED_IMAGE_OK && sample.elapsed_us > 0);
    assert(katmed_images_open_file_count() == 0);
    assert(!lv_obj_is_hidden(image));
    assert(katmed_images_present(image, fallback, KATMED_IMAGE_COUNT, orientation) ==
           KATMED_IMAGE_BAD_ID);
    assert(events.begin[KATMED_RENDER_INNER_IMAGE_PRESENT]==2 &&
           events.end[KATMED_RENDER_INNER_IMAGE_PRESENT]==2 &&
           events.begin[KATMED_RENDER_INNER_IMAGE_SELECT]==2 &&
           events.end[KATMED_RENDER_INNER_IMAGE_SELECT]==2 &&
           events.begin[KATMED_RENDER_INNER_JPEG_DECODE]==1 &&
           events.last[KATMED_RENDER_INNER_IMAGE_SELECT]==KATMED_IMAGE_BAD_ID &&
           events.last[KATMED_RENDER_INNER_IMAGE_PRESENT]==KATMED_IMAGE_BAD_ID);
    assert(!katmed_images_jpeg_decode_timing().measured);
    assert(katmed_images_open_file_count() == 0);
    assert(katmed_images_present(image, fallback, 0, orientation) == KATMED_IMAGE_OK);

    /* The real partial renderer must draw the active JPEG again when the
     * translucent UI above it moves. Count actual pack reads at that boundary. */
    uint64_t reads_before_redraw = pack.reads;
    lv_obj_invalidate(image);
    uint64_t redraw_started_us = monotonic_us();
    lv_timer_handler();
    uint64_t redraw_us = monotonic_us() - redraw_started_us;
    uint64_t redraw_reads = pack.reads - reads_before_redraw;
    fprintf(stderr, "jpeg_redraw_reads=%llu redraw_us=%llu buffer_lines=%u profile=%dx%d\n",
            (unsigned long long)redraw_reads, (unsigned long long)redraw_us,
            DRAW_LINES, width, height);
    if(argc == 5) {
        unsigned long long max_reads = strtoull(argv[4], NULL, 10);
        assert(redraw_reads <= max_reads);
    }

    /* First image hash succeeds; the next storage access fails during the
     * actual decoder attempt. The timing must describe the failed attempt. */
    const KatmedImageVariant *variant = katmed_images_variant(1,
        katmed_images_profile_for_orientation(orientation));
    assert(variant != NULL);
    uint64_t hash_reads = (variant->length + KATMED_IMAGE_READ_CHUNK - 1u) /
                          KATMED_IMAGE_READ_CHUNK;
    pack.fail_at_read = pack.reads + hash_reads + 1u;
    assert(katmed_images_present(image, fallback, 1, orientation) == KATMED_IMAGE_IO_ERROR);
    sample = katmed_images_jpeg_decode_timing();
    assert(sample.measured && sample.status == KATMED_IMAGE_IO_ERROR);
    sample = katmed_images_variant_hash_timing();
    assert(sample.measured && sample.status == KATMED_IMAGE_IO_ERROR);
    assert(katmed_images_open_file_count() == 0);
    assert(lv_obj_is_hidden(image));
    assert(!lv_obj_is_hidden(fallback));

    pack.fail_at_read = 0;
    assert(katmed_images_present(image, fallback, 1, orientation) == KATMED_IMAGE_OK);
    sample = katmed_images_jpeg_decode_timing();
    assert(sample.measured && sample.status == KATMED_IMAGE_OK);
    assert(katmed_images_open_file_count() == 0);
    lv_display_delete(display);
    lv_deinit();

    katmed_images_unmount();
    assert(!katmed_images_pack_hash_timing().measured);
    assert(!katmed_images_variant_hash_timing().measured);
    pack.corrupt = true;
    assert(katmed_images_mount_storage(storage) == KATMED_IMAGE_BAD_HASH);
    sample = katmed_images_pack_hash_timing();
    assert(sample.measured && sample.status == KATMED_IMAGE_BAD_HASH);
    pack.corrupt = false;
    pack.fail_at_read = pack.reads + 1u;
    assert(katmed_images_mount_storage(storage) == KATMED_IMAGE_IO_ERROR);
    sample = katmed_images_pack_hash_timing();
    assert(sample.measured && sample.status == KATMED_IMAGE_IO_ERROR);
    pack.fail_at_read = 0;
    katmed_images_set_render_trace(NULL,NULL);
    katmed_images_unmount();
    assert(fclose(pack.file) == 0);
    printf("image_metrics=PASS profile=%dx%d reads=%llu\n", width, height,
           (unsigned long long)pack.reads);
    return 0;
}
