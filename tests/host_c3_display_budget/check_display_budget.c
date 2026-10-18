#include "board_display_budget.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const size_t fast_bytes = 240u * KATMED_C3_FAST_DRAW_LINES * 2u;
    const size_t threshold = fast_bytes + KATMED_C3_DMA_RESERVE_BYTES;
    BoardC3DisplayPlan low = board_c3_display_plan(240, threshold - 1u);
    BoardC3DisplayPlan enough = board_c3_display_plan(240, threshold);
    assert(low.lines == 16u && low.double_buffer);
    assert(enough.lines == 80u && !enough.double_buffer);
    assert((size_t)240u * low.lines * 2u * 2u == 15360u);
    assert((size_t)240u * enough.lines * 2u == fast_bytes);
    assert(KATMED_C3_SPI_TRANSFER_MAX_BYTES(240u) == 38408u);
    BoardC3DisplayPlan observed = board_c3_display_plan(240, 114688u);
    assert(observed.lines == 80u && !observed.double_buffer);
    assert(57600u - fast_bytes == 19200u);
    printf("display_budget=PASS fallback=16x2 fast=80x1 threshold=%zu transfer=%zu dma_saved=%zu\n",
           threshold, KATMED_C3_SPI_TRANSFER_MAX_BYTES(240u), 57600u - fast_bytes);
    return 0;
}
