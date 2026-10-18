#include "board_protocol.h"

bool board_c3_touch_decode(uint8_t count, const uint8_t point[4],
                           bool *down, uint16_t *x, uint16_t *y)
{
    if (!down || !x || !y || count > 1 || (count == 1 && !point)) return false;
    *down = count == 1;
    *x = *y = 0;
    if (*down) {
        *x = ((uint16_t)(point[0] & 0x0f) << 8) | point[1];
        *y = ((uint16_t)(point[2] & 0x0f) << 8) | point[3];
    }
    return true;
}

bool board_c6_touch_decode(const uint8_t packet[14],
                           bool *down, uint16_t *x, uint16_t *y)
{
    if (!packet || !down || !x || !y) return false;
    uint8_t count = packet[1] & 0x0f;
    if (count > 1) return false;
    *down = count == 1;
    *x = *y = 0;
    if (*down) {
        *x = ((uint16_t)(packet[2] & 0x0f) << 8) | packet[3];
        *y = ((uint16_t)(packet[4] & 0x0f) << 8) | packet[5];
    }
    return true;
}
