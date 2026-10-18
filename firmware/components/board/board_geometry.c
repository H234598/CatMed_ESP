#include "board_geometry.h"

bool board_geometry_panel_transform(Orientation orientation, bool mirror_x_at_zero,
                                    BoardPanelTransform *transform)
{
    if (!transform || orientation > ORIENTATION_270) return false;
    *transform = (BoardPanelTransform){
        .swap_xy = orientation == ORIENTATION_90 || orientation == ORIENTATION_270,
        .mirror_x = (orientation == ORIENTATION_180 || orientation == ORIENTATION_270) !=
                    mirror_x_at_zero,
        .mirror_y = orientation == ORIENTATION_90 || orientation == ORIENTATION_180,
    };
    return true;
}

bool board_geometry_size(BoardGeometry g, Orientation orientation,
                         uint16_t *width, uint16_t *height)
{
    if (!width || !height || !g.width || !g.height || orientation > ORIENTATION_270) return false;
    if (orientation == ORIENTATION_90 || orientation == ORIENTATION_270) {
        *width = g.height;
        *height = g.width;
    } else {
        *width = g.width;
        *height = g.height;
    }
    return true;
}

bool board_geometry_touch(BoardGeometry g, Orientation orientation,
                          uint16_t raw_x, uint16_t raw_y,
                          int16_t *logical_x, int16_t *logical_y)
{
    uint16_t width, height;
    if (!logical_x || !logical_y || !board_geometry_size(g, orientation, &width, &height) ||
        raw_x >= g.width || raw_y >= g.height) return false;
    int32_t x = g.mirror_x_at_zero ? g.width - 1 - raw_x : raw_x;
    int32_t y = raw_y;
    int32_t rx = x, ry = y;
    switch (orientation) {
    case ORIENTATION_0: break;
    case ORIENTATION_90: rx = g.height - 1 - y; ry = x; break;
    case ORIENTATION_180: rx = g.width - 1 - x; ry = g.height - 1 - y; break;
    case ORIENTATION_270: rx = y; ry = g.width - 1 - x; break;
    default: return false;
    }
    if (rx < 0 || ry < 0 || rx >= width || ry >= height) return false;
    *logical_x = (int16_t)rx;
    *logical_y = (int16_t)ry;
    return true;
}

bool board_geometry_sample(BoardGeometry g, Orientation orientation,
                           bool read_ok, bool raw_down, uint16_t raw_x, uint16_t raw_y,
                           bool *down, int16_t *logical_x, int16_t *logical_y)
{
    if (!down || !logical_x || !logical_y) return false;
    *down = false;
    *logical_x = *logical_y = 0;
    if (!read_ok) return false;
    if (!raw_down) return true;
    if (!board_geometry_touch(g, orientation, raw_x, raw_y, logical_x, logical_y)) return false;
    *down = true;
    return true;
}
