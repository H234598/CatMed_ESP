#ifndef KATMED_BOARD_BOOT_GATE_H
#define KATMED_BOARD_BOOT_GATE_H

#include <stdbool.h>
#include <stdint.h>

/* A BOOT press is accepted only after a debounced release at startup. A
 * complete press/release gesture fires once, after GPIO9 is high again, so
 * a software restart cannot sample a held BOOT strap as download mode. */
typedef struct {
    uint64_t candidate_since_ms;
    bool initialized;
    bool candidate_down;
    bool stable_down;
    bool armed;
    bool press_latched;
} BoardBootGate;

bool board_boot_gate_poll(BoardBootGate *gate, bool down, uint64_t now_ms);

#endif
