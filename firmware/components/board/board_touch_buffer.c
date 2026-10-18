#include "board_touch_buffer.h"

void board_touch_buffer_push(BoardTouchBuffer *buffer, BoardTouch sample, BoardResult result)
{
    if (!buffer) return;
    if (buffer->overflow) {
        /* Nothing from a contact with lost samples may become a confirmation.
         * A valid release ends quarantine and arms a later fresh contact. */
        if (result != BOARD_OK || sample.down) return;
        buffer->overflow = false;
        buffer->head = 0;
        buffer->count = 0;
        buffer->result[0] = BOARD_IO_ERROR;
        buffer->sample[0] = sample;
        buffer->result[1] = BOARD_OK;
        buffer->sample[1] = sample;
        buffer->count = 2;
        return;
    }
    if (buffer->count == BOARD_TOUCH_BUFFER_CAPACITY) {
        buffer->overflow = true;
        buffer->head = 0;
        buffer->count = 0;
        return;
    }
    unsigned tail = (buffer->head + buffer->count) % BOARD_TOUCH_BUFFER_CAPACITY;
    buffer->sample[tail] = sample;
    buffer->result[tail] = result;
    ++buffer->count;
}

bool board_touch_buffer_take(BoardTouchBuffer *buffer, BoardTouch *sample, BoardResult *result)
{
    if (!buffer || !sample || !result || buffer->count == 0) return false;
    *sample = buffer->sample[buffer->head];
    *result = buffer->result[buffer->head];
    buffer->head = (buffer->head + 1) % BOARD_TOUCH_BUFFER_CAPACITY;
    --buffer->count;
    return true;
}
