#include "board_boot_gate.h"

#define BOOT_DEBOUNCE_MS 50u

bool board_boot_gate_poll(BoardBootGate *gate, bool down, uint64_t now_ms)
{
    if (!gate) return false;
    if (!gate->initialized) {
        gate->initialized = true;
        gate->candidate_down = down;
        gate->stable_down = down;
        gate->candidate_since_ms = now_ms;
        gate->armed = false;
        gate->press_latched = false;
        return false;
    }
    if (gate->candidate_down != down || now_ms < gate->candidate_since_ms) {
        gate->candidate_down = down;
        gate->candidate_since_ms = now_ms;
    }
    if (now_ms - gate->candidate_since_ms < BOOT_DEBOUNCE_MS) return false;
    if (gate->stable_down == down) {
        if (!down && !gate->press_latched) gate->armed = true;
        return false;
    }
    gate->stable_down = down;
    if (down) {
        if (gate->armed) {
            gate->press_latched = true;
            gate->armed = false;
        }
        return false;
    }
    const bool completed = gate->press_latched;
    gate->press_latched = false;
    gate->armed = true;
    return completed;
}
