#ifndef KATMED_BOARD_H
#define KATMED_BOARD_H
#include "katmed_types.h"
#include "lvgl.h"
/* Filter runs under the LVGL lock; coordinates are already logical. */
typedef bool (*BoardTouchFilter)(bool down, int16_t x, int16_t y,
                                 bool valid, uint64_t monotonic_ms);


#ifdef __cplusplus
extern "C" {
#endif
void board_set_touch_filter(BoardTouchFilter filter);

typedef struct {
    bool down;
    int16_t x, y; /* Logical coordinates after orientation transform. */
    uint64_t monotonic_ms;
} BoardTouch;
typedef enum { BOARD_OK, BOARD_UNPROVEN_WIRING, BOARD_IO_ERROR, BOARD_INVALID } BoardResult;
/* board_init owns the LVGL port and its partial RGB565 draw buffer(s). It
 * returns a display only after panel, input and backlight can be controlled.
 * On failure it leaves no live partially initialized device. */
BoardResult board_init(lv_display_t **display);
BoardResult board_set_backlight(bool enabled);
void board_deinit(void);
/* Implement only against verified board/controller pin evidence. */
BoardResult board_set_orientation(Orientation orientation);
BoardResult board_read_touch(BoardTouch *touch);
/* Polls the I2C controller independently of LVGL's potentially long render
 * lock. Events retain their acquisition timestamps and are consumed in order. */
BoardResult board_touch_sampler_start(void);
void board_touch_sampler_stop(void);
bool board_touch_sampler_running(void);
bool board_touch_sampler_take(BoardTouch *touch, BoardResult *result);
/* BOOT is an input only after ROM strap sampling. A completed, debounced
 * press/release yields one restart request; a held key never requests one. */
BoardResult board_boot_button_init(void);
bool board_boot_button_released(uint64_t monotonic_ms);

#ifdef __cplusplus
}
#endif
#endif
