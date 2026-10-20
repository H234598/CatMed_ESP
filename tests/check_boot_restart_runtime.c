#include <assert.h>
#include <stdio.h>
#include "board_boot_gate.h"
#include "boot_restart.h"

typedef struct {
    BoardBootGate button;
    BootRestartState runtime;
    uint64_t now;
    bool low;
    bool lock_available;
    bool locked;
    bool reset_busy;
    bool prepared;
    bool cancel_ok;
    bool low_during_cancel;
    unsigned cancel_calls;
    unsigned restart_calls;
    unsigned pin_reads;
    unsigned low_on_pin_read;
} Fixture;

static bool pin_high(void *context)
{
    Fixture *f=context;
    ++f->pin_reads;
    if (f->pin_reads == f->low_on_pin_read) f->low = true;
    return !f->low;
}

static bool try_lock(void *context)
{
    Fixture *f=context;
    assert(!f->locked);
    if (!f->lock_available) return false;
    f->locked = true;
    return true;
}

static void unlock(void *context)
{
    Fixture *f=context;
    assert(f->locked);
    f->locked = false;
}

static bool busy(void *context) { return ((Fixture *)context)->reset_busy; }
static bool prepared(void *context) { return ((Fixture *)context)->prepared; }

static bool cancel(void *context)
{
    Fixture *f=context;
    assert(f->locked && f->prepared);
    ++f->cancel_calls;
    f->now += 100; /* A persistent commit may consume the old release window. */
    if (!f->cancel_ok) return false;
    f->prepared = false;
    if (f->low_during_cancel) f->low = true;
    return true;
}

static uint64_t now_ms(void *context) { return ((Fixture *)context)->now; }

static void restart(void *context)
{
    Fixture *f=context;
    assert(f->locked && !f->low && !f->prepared && !f->reset_busy);
    ++f->restart_calls;
}

static const BootRestartOps operations = {
    .pin_high = pin_high, .try_reset_lock = try_lock, .unlock_reset = unlock,
    .reset_busy = busy, .reset_prepared = prepared,
    .cancel_preparation = cancel, .now_ms = now_ms, .restart = restart,
};

static BootRestartResult tick(Fixture *f, uint64_t now, bool low)
{
    f->now = now;
    f->low = low;
    const bool released = board_boot_gate_poll(&f->button, low, now);
    return boot_restart_step(&f->runtime, &operations, f, released);
}

static void start(Fixture *f)
{
    *f = (Fixture){.lock_available = true, .cancel_ok = true};
    assert(tick(f, 0, false) == BOOT_RESTART_IDLE);
    assert(tick(f, 50, false) == BOOT_RESTART_IDLE);
}

static void press_and_release(Fixture *f, uint64_t start)
{
    assert(tick(f, start, true) == BOOT_RESTART_IDLE);
    assert(tick(f, start + 50, true) == BOOT_RESTART_IDLE);
    assert(tick(f, start + 100, false) == BOOT_RESTART_IDLE);
}

static void check_lock_delay_then_new_low(void)
{
    Fixture f;
    start(&f);
    press_and_release(&f, 100);
    f.lock_available = false;
    assert(tick(&f, 250, false) == BOOT_RESTART_WAIT);
    assert(f.runtime.pending && f.restart_calls == 0);
    assert(tick(&f, 300, false) == BOOT_RESTART_WAIT);
    assert(tick(&f, 350, false) == BOOT_RESTART_WAIT);
    f.lock_available = true;
    assert(tick(&f, 375, true) == BOOT_RESTART_LOW);
    assert(!f.runtime.pending && f.restart_calls == 0);
    /* The old event cannot restart even after the lock becomes available. */
    assert(tick(&f, 425, true) == BOOT_RESTART_IDLE);
    assert(tick(&f, 450, false) == BOOT_RESTART_IDLE);
    assert(tick(&f, 500, false) == BOOT_RESTART_WAIT);
    assert(tick(&f, 525, false) == BOOT_RESTART_WAIT);
    assert(tick(&f, 550, false) == BOOT_RESTART_TRIGGERED);
    assert(f.restart_calls == 1 && !f.runtime.pending);
    assert(tick(&f, 600, false) == BOOT_RESTART_IDLE);
    assert(f.restart_calls == 1);
}

static void check_low_during_persistent_cancel(void)
{
    Fixture f;
    start(&f);
    f.prepared = true;
    f.low_during_cancel = true;
    press_and_release(&f, 100);
    assert(tick(&f, 250, false) == BOOT_RESTART_LOW);
    assert(f.cancel_calls == 1 && !f.prepared && f.restart_calls == 0);
    assert(tick(&f, 400, true) == BOOT_RESTART_IDLE);
    assert(tick(&f, 450, true) == BOOT_RESTART_IDLE);
    f.low_during_cancel = false;
    assert(tick(&f, 500, false) == BOOT_RESTART_IDLE);
    assert(tick(&f, 550, false) == BOOT_RESTART_WAIT);
    assert(tick(&f, 575, false) == BOOT_RESTART_WAIT);
    assert(tick(&f, 600, false) == BOOT_RESTART_TRIGGERED);
    assert(f.restart_calls == 1 && f.cancel_calls == 1);
}

static void check_fresh_high_after_successful_cancel(void)
{
    Fixture f;
    start(&f);
    f.prepared = true;
    press_and_release(&f, 100);
    assert(tick(&f, 250, false) == BOOT_RESTART_WAIT);
    assert(f.now == 350 && f.cancel_calls == 1 && f.restart_calls == 0);
    assert(tick(&f, 399, false) == BOOT_RESTART_WAIT);
    assert(tick(&f, 400, false) == BOOT_RESTART_TRIGGERED);
    assert(f.restart_calls == 1);
}

static void check_failed_cancel_refuses_restart(void)
{
    Fixture f;
    start(&f);
    f.prepared = true;
    f.cancel_ok = false;
    press_and_release(&f, 100);
    assert(tick(&f, 250, false) == BOOT_RESTART_CANCEL_FAILED);
    assert(f.prepared && !f.runtime.pending && f.restart_calls == 0);
    assert(tick(&f, 400, false) == BOOT_RESTART_IDLE);
    assert(f.restart_calls == 0); /* No reboot can consume a stale marker. */
    f.cancel_ok = true;
    press_and_release(&f, 500);
    assert(tick(&f, 650, false) == BOOT_RESTART_WAIT);
    assert(f.cancel_calls == 2 && !f.prepared);
    assert(tick(&f, 775, false) == BOOT_RESTART_WAIT);
    assert(tick(&f, 800, false) == BOOT_RESTART_TRIGGERED);
    assert(f.restart_calls == 1);
}

static void check_last_sample_and_busy_reset(void)
{
    Fixture f;
    start(&f);
    press_and_release(&f, 100);
    assert(tick(&f, 250, false) == BOOT_RESTART_WAIT);
    assert(tick(&f, 275, false) == BOOT_RESTART_WAIT);
    /* Force GPIO9 Low on the final read, after lock and elapsed dwell. */
    f.low_on_pin_read = f.pin_reads + 3;
    assert(tick(&f, 300, false) == BOOT_RESTART_LOW);
    assert(f.restart_calls == 0 && !f.runtime.pending);
    f.low_on_pin_read = 0;
    assert(tick(&f, 350, true) == BOOT_RESTART_IDLE);
    assert(tick(&f, 400, true) == BOOT_RESTART_IDLE);
    f.reset_busy = true;
    assert(tick(&f, 450, false) == BOOT_RESTART_IDLE);
    assert(tick(&f, 500, false) == BOOT_RESTART_RESET_BUSY);
    assert(f.restart_calls == 0);
}

static void check_unobserved_poll_gap(void)
{
    Fixture f;
    start(&f);
    press_and_release(&f, 100);
    assert(tick(&f, 250, false) == BOOT_RESTART_WAIT);
    /* A 100 ms main-loop stall may conceal a complete new press. */
    assert(tick(&f, 350, false) == BOOT_RESTART_WAIT);
    assert(f.restart_calls == 0 && f.runtime.high_since_ms == 350);
    assert(tick(&f, 375, false) == BOOT_RESTART_WAIT);
    assert(tick(&f, 400, false) == BOOT_RESTART_TRIGGERED);
    assert(f.restart_calls == 1);
}

int main(void)
{
    check_lock_delay_then_new_low();
    check_low_during_persistent_cancel();
    check_fresh_high_after_successful_cancel();
    check_failed_cancel_refuses_restart();
    check_last_sample_and_busy_reset();
    check_unobserved_poll_gap();
    puts("BOOT runtime: delayed lock, NVS, renewed Low/High and fail-closed reset passed");
    return 0;
}
