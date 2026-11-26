#ifndef KATMED_RENDER_TRACE_H
#define KATMED_RENDER_TRACE_H

#include <stdbool.h>
#include <stdint.h>

/* Fixed, non-private boundaries inside one owner render call. Durations of
 * nested groups overlap and include time descheduled, not just CPU work. */
typedef enum {
    KATMED_RENDER_INNER_LVGL_LOCK = 0,
    KATMED_RENDER_INNER_UI_RENDER,
    KATMED_RENDER_INNER_LANGUAGE_REBUILD,
    KATMED_RENDER_INNER_IMAGE_PRESENT,
    KATMED_RENDER_INNER_IMAGE_SELECT,
    KATMED_RENDER_INNER_VARIANT_HASH,
    KATMED_RENDER_INNER_JPEG_DECODE,
    KATMED_RENDER_INNER_STORE_WAIT,
    KATMED_RENDER_INNER_VALIDATION_WAIT,
    KATMED_RENDER_INNER_COUNT
} KatmedRenderInnerPhase;

/* Last fixed public KatmedImageStatus value. The Web projection rejects
 * synthetic/out-of-contract values without importing image payload types. */
#define KATMED_RENDER_IMAGE_STATUS_MAX 10u

typedef void (*KatmedRenderTraceObserver)(void *context, KatmedRenderInnerPhase phase,
                                          bool begin, uint8_t outcome);

#endif
