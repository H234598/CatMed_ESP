#include "katmed_images_internal.h"
#include "katmed_images.h"
#include "lvgl.h"
#include "src/draw/lv_image_decoder_private.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TJPGD_INFO_WORK_BYTES 4096u

typedef struct {
    FILE *file;
    bool corrupt_jpeg;
    uint32_t corrupt_offset;
} PackContext;

static bool read_pack(void *context, uint32_t offset, uint8_t *output,
                      size_t wanted, size_t *actual)
{
    PackContext *pack = context;
    *actual = 0;
    if(offset > katmed_generated_catalog.pack_bytes ||
       wanted > katmed_generated_catalog.pack_bytes - offset ||
       fseek(pack->file, (long)offset, SEEK_SET) != 0) return false;
    *actual = fread(output, 1, wanted, pack->file);
    /* Damage one SOI byte only after selection and file validation. */
    if(pack->corrupt_jpeg && offset <= pack->corrupt_offset &&
       pack->corrupt_offset - offset < *actual)
        output[pack->corrupt_offset - offset] = 0;
    return *actual == wanted;
}

static lv_image_decoder_t *find_tjpgd(void)
{
    lv_image_decoder_t *decoder = NULL;
    while((decoder = lv_image_decoder_get_next(decoder)) != NULL) {
        if(decoder->name != NULL && strcmp(decoder->name, "TJPGD") == 0)
            return decoder;
    }
    return NULL;
}

static lv_mem_monitor_t memory(void)
{
    lv_mem_monitor_t result;
    lv_mem_monitor(&result);
    return result;
}

static void check_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                       uint16_t width, uint16_t height)
{
    lv_image_header_t header = {0};
    assert(lv_fs_seek(&dsc->file, 0, LV_FS_SEEK_SET) == LV_FS_RES_OK);
    assert(decoder->info_cb(decoder, dsc, &header) == LV_RESULT_OK);
    assert(header.w == width && header.h == height);
    assert(header.cf == LV_COLOR_FORMAT_RAW);
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    uint16_t width = (uint16_t)strtoul(argv[1], NULL, 10);
    uint16_t height = (uint16_t)strtoul(argv[2], NULL, 10);
    Orientation orientation = (Orientation)strtoul(argv[3], NULL, 10);
    assert((width == 240 && height == 240 && orientation == ORIENTATION_0) ||
           (width == 172 && height == 320 && orientation == ORIENTATION_0) ||
           (width == 320 && height == 172 && orientation == ORIENTATION_90));

    lv_init();
    katmed_images_register_lvgl_fs();
    FILE *pack = fopen(argv[4], "rb");
    assert(pack != NULL);
    PackContext pack_context = {.file = pack};
    KatmedStorage storage = {&pack_context, katmed_generated_catalog.pack_bytes, read_pack, NULL};
    assert(katmed_images_mount_storage(storage) == KATMED_IMAGE_OK);

    KatmedImageSelection selection;
    assert(katmed_images_select(0, orientation, &selection) == KATMED_IMAGE_OK);
    assert(selection.width == width && selection.height == height);
    const KatmedImageVariant *variant = katmed_images_variant(
        0, katmed_images_profile_for_orientation(orientation));
    assert(variant != NULL);
    pack_context.corrupt_offset = variant->offset;
    lv_image_decoder_t *decoder = find_tjpgd();
    assert(decoder != NULL && decoder->info_cb != NULL);

    /* Keep the real pack file open so only the decoder work allocation changes. */
    lv_image_decoder_dsc_t dsc = {0};
    dsc.src = selection.lvgl_path;
    dsc.src_type = LV_IMAGE_SRC_FILE;
    assert(lv_fs_open(&dsc.file, selection.lvgl_path, LV_FS_MODE_RD) == LV_FS_RES_OK);
    assert(katmed_images_open_file_count() == 1);
    lv_mem_monitor_t baseline = memory();
    check_info(decoder, &dsc, width, height);
    lv_mem_monitor_t after_success = memory();
    assert(after_success.free_size == baseline.free_size);
    assert(after_success.free_biggest_size == baseline.free_biggest_size);
    assert(katmed_images_open_file_count() == 1);

    /* Leave less than 4096 contiguous bytes in the actual bounded LVGL pool. */
    assert(baseline.free_biggest_size > TJPGD_INFO_WORK_BYTES + 2048u);
    size_t pressure_bytes = baseline.free_biggest_size - 2048u;
    uint8_t *pressure = lv_malloc(pressure_bytes);
    assert(pressure != NULL);
    memset(pressure, 0xa5, pressure_bytes);
    lv_mem_monitor_t constrained = memory();
    assert(constrained.free_biggest_size < TJPGD_INFO_WORK_BYTES);
    assert(lv_fs_seek(&dsc.file, 0, LV_FS_SEEK_SET) == LV_FS_RES_OK);
    lv_image_header_t rejected_header = {0};
    assert(decoder->info_cb(decoder, &dsc, &rejected_header) == LV_RESULT_INVALID);
    lv_mem_monitor_t after_rejection = memory();
    assert(after_rejection.free_size == constrained.free_size);
    assert(after_rejection.free_biggest_size == constrained.free_biggest_size);
    assert(katmed_images_open_file_count() == 1);
    for(size_t i = 0; i < pressure_bytes; ++i) assert(pressure[i] == 0xa5);

    lv_free(pressure);
    lv_mem_monitor_t restored = memory();
    assert(restored.free_size == baseline.free_size);
    assert(restored.free_biggest_size == baseline.free_biggest_size);
    check_info(decoder, &dsc, width, height);
    lv_mem_monitor_t after_retry = memory();
    assert(after_retry.free_size == baseline.free_size);
    assert(after_retry.free_biggest_size == baseline.free_biggest_size);

    /* A parsed-but-damaged JPEG must also release the 4096-byte work buffer. */
    pack_context.corrupt_jpeg = true;
    assert(lv_fs_seek(&dsc.file, 0, LV_FS_SEEK_SET) == LV_FS_RES_OK);
    lv_image_header_t bad_header = {0};
    assert(decoder->info_cb(decoder, &dsc, &bad_header) == LV_RESULT_INVALID);
    lv_mem_monitor_t after_bad_jpeg = memory();
    assert(after_bad_jpeg.free_size == baseline.free_size);
    assert(after_bad_jpeg.free_biggest_size == baseline.free_biggest_size);
    assert(katmed_images_open_file_count() == 1);
    pack_context.corrupt_jpeg = false;
    check_info(decoder, &dsc, width, height);
    lv_mem_monitor_t after_bad_retry = memory();
    assert(after_bad_retry.free_size == baseline.free_size);
    assert(after_bad_retry.free_biggest_size == baseline.free_biggest_size);
    assert(lv_fs_close(&dsc.file) == LV_FS_RES_OK);
    assert(katmed_images_open_file_count() == 0);
    katmed_images_unmount();
    assert(fclose(pack) == 0);
    printf("tjpgd_info=%ux%u baseline_biggest=%zu constrained_biggest=%zu retry=PASS corrupt_retry=PASS\n",
           width, height, baseline.free_biggest_size, constrained.free_biggest_size);
    lv_deinit();
    return 0;
}
