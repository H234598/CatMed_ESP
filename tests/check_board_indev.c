#include <assert.h>
#include <stdio.h>
#include "board_indev_gate.h"
#include "board_geometry.h"

static void check_error_quarantine(void)
{
    BoardIndevGate gate = {.quarantined = true};
    bool reset = false;
    assert(!board_indev_gate_poll(&gate, true, true, false, &reset) && reset);
    assert(!board_indev_gate_poll(&gate, true, false, false, &reset) && reset);
    assert(!gate.quarantined);
    assert(board_indev_gate_poll(&gate, true, true, false, &reset) && !reset);

    /* The error occurs after LVGL saw PRESSED. It remains latched even if a
       later clean sample overwrites the cache before LVGL polls again. */
    board_indev_gate_invalidate(&gate);
    assert(!board_indev_gate_poll(&gate, true, false, false, &reset) && reset);
    assert(!gate.quarantined);
    assert(board_indev_gate_poll(&gate, true, true, false, &reset) && !reset);
    assert(!board_indev_gate_poll(&gate, false, false, false, &reset) && reset);
    assert(!board_indev_gate_poll(&gate, true, true, false, &reset) && reset);
    assert(gate.quarantined);
    assert(!board_indev_gate_poll(&gate, true, false, false, &reset) && reset);
    assert(board_indev_gate_poll(&gate, true, true, false, &reset) && !reset);

    /* A missing poll is also treated as an untrusted release. */
    assert(!board_indev_gate_poll(&gate, true, true, true, &reset) && reset);
    assert(!board_indev_gate_poll(&gate, true, true, false, &reset) && reset);
    assert(!board_indev_gate_poll(&gate, true, false, false, &reset) && reset);
}

static void check_base_coordinates(void)
{
    const BoardGeometry boards[] = {{240, 240, false}, {172, 320, true}};
    for (unsigned b = 0; b < 2; ++b) {
        BoardGeometry g = boards[b];
        for (unsigned angle = 0; angle < 4; ++angle) {
            for (unsigned point = 0; point < 5; ++point) {
                uint16_t rx = point < 4 ? (point & 1 ? g.width - 1 : 0) : g.width / 2;
                uint16_t ry = point < 4 ? (point & 2 ? g.height - 1 : 0) : g.height / 2;
                int16_t base_x, base_y, logical_x, logical_y;
                assert(board_geometry_touch(g, ORIENTATION_0, rx, ry, &base_x, &base_y));
                assert(board_geometry_touch(g, (Orientation)angle, rx, ry, &logical_x, &logical_y));
                /* LVGL 9.4's indev_pointer_proc applies this rotation to the
                   unrotated panel point; the BoardTouch API is already logical. */
                int16_t lx = base_x, ly = base_y;
                if (angle == ORIENTATION_180 || angle == ORIENTATION_270) {
                    lx = g.width - lx - 1;
                    ly = g.height - ly - 1;
                }
                if (angle == ORIENTATION_90 || angle == ORIENTATION_270) {
                    int16_t old_y = ly;
                    ly = lx;
                    lx = g.height - old_y - 1;
                }
                assert(lx == logical_x && ly == logical_y);
            }
        }
    }
}

int main(void)
{
    check_error_quarantine();
    check_base_coordinates();
    puts("Board-Indev: Fehlerabbruch, Wiederarmierung und 40 Rotationspunkte bestanden");
    return 0;
}
