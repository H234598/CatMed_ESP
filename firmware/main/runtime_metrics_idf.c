#include "runtime_metrics.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "katmed_images.h"
#include "katmed_ui.h"
#if CONFIG_IDF_TARGET_ESP32C6
#include "katmed_board.h"
#endif
#include <limits.h>
#include <stddef.h>

static uint32_t bounded_u32(size_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

void katmed_runtime_metrics_collect(uint64_t now_ms, KatmedRuntimeMetricInput *out)
{
    if (out == NULL) return;
    *out = (KatmedRuntimeMetricInput){.uptime_ms = now_ms};

    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    out->internal_min_free_bytes = bounded_u32(heap_caps_get_minimum_free_size(caps));
    out->internal_largest_block_bytes = bounded_u32(heap_caps_get_largest_free_block(caps));

    /* This function is called by app_main. In the selected ESP-IDF 5.5.5
     * non-SMP kernel the high-water mark is in bytes. The standard FreeRTOS
     * unit differs, so retain the explicit unit in the diagnostic name. */
    out->main_stack_hwm_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    TaskHandle_t lvgl_task = xTaskGetHandle("taskLVGL");
    if (lvgl_task != NULL) {
        out->lvgl_stack_hwm_bytes = (uint32_t)uxTaskGetStackHighWaterMark(lvgl_task);
        out->lvgl_stack_valid = true;
    }

#if CONFIG_IDF_TARGET_ESP32C6
    const BoardTouchDiagnostics touch = board_touch_diagnostics();
    out->native_diagnostic_valid = true;
    out->touch_init = touch.init;
    out->touch_probe = touch.probe;
    out->touch_sequence = touch.sequence;
    out->touch_transfer = touch.transfer;
    out->touch_sample = touch.sample;
#endif

    /* The UI and decoder update these values on the serialized LVGL path.
     * A missed lock marks samples unavailable without blocking the loop. */
    if (lvgl_port_lock(100)) {
        const KatmedImageTimingSample pack = katmed_images_pack_hash_timing();
        const KatmedImageTimingSample variant = katmed_images_variant_hash_timing();
        const KatmedImageTimingSample jpeg = katmed_images_jpeg_decode_timing();
        const UiBlinkRenderMetrics blink = ui_blink_render_metrics();
        out->pack_hash_us = pack.elapsed_us;
        out->pack_hash_valid = pack.measured;
        out->pack_hash_status = pack.status;
        out->variant_hash_us = variant.elapsed_us;
        out->variant_hash_valid = variant.measured;
        out->variant_hash_status = variant.status;
        out->variant_sequence = variant.sequence;
        out->variant_io_stage = variant.io_stage;
        out->variant_io_status = variant.io_status;
        out->jpeg_decode_us = jpeg.elapsed_us;
        /* A retained JPEG success cannot attest a later failed validation. */
        out->jpeg_decode_valid = jpeg.measured && variant.measured && variant.status==KATMED_IMAGE_OK;
        out->jpeg_decode_status = jpeg.status;
        out->jpeg_detail_valid = jpeg.measured;
        out->jpeg_sequence = jpeg.sequence;
        out->jpeg_stage = jpeg.decode_stage;
        out->jpeg_mcus = jpeg.decoded_areas;
#ifdef ESP_PLATFORM
        out->image_failed_presentations = katmed_images_failed_presentations();
        out->image_failure_valid = true;
#endif
        out->blink_cycle_us = blink.last_render_us;
        out->blink_cycle_valid = blink.valid;
        out->blink_flush_pixels = blink.last_flushed_pixels;
        out->blink_flush_segments = blink.last_flush_areas;
        out->blink_cycle_count = blink.completed_cycles;
        out->blink_changes = blink.last_blink_changes;
        lvgl_port_unlock();
    }
}
