#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "board_geometry.h"
#include "c3_pins.h"

/* The pinned esp_lvgl_port 2.9.0 callback's four hardware rotations. */
static BoardPanelTransform port_transform(unsigned angle, bool base_mirror_x)
{
    const bool swap = angle == ORIENTATION_90 || angle == ORIENTATION_270;
    return (BoardPanelTransform){
        .swap_xy = swap,
        .mirror_x = (angle == ORIENTATION_180 || angle == ORIENTATION_270) != base_mirror_x,
        .mirror_y = angle == ORIENTATION_90 || angle == ORIENTATION_180,
    };
}

/* The pinned LVGL 9.4 indev_pointer_proc mapping from a zero-degree point. */
static void lvgl_pointer(unsigned angle, int16_t *x, int16_t *y)
{
    if (angle == ORIENTATION_180 || angle == ORIENTATION_270) {
        *x = 239 - *x;
        *y = 239 - *y;
    }
    if (angle == ORIENTATION_90 || angle == ORIENTATION_270) {
        const int16_t old_y = *y;
        *y = *x;
        *x = 239 - old_y;
    }
}

int main(void)
{
    const BoardGeometry c3 = {.width = 240, .height = 240};
    const uint16_t points[][2] = {{0, 0}, {239, 0}, {0, 239}, {239, 239}, {61, 173}};
    const BoardPanelTransform expected_c3[] = {
        {false, true, false}, {true, true, true},
        {false, false, true}, {true, false, false},
    };
    const BoardPanelTransform expected_c6[] = {
        {false, false, false}, {true, false, true},
        {false, true, true}, {true, true, false},
    };
    for (unsigned angle = ORIENTATION_0; angle <= ORIENTATION_270; ++angle) {
        BoardPanelTransform board;
        assert(board_geometry_panel_transform((Orientation)angle,
                KATMED_C3_PANEL_MIRROR_X_AT_ZERO, &board));
        const BoardPanelTransform port = port_transform(angle, KATMED_C3_PANEL_MIRROR_X_AT_ZERO);
        assert(board.swap_xy == port.swap_xy);
        assert(board.mirror_x == port.mirror_x);
        assert(board.mirror_y == port.mirror_y);
        assert(board.swap_xy == expected_c3[angle].swap_xy);
        assert(board.mirror_x == expected_c3[angle].mirror_x);
        assert(board.mirror_y == expected_c3[angle].mirror_y);
        /* A single X calibration is retained after the LVGL port callback. */
        assert(board.mirror_x == (angle < ORIENTATION_180));
        BoardPanelTransform c6;
        assert(board_geometry_panel_transform((Orientation)angle, false, &c6));
        assert(c6.swap_xy == expected_c6[angle].swap_xy);
        assert(c6.mirror_x == expected_c6[angle].mirror_x);
        assert(c6.mirror_y == expected_c6[angle].mirror_y);
        for (unsigned p = 0; p < sizeof(points) / sizeof(points[0]); ++p) {
            int16_t base_x, base_y, logical_x, logical_y;
            assert(board_geometry_touch(c3, ORIENTATION_0, points[p][0], points[p][1],
                                        &base_x, &base_y));
            assert(board_geometry_touch(c3, (Orientation)angle, points[p][0], points[p][1],
                                        &logical_x, &logical_y));
            lvgl_pointer(angle, &base_x, &base_y);
            assert(base_x == logical_x && base_y == logical_y);
            if (angle == ORIENTATION_0) {
                /* Hypothesis: this panel variant's zero-degree scanout is
                   reversed in X while the touch controller uses physical X. */
                const int16_t physical_x = board.mirror_x
                    ? (int16_t)points[p][0] : (int16_t)(239 - points[p][0]);
                assert(physical_x == logical_x);
            }
        }
    }
    BoardPanelTransform invalid;
    assert(!board_geometry_panel_transform((Orientation)4, true, &invalid));
    assert(!board_geometry_panel_transform(ORIENTATION_0, true, NULL));
    puts("C3-Panel/LVGL-Port und rohe/logische Touchrotation stimmen in vier Winkeln");
    return 0;
}
