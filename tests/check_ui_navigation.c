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
    const UiView middle_views[] = {CLOCK, BACKGROUND, SETTINGS};
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned column = 0; column < 3; ++column) {
            for (int direction = -1; direction <= 1; direction += 2) {
                UiGrid grid = {(uint8_t)row, (uint8_t)column};
                for (unsigned step = 1; step <= 3; ++step) {
                    ui_grid_move(&grid, direction, 0);
                    unsigned expected = (column + step * (direction > 0 ? 1u : 2u)) % 3u;
                    assert(grid.row == row && grid.column == expected);
                    assert(ui_grid_view(grid) == (row == 1 ? middle_views[expected] :
                           row == 0 ? UI_VIEW_PLACEHOLDER : UI_VIEW_STATUS));
                    UiGrid middle = grid;
                    if (row != 1) ui_grid_move(&middle, 0, row == 0 ? 1 : -1);
                    assert(middle.row == 1 && middle.column == expected);
                    assert(ui_grid_view(middle) == middle_views[expected]);
                }
                assert(grid.row == row && grid.column == column);
            }
        }
    }
    return 0;
}
