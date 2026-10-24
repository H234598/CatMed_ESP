#ifndef KATMED_BOARD_ORIENTATION_H
#define KATMED_BOARD_ORIENTATION_H
#include "katmed_types.h"
/* QMI8658A: ±4g, 8192 counts/g. State belongs to the existing sampler. */
typedef struct {
    Orientation candidate, accepted;
    uint64_t since_ms;
    bool pending, valid;
} BoardOrientationPolicy;
/* Flat, implausible, failed or interrupted samples break the dwell, retaining
 * the last plausible orientation. No allocation, clock or I/O in this policy. */
bool board_orientation_update(BoardOrientationPolicy *state, bool sample_ok,
    int16_t x, int16_t y, int16_t z, uint64_t now_ms);
#endif
