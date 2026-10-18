#ifndef KATMED_BOARD_TOUCH_BUFFER_H
#define KATMED_BOARD_TOUCH_BUFFER_H

#include "katmed_board.h"

#define BOARD_TOUCH_BUFFER_CAPACITY 128u

typedef struct {
    BoardTouch sample[BOARD_TOUCH_BUFFER_CAPACITY];
    BoardResult result[BOARD_TOUCH_BUFFER_CAPACITY];
    unsigned head, count;
    bool overflow;
} BoardTouchBuffer;

void board_touch_buffer_push(BoardTouchBuffer *buffer, BoardTouch sample, BoardResult result);
bool board_touch_buffer_take(BoardTouchBuffer *buffer, BoardTouch *sample, BoardResult *result);

#endif
