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
    #if KATMED_FULL_STACK
    assert(enough.lines == 16u && enough.double_buffer);
#else
    assert(enough.lines == 80u && !enough.double_buffer);
#endif
    assert((size_t)240u * low.lines * 2u * 2u == 15360u);
    #if !KATMED_FULL_STACK
    assert((size_t)240u * enough.lines * 2u == fast_bytes);
#endif
    assert(KATMED_C3_SPI_TRANSFER_MAX_BYTES(240u) == 38408u);
    BoardC3DisplayPlan observed = board_c3_display_plan(240, 114688u);
    #if KATMED_FULL_STACK
    assert(observed.lines == 16u && observed.double_buffer);
    const size_t budgets[] = {0u, 7679u, 15360u, threshold - 1u, threshold, 114688u, SIZE_MAX};
    for(size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); ++i) {
        BoardC3DisplayPlan plan = board_c3_display_plan(240, budgets[i]);
        assert(plan.lines == 16u && plan.double_buffer);
        assert(240u * plan.lines * 2u % 4u == 0u);
        assert(240u * plan.lines * 2u * 2u == 15360u);
        assert(240u * plan.lines * 2u <= KATMED_C3_SPI_TRANSFER_MAX_BYTES(240u));
    }
    assert(fast_bytes - 15360u == 23040u);
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
