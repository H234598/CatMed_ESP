#ifndef KATMED_BOARD_GEOMETRY_H
#define KATMED_BOARD_GEOMETRY_H

#include <stdbool.h>
#include <stdint.h>
#include "katmed_types.h"

/* Raw touch coordinates use the controller's unrotated coordinate system. */
typedef struct {
    uint16_t width;
    uint16_t height;
    bool mirror_x_at_zero;
} BoardGeometry;

typedef struct {
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
} BoardPanelTransform;

bool board_geometry_panel_transform(Orientation orientation, bool mirror_x_at_zero,
                                    BoardPanelTransform *transform);

bool board_geometry_size(BoardGeometry geometry, Orientation orientation,
                         uint16_t *width, uint16_t *height);
bool board_geometry_touch(BoardGeometry geometry, Orientation orientation,
                          uint16_t raw_x, uint16_t raw_y,
                          int16_t *logical_x, int16_t *logical_y);
/* A failed read cannot become a release event accepted by the alarm gesture. */
bool board_geometry_sample(BoardGeometry geometry, Orientation orientation,
                           bool read_ok, bool raw_down, uint16_t raw_x, uint16_t raw_y,
                           bool *down, int16_t *logical_x, int16_t *logical_y);

#endif
