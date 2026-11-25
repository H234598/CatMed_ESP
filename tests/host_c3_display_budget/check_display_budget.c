#include "board_display_budget.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    const size_t fast_bytes = 240u * KATMED_C3_FAST_DRAW_LINES * 2u;
    const size_t threshold = fast_bytes + KATMED_C3_DMA_RESERVE_BYTES;
    BoardC3DisplayPlan low = board_c3_display_plan(240, threshold - 1u);
    BoardC3DisplayPlan enough = board_c3_display_plan(240, threshold);
#if KATMED_FULL_STACK
    assert(low.lines == 8u && low.double_buffer);
    assert(enough.lines == 8u && enough.double_buffer);
#else
    assert(low.lines == 16u && low.double_buffer);
    assert(enough.lines == 80u && !enough.double_buffer);
    assert((size_t)240u * low.lines * 2u * 2u == 15360u);
    assert((size_t)240u * enough.lines * 2u == fast_bytes);
#endif
    assert(KATMED_C3_SPI_TRANSFER_MAX_BYTES(240u) == 38408u);
    BoardC3DisplayPlan observed = board_c3_display_plan(240, 114688u);
#if KATMED_FULL_STACK
    assert(observed.lines == 8u && observed.double_buffer);
    const size_t budgets[] = {0u, 7679u, 15360u, threshold - 1u, threshold, 114688u, SIZE_MAX};
    for(size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); ++i) {
        BoardC3DisplayPlan plan = board_c3_display_plan(240, budgets[i]);
        assert(plan.lines == 8u && plan.double_buffer);
        const size_t buffer_bytes = 240u * plan.lines * 2u;
        assert(buffer_bytes == 3840u);
        assert(buffer_bytes % 4u == 0u);
        assert(buffer_bytes <= KATMED_C3_SPI_TRANSFER_MAX_BYTES(240u));
        assert(15360u - buffer_bytes * 2u == 7680u);
        assert(240u / plan.lines == 30u && 240u % plan.lines == 0u);
    }
    assert(fast_bytes - (size_t)240u * observed.lines * 2u * 2u == 30720u);
#else
    assert(observed.lines == 80u && !observed.double_buffer);
#endif
    assert(57600u - fast_bytes == 19200u);
    printf("display_budget=PASS full_stack=%d selected_lines=%u buffers=%u payload=%zu transfer=%zu\n",
           KATMED_FULL_STACK, observed.lines, observed.double_buffer ? 2u : 1u,
           (size_t)240u * observed.lines * 2u * (observed.double_buffer ? 2u : 1u),
           KATMED_C3_SPI_TRANSFER_MAX_BYTES(240u));
    return 0;
}
