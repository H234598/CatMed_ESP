#ifndef KATMED_BOARD_DISPLAY_BUDGET_H
#define KATMED_BOARD_DISPLAY_BUDGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KATMED_C3_FAST_DRAW_LINES 80u
#define KATMED_C3_SAFE_DRAW_LINES 16u
#define KATMED_C3_DMA_RESERVE_BYTES 16384u
#define KATMED_C3_SPI_TRANSFER_MAX_BYTES(width) \
    ((size_t)(width) * KATMED_C3_FAST_DRAW_LINES * 2u + 8u)

typedef struct {
    uint16_t lines;
    bool double_buffer;
} BoardC3DisplayPlan;

static inline BoardC3DisplayPlan board_c3_display_plan(uint16_t width,
                                                        size_t largest_dma_block)
{
#if defined(KATMED_FULL_STACK) && KATMED_FULL_STACK
    /* Reserve runtime headroom for the network/Web stack using the existing
     * partial-refresh dual-buffer path (15360 RGB565 bytes at width240). */
    (void)width;
    (void)largest_dma_block;
    return (BoardC3DisplayPlan){KATMED_C3_SAFE_DRAW_LINES, true};
#else
    const size_t fast_bytes = (size_t)width * KATMED_C3_FAST_DRAW_LINES * 2u;
    if(largest_dma_block >= fast_bytes + KATMED_C3_DMA_RESERVE_BYTES)
        return (BoardC3DisplayPlan){KATMED_C3_FAST_DRAW_LINES, false};
    return (BoardC3DisplayPlan){KATMED_C3_SAFE_DRAW_LINES, true};
#endif
}

#endif
