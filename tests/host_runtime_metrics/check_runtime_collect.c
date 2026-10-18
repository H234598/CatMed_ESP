#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "runtime_metrics.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "katmed_images.h"
#include "katmed_ui.h"

static bool lock_allowed = true;
static bool task_available = true;
static unsigned heap_calls;
static unsigned stack_calls;
static unsigned locks;
static unsigned unlocks;
static unsigned image_calls;
static unsigned blink_calls;
static size_t heap_min = 1234;
static size_t heap_largest = 567;

size_t heap_caps_get_minimum_free_size(uint32_t caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ++heap_calls;
    return heap_min;
}
size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ++heap_calls;
    return heap_largest;
}
TaskHandle_t xTaskGetHandle(const char *name)
{
    assert(strcmp(name, "taskLVGL") == 0);
    return task_available ? (TaskHandle_t)(uintptr_t)1 : NULL;
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t handle)
{
    ++stack_calls;
    return handle == NULL ? 333 : 222;
}
bool lvgl_port_lock(uint32_t timeout)
{
    assert(timeout == 100);
    ++locks;
    return lock_allowed;
}
void lvgl_port_unlock(void) { ++unlocks; }
KatmedImageTimingSample katmed_images_pack_hash_timing(void)
{
    ++image_calls;
    return (KatmedImageTimingSample){123, KATMED_IMAGE_OK, true};
}
KatmedImageTimingSample katmed_images_variant_hash_timing(void)
{
    ++image_calls;
    return (KatmedImageTimingSample){456, KATMED_IMAGE_BAD_HASH, true};
}
KatmedImageTimingSample katmed_images_jpeg_decode_timing(void)
{
    ++image_calls;
    return (KatmedImageTimingSample){789, KATMED_IMAGE_IO_ERROR, true};
}
UiBlinkRenderMetrics ui_blink_render_metrics(void)
{
    ++blink_calls;
    return (UiBlinkRenderMetrics){
        .valid = true, .completed_cycles = 4, .last_blink_changes = 5,
        .last_flush_areas = 6, .last_flushed_pixels = 700,
        .last_render_us = 800};
}

int main(void)
{
    katmed_runtime_metrics_collect(9, NULL);
    assert(heap_calls == 0 && stack_calls == 0 && locks == 0);

    KatmedRuntimeMetricInput result = {0};
    katmed_runtime_metrics_collect(42, &result);
    assert(result.uptime_ms == 42);
    assert(result.internal_min_free_bytes == 1234 && result.internal_largest_block_bytes == 567);
    assert(result.main_stack_hwm_bytes == 333 && result.lvgl_stack_hwm_bytes == 222);
    assert(result.lvgl_stack_valid && heap_calls == 2 && stack_calls == 2);
    assert(result.pack_hash_us == 123 && result.pack_hash_valid && result.pack_hash_status == KATMED_IMAGE_OK);
    assert(result.variant_hash_us == 456 && result.variant_hash_valid && result.variant_hash_status == KATMED_IMAGE_BAD_HASH);
    assert(result.jpeg_decode_us == 789 && result.jpeg_decode_valid && result.jpeg_decode_status == KATMED_IMAGE_IO_ERROR);
    assert(result.blink_cycle_valid && result.blink_cycle_us == 800 && result.blink_cycle_count == 4);
    assert(result.blink_changes == 5 && result.blink_flush_segments == 6 && result.blink_flush_pixels == 700);
    assert(locks == 1 && unlocks == 1 && image_calls == 3 && blink_calls == 1);

    lock_allowed = false;
    task_available = false;
    heap_min = (size_t)UINT32_MAX + 11;
    heap_largest = (size_t)UINT32_MAX + 22;
    memset(&result, 0xa5, sizeof(result));
    katmed_runtime_metrics_collect(UINT64_MAX, &result);
    assert(result.uptime_ms == UINT64_MAX);
    assert(result.internal_min_free_bytes == UINT32_MAX);
    assert(result.internal_largest_block_bytes == UINT32_MAX);
    assert(result.main_stack_hwm_bytes == 333 && !result.lvgl_stack_valid);
    assert(result.lvgl_stack_hwm_bytes == 0);
    assert(!result.pack_hash_valid && !result.variant_hash_valid && !result.jpeg_decode_valid);
    assert(!result.blink_cycle_valid && result.pack_hash_us == 0 && result.blink_cycle_us == 0);
    assert(heap_calls == 4 && stack_calls == 3 && locks == 2 && unlocks == 1);
    assert(image_calls == 3 && blink_calls == 1);

    puts("runtime SDK collection and unavailable-sample boundaries passed");
    return 0;
}
