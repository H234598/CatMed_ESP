#include <assert.h>
#include <stdio.h>
#include "board_boot_gate.h"

static void stable_start(BoardBootGate *gate)
{
    assert(!board_boot_gate_poll(gate, false, 0));
    assert(!board_boot_gate_poll(gate, false, 49));
    assert(!board_boot_gate_poll(gate, false, 50));
    assert(gate->armed);
}

static void one_complete_press(BoardBootGate *gate, uint64_t start)
{
    assert(!board_boot_gate_poll(gate, true, start));
    assert(!board_boot_gate_poll(gate, true, start + 49));
    assert(!board_boot_gate_poll(gate, true, start + 50));
    assert(!board_boot_gate_poll(gate, true, start + 5000));
    assert(!board_boot_gate_poll(gate, false, start + 5010));
    assert(!board_boot_gate_poll(gate, false, start + 5059));
    assert(board_boot_gate_poll(gate, false, start + 5060));
    assert(!board_boot_gate_poll(gate, false, start + 9000));
}

static void check_strap_held_at_start(void)
{
    BoardBootGate gate = {0};
    assert(!board_boot_gate_poll(&gate, true, 0));
    assert(!board_boot_gate_poll(&gate, true, 10000));
    assert(!board_boot_gate_poll(&gate, false, 10010));
    assert(!board_boot_gate_poll(&gate, false, 10060));
    assert(gate.armed && !gate.press_latched);
    one_complete_press(&gate, 10100);
}

static void check_bounce_and_short_tap(void)
{
    BoardBootGate gate = {0};
    stable_start(&gate);
    /* Short tap never reaches the stable-down state. */
    assert(!board_boot_gate_poll(&gate, true, 100));
    assert(!board_boot_gate_poll(&gate, false, 125));
    assert(!board_boot_gate_poll(&gate, false, 200));
    assert(!gate.press_latched);

    assert(!board_boot_gate_poll(&gate, true, 250));
    assert(!board_boot_gate_poll(&gate, false, 270));
    assert(!board_boot_gate_poll(&gate, true, 280));
    assert(!board_boot_gate_poll(&gate, true, 330));
    assert(gate.press_latched);
    /* A bouncing release does not complete or rearm the gesture. */
    assert(!board_boot_gate_poll(&gate, false, 350));
    assert(!board_boot_gate_poll(&gate, true, 370));
    assert(!board_boot_gate_poll(&gate, true, 430));
    assert(!board_boot_gate_poll(&gate, false, 450));
    assert(!board_boot_gate_poll(&gate, false, 499));
    assert(board_boot_gate_poll(&gate, false, 500));
    assert(!board_boot_gate_poll(&gate, false, 1000));
    one_complete_press(&gate, 1100);
}

static void check_release_needed_before_first_press(void)
{
    BoardBootGate gate = {0};
    assert(!board_boot_gate_poll(&gate, false, 0));
    assert(!board_boot_gate_poll(&gate, true, 30));
    assert(!board_boot_gate_poll(&gate, true, 80));
    assert(!board_boot_gate_poll(&gate, false, 100));
    assert(!board_boot_gate_poll(&gate, false, 150));
    assert(gate.armed && !gate.press_latched);
    one_complete_press(&gate, 200);
}

static void check_clock_regression(void)
{
    BoardBootGate gate = {0};
    stable_start(&gate);
    assert(!board_boot_gate_poll(&gate, true, 100));
    assert(!board_boot_gate_poll(&gate, true, 90));
    assert(!board_boot_gate_poll(&gate, true, 139));
    assert(!board_boot_gate_poll(&gate, true, 140));
    assert(!board_boot_gate_poll(&gate, false, 150));
    assert(board_boot_gate_poll(&gate, false, 200));
    assert(!board_boot_gate_poll(&gate, false, 201));
}

int main(void)
{
    check_strap_held_at_start();
    check_bounce_and_short_tap();
    check_release_needed_before_first_press();
    check_clock_regression();
    puts("BOOT: startup release, press/hold/release, debounce and one-shot passed");
    return 0;
}
