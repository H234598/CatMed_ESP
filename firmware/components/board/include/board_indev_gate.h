#ifndef KATMED_BOARD_INDEV_GATE_H
#define KATMED_BOARD_INDEV_GATE_H

#include <stdbool.h>

typedef struct {
    bool quarantined;
} BoardIndevGate;

/* Call on every failed hardware sample, even if a newer sample may overwrite it. */
void board_indev_gate_invalidate(BoardIndevGate *gate);

/* A clean release is required before the next press can reach LVGL. */
bool board_indev_gate_poll(BoardIndevGate *gate, bool valid, bool down,
                           bool stale, bool *reset);

#endif
