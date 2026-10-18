#include "katmed_ui.h"

UiView ui_next_view(UiView current, UiGesture gesture, bool alarm_active)
{
    if (alarm_active) return ALARM;
    if (current == ALARM) return BACKGROUND;
    if (gesture == NONE) return current;

    switch (current) {
    case BACKGROUND:
        if (gesture == SWIPE_LEFT) return SETTINGS;
        if (gesture == SWIPE_RIGHT) return CLOCK;
        break;
    case CLOCK:
        if (gesture == SWIPE_LEFT) return BACKGROUND;
        break;
    case SETTINGS:
        if (gesture == SWIPE_RIGHT) return BACKGROUND;
        break;
    case ALARM:
    case MORSE:
        break;
    }
    return current;
}

UiView ui_grid_view(UiGrid grid)
{
    if (grid.row == 0) return UI_VIEW_PLACEHOLDER;
    if (grid.row == 2) return UI_VIEW_STATUS;
    return grid.column == 0 ? CLOCK : grid.column == 2 ? SETTINGS : BACKGROUND;
}

void ui_grid_move(UiGrid *grid, int horizontal, int vertical)
{
    if (!grid) return;
    if (vertical) grid->row = (uint8_t)((grid->row + (vertical > 0 ? 1 : 2)) % 3);
    if (horizontal) grid->column = (uint8_t)((grid->column + (horizontal > 0 ? 1 : 2)) % 3);
}
