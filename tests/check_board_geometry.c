#include <assert.h>
#include <stdio.h>
#include "board_geometry.h"
#include "board_protocol.h"

typedef struct { uint16_t raw_x, raw_y; int16_t x[4], y[4]; } Expected;

static void check_geometry(BoardGeometry geometry, const Expected *points, size_t count)
{
    for (int orientation = ORIENTATION_0; orientation <= ORIENTATION_270; ++orientation) {
        uint16_t width = 0, height = 0;
        assert(board_geometry_size(geometry, (Orientation)orientation, &width, &height));
        assert(width == ((orientation & 1) ? geometry.height : geometry.width));
        assert(height == ((orientation & 1) ? geometry.width : geometry.height));
        for (size_t i = 0; i < count; ++i) {
            int16_t x = -1, y = -1;
            assert(board_geometry_touch(geometry, (Orientation)orientation,
                                        points[i].raw_x, points[i].raw_y, &x, &y));
            assert(x == points[i].x[orientation]);
            assert(y == points[i].y[orientation]);
        }
    }
    int16_t x, y;
    assert(!board_geometry_touch(geometry, ORIENTATION_0, geometry.width, 0, &x, &y));
    assert(!board_geometry_touch(geometry, ORIENTATION_0, 0, geometry.height, &x, &y));
    assert(!board_geometry_touch(geometry, (Orientation)4, 0, 0, &x, &y));
    assert(!board_geometry_size(geometry, (Orientation)4, &geometry.width, &geometry.height));
    bool down;
    x = y = 100;
    assert(!board_geometry_sample(geometry, ORIENTATION_0, false, true, 0, 0, &down, &x, &y));
    assert(!down && x == 0 && y == 0);
    assert(board_geometry_sample(geometry, ORIENTATION_0, true, false, 0, 0, &down, &x, &y));
    assert(!down && x == 0 && y == 0);
    assert(!board_geometry_sample(geometry, ORIENTATION_0, true, true,
                                  geometry.width, 0, &down, &x, &y));
    assert(!down && x == 0 && y == 0);
}

static void check_protocol(void)
{
    bool down = true;
    uint16_t x = 99, y = 99;
    assert(board_c3_touch_decode(0, NULL, &down, &x, &y));
    assert(!down && x == 0 && y == 0);
    const uint8_t c3_point[4] = {0x01, 0x2c, 0x00, 0xef};
    assert(board_c3_touch_decode(1, c3_point, &down, &x, &y));
    assert(down && x == 300 && y == 239);
    assert(!board_c3_touch_decode(2, c3_point, &down, &x, &y));
    assert(!board_c3_touch_decode(1, NULL, &down, &x, &y));
    assert(!board_geometry_touch((BoardGeometry){240, 240, false}, ORIENTATION_0, x, y,
                                 &(int16_t){0}, &(int16_t){0}));

    uint8_t c6_packet[14] = {0};
    assert(board_c6_touch_decode(c6_packet, &down, &x, &y));
    assert(!down && x == 0 && y == 0);
    c6_packet[1] = 1;
    c6_packet[2] = 0; c6_packet[3] = 171;
    c6_packet[4] = 1; c6_packet[5] = 63;
    assert(board_c6_touch_decode(c6_packet, &down, &x, &y));
    assert(down && x == 171 && y == 319);
    c6_packet[1] = 2;
    assert(!board_c6_touch_decode(c6_packet, &down, &x, &y));
    assert(!board_c6_touch_decode(NULL, &down, &x, &y));
}

int main(void)
{
    static const Expected c3[] = {
        {0,0,       {0,239,239,0},       {0,0,239,239}},
        {239,0,     {239,239,0,0},       {0,239,239,0}},
        {0,239,     {0,0,239,239},       {239,0,0,239}},
        {239,239,   {239,0,0,239},       {239,239,0,0}},
        {120,120,   {120,119,119,120},   {120,120,119,119}},
    };
    static const Expected c6[] = {
        {0,0,       {171,319,0,0},       {0,171,319,0}},
        {171,0,     {0,319,171,0},       {0,0,319,171}},
        {0,319,     {171,0,0,319},       {319,171,0,0}},
        {171,319,   {0,0,171,319},       {319,0,0,171}},
        {86,160,    {85,159,86,160},    {160,85,159,86}},
    };
    check_geometry((BoardGeometry){240, 240, false}, c3, sizeof(c3) / sizeof(c3[0]));
    check_geometry((BoardGeometry){172, 320, true}, c6, sizeof(c6) / sizeof(c6[0]));
    check_protocol();
    puts("Board-Geometrie und Touchprotokoll: alle Sollwerte bestanden");
    return 0;
}
