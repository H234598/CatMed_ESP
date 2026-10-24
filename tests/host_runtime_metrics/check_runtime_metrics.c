#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "runtime_metrics.h"

static unsigned log_count;
static char native_line[512];
static char log_tag[32];
static char log_line[1024];

void test_log_info(const char *tag, const char *format, ...)
{
    ++log_count;
    assert(strlen(tag) < sizeof(log_tag));
    strcpy(log_tag, tag);
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(log_line, sizeof(log_line), format, args);
    va_end(args);
    assert(length > 0 && (size_t)length < sizeof(log_line));
    if(strstr(log_line,"jpeg_detail_v=")) {assert(length<512);strcpy(native_line,log_line);}
}

static void check_gate(void)
{
    KatmedRuntimeMetricsGate gate = {0};
    assert(!katmed_runtime_metrics_due(NULL, 1));
    assert(katmed_runtime_metrics_due(&gate, 0));
    assert(!katmed_runtime_metrics_due(&gate, 0));
    assert(!katmed_runtime_metrics_due(&gate, 9999));
    assert(katmed_runtime_metrics_due(&gate, 10000));
    assert(!katmed_runtime_metrics_due(&gate, 19999));
    assert(katmed_runtime_metrics_due(&gate, 20000));
    assert(katmed_runtime_metrics_due(&gate, 5)); /* Backward sample resets interval. */
    assert(!katmed_runtime_metrics_due(&gate, 10004));
    assert(katmed_runtime_metrics_due(&gate, 10005));

    /* Subtraction must stay valid beyond the old 32-bit uptime boundary. */
    gate = (KatmedRuntimeMetricsGate){0};
    const uint64_t late = UINT64_C(4294967295) + 20000;
    assert(katmed_runtime_metrics_due(&gate, late));
    assert(!katmed_runtime_metrics_due(&gate, late + 9999));
    assert(katmed_runtime_metrics_due(&gate, late + 10000));
    gate = (KatmedRuntimeMetricsGate){0};
    assert(katmed_runtime_metrics_due(&gate, UINT64_MAX - 10000));
    assert(katmed_runtime_metrics_due(&gate, UINT64_MAX));
    assert(!katmed_runtime_metrics_due(&gate, UINT64_MAX));
}

static void check_log(void)
{
    const KatmedRuntimeMetricInput input = {
        .uptime_ms = UINT64_MAX,
        .pack_hash_us = UINT64_C(1234567890123),
        .variant_hash_us = 23,
        .jpeg_decode_us = 34,
        .blink_cycle_us = 45,
        .blink_flush_pixels = 55,
        .blink_flush_segments = 2,
        .blink_cycle_count = 3,
        .blink_changes = 4,
        .internal_min_free_bytes = 66,
        .internal_largest_block_bytes = 77,
        .main_stack_hwm_bytes = 88,
        .lvgl_stack_hwm_bytes = 99,
        .pack_hash_valid = true,
        .variant_hash_valid = false,
        .jpeg_decode_valid = true,
        .blink_cycle_valid = true,
        .lvgl_stack_valid = false,
        .pack_hash_status = -1,
        .variant_hash_status = -2,
        .jpeg_decode_status = -3,
        .variant_sequence = 7,
        .variant_io_stage = 9,
        .variant_io_status = 7,
    };
    katmed_runtime_metrics_log(NULL);
    assert(log_count == 0);
    katmed_runtime_metrics_log(&input);
    assert(log_count == 1 && strcmp(log_tag, "katmed_metrics") == 0);
    assert(strstr(log_line, "uptime_ms=18446744073709551615"));
    assert(strstr(log_line, "pack_hash_us=1234567890123 pack_valid=1"));
    assert(strstr(log_line, "variant_hash_us=23 variant_valid=0"));
    assert(strstr(log_line, "jpeg_decode_us=34 jpeg_valid=1"));
    assert(strstr(log_line, "blink_cycle_us=45 blink_valid=1"));
    assert(strstr(log_line, "blink_pixels=55 blink_segments=2"));
    assert(strstr(log_line, "blink_cycles=3 blink_changes=4"));
    assert(strstr(log_line, "pack_status=-1 variant_status=-2 jpeg_status=-3"));
    assert(strstr(log_line, "internal_min_free=66 internal_largest_block=77"));
    assert(strstr(log_line, "main_stack_hwm_bytes=88 lvgl_stack_hwm_bytes=99 lvgl_stack_valid=0"));
    assert(strstr(log_line,"variant_seq=7 variant_io_stage=9 variant_io_status=7"));
    assert(strlen(log_line) < 768);
    /* The diagnostics contain fixed numeric fields, without the USB status protocol. */
    assert(strstr(log_line, "KM1") == NULL);
    assert(strstr(log_line, "ssid") == NULL);

    const KatmedRuntimeMetricInput worst = {
        .uptime_ms = UINT64_MAX,
        .pack_hash_us = UINT64_MAX,
        .variant_hash_us = UINT64_MAX,
        .jpeg_decode_us = UINT64_MAX,
        .blink_cycle_us = UINT64_MAX,
        .blink_flush_pixels = UINT32_MAX,
        .blink_flush_segments = UINT32_MAX,
        .blink_cycle_count = UINT32_MAX,
        .blink_changes = UINT32_MAX,
        .internal_min_free_bytes = UINT32_MAX,
        .internal_largest_block_bytes = UINT32_MAX,
        .main_stack_hwm_bytes = UINT32_MAX,
        .lvgl_stack_hwm_bytes = UINT32_MAX,
        .pack_hash_valid = true,
        .variant_hash_valid = true,
        .jpeg_decode_valid = true,
        .blink_cycle_valid = true,
        .lvgl_stack_valid = true,
        .pack_hash_status = INT32_MIN,
        .variant_hash_status = INT32_MIN,
        .jpeg_decode_status = INT32_MIN,
        .variant_sequence = UINT32_MAX,
        .variant_io_stage = 12,
        .variant_io_status = 10,
    };
    katmed_runtime_metrics_log(&worst);
    assert(log_count == 2 && strlen(log_line) < 768);
    KatmedRuntimeMetricInput diagnostic={.uptime_ms=UINT64_MAX,.native_diagnostic_valid=true,
      .jpeg_detail_valid=true,.jpeg_sequence=UINT32_MAX,.jpeg_stage=2,.jpeg_decode_status=5,.jpeg_mcus=1280,
      .touch_init=4,.touch_probe=1,.touch_sequence=UINT32_MAX,.touch_transfer=2,.touch_sample=4,
      .jpeg_pool_valid=true,.jpeg_pool_free=UINT32_MAX,.jpeg_pool_largest=UINT32_MAX,.touch_result=4};
    katmed_runtime_metrics_log(&diagnostic);
    assert(log_count==4 && strstr(native_line,"jpeg_detail_v=1 jpeg_seq=4294967295 jpeg_stage=2 jpeg_result=5 jpeg_mcus=1280"));
    assert(strstr(native_line,"touch_init=4 touch_probe=1 touch_seq=4294967295 touch_transfer=2 touch_sample=4"));
    assert(strstr(native_line,"jpeg_pool_v=1 jpeg_pool_free=4294967295 jpeg_pool_largest=4294967295 touch_result=4"));
    assert(strlen(native_line)+32<512);

}

int main(void)
{
    check_gate();
    check_log();
    puts("runtime metrics gate and bounded log passed");
    return 0;
}
