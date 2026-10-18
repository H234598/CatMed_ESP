#include "board_indev_gate.h"

void board_indev_gate_invalidate(BoardIndevGate *gate)
{
    if (gate) gate->quarantined = true;
}

bool board_indev_gate_poll(BoardIndevGate *gate, bool valid, bool down,
                           bool stale, bool *reset)
{
    if (!gate || !reset) return false;
    if (!valid || stale) gate->quarantined = true;
    *reset = gate->quarantined;
    if (gate->quarantined && valid && !down && !stale) gate->quarantined = false;
    return !*reset && valid && down && !stale;
}
