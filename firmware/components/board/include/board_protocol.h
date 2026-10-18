#ifndef KATMED_BOARD_PROTOCOL_H
#define KATMED_BOARD_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

/* False means an invalid sample; a zero contact count is a valid release. */
bool board_c3_touch_decode(uint8_t count, const uint8_t point[4],
                           bool *down, uint16_t *x, uint16_t *y);
bool board_c6_touch_decode(const uint8_t packet[14],
                           bool *down, uint16_t *x, uint16_t *y);

#endif
