#include <assert.h>
#include "katmed_ui.h"

int main(void)
{
    assert(ui_next_view(BACKGROUND, SWIPE_LEFT, false) == SETTINGS);
    assert(ui_next_view(BACKGROUND, SWIPE_RIGHT, false) == CLOCK);
    assert(ui_next_view(CLOCK, SWIPE_LEFT, false) == BACKGROUND);
    assert(ui_next_view(CLOCK, SWIPE_RIGHT, false) == CLOCK);
    assert(ui_next_view(SETTINGS, SWIPE_LEFT, false) == SETTINGS);
    assert(ui_next_view(SETTINGS, SWIPE_RIGHT, false) == BACKGROUND);
    assert(ui_next_view(BACKGROUND, NONE, false) == BACKGROUND);
    assert(ui_next_view(CLOCK, NONE, false) == CLOCK);
    assert(ui_next_view(SETTINGS, NONE, false) == SETTINGS);
    for (int view = BACKGROUND; view <= ALARM; ++view) {
        assert(ui_next_view((UiView)view, SWIPE_LEFT, true) == ALARM);
        assert(ui_next_view((UiView)view, SWIPE_RIGHT, true) == ALARM);
        assert(ui_next_view((UiView)view, NONE, true) == ALARM);
    }
    assert(ui_next_view(ALARM, NONE, false) == BACKGROUND);
    assert(ui_next_view(ALARM, SWIPE_LEFT, false) == BACKGROUND);
    assert(ui_next_view(ALARM, SWIPE_RIGHT, false) == BACKGROUND);
    for (unsigned column = 0; column < 3; ++column) {
        UiGrid grid = {1, (uint8_t)column};
        UiView middle = ui_grid_view(grid);
        ui_grid_move(&grid, 0, -1);
        assert(grid.column == column && ui_grid_view(grid) == UI_VIEW_PLACEHOLDER);
        ui_grid_move(&grid, 0, -1);
        assert(grid.column == column && ui_grid_view(grid) == UI_VIEW_STATUS);
        ui_grid_move(&grid, 0, -1);
        assert(ui_grid_view(grid) == middle);
        for (unsigned n = 0; n < 3; ++n) ui_grid_move(&grid, 0, 1);
        assert(grid.row == 1 && grid.column == column);
    }
    UiGrid edge = {0, 0};
    ui_grid_move(&edge, -1, 0);
    assert(edge.column == 0);
    ui_grid_move(&edge, 1, 0);
    ui_grid_move(&edge, 1, 0);
    ui_grid_move(&edge, 1, 0);
    assert(edge.row == 0 && edge.column == 2);
    return 0;
}
