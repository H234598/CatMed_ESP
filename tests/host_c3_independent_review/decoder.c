#include <assert.h>
#include <stdio.h>
#include "board_protocol.h"
#include "board_geometry.h"
#include "board_indev_gate.h"

int main(void)
{
    bool down = false, reset = false;
    uint16_t x = 0, y = 0;
    int16_t lx = 0, ly = 0;
    const BoardGeometry c3 = {240, 240, true};
    const BoardGeometry c6 = {172, 320, false};
    const uint8_t c3_point[4] = {0, 100, 0, 110};
    assert(board_c3_touch_decode(1, c3_point, &down, &x, &y));
    assert(down && x == 100 && y == 110);
    assert(board_geometry_sample(c3, ORIENTATION_0, true, down, x, y,
                                 &down, &lx, &ly));
    assert(down && lx == 139 && ly == 110);
    assert(board_c3_touch_decode(0, NULL, &down, &x, &y) && !down);
    assert(!board_c3_touch_decode(2, c3_point, &down, &x, &y));
    assert(!board_geometry_sample(c3, ORIENTATION_0, false, true, 100, 110,
                                  &down, &lx, &ly));
    assert(!down);

    uint8_t c6_packet[14] = {0};
    c6_packet[1] = 1;
    c6_packet[3] = 80;
    c6_packet[5] = 120;
    assert(board_c6_touch_decode(c6_packet, &down, &x, &y));
    assert(down && x == 80 && y == 120);
    assert(board_geometry_sample(c6, ORIENTATION_90, true, down, x, y,
                                 &down, &lx, &ly));
    assert(down && lx == 199 && ly == 80);
    assert(!board_geometry_sample(c6, ORIENTATION_0, true, true, 172, 0,
                                  &down, &lx, &ly));
    assert(!down);
    c6_packet[1] = 2;
    assert(!board_c6_touch_decode(c6_packet, &down, &x, &y));

    BoardIndevGate gate = {.quarantined = true};
    assert(!board_indev_gate_poll(&gate, true, true, false, &reset) && reset);
    assert(!board_indev_gate_poll(&gate, true, false, false, &reset) && reset);
    assert(board_indev_gate_poll(&gate, true, true, false, &reset) && !reset);
    assert(!board_indev_gate_poll(&gate, true, true, true, &reset) && reset);
    assert(!board_indev_gate_poll(&gate, true, false, false, &reset) && reset);
    assert(board_indev_gate_poll(&gate, true, true, false, &reset) && !reset);
    puts("independent C3/C6 decoder, geometry and stale-contact gate: OK");
    return 0;
}
