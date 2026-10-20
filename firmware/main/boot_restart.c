#include "boot_restart.h"

#define BOOT_RESTART_HIGH_MS 50u

static void discard(BootRestartState *state)
{
    state->pending = false;
    state->validating_high = false;
}

BootRestartResult boot_restart_step(BootRestartState *state,
                                    const BootRestartOps *ops, void *context,
                                    bool released_event)
{
    if (!state || !ops || !ops->pin_high || !ops->try_reset_lock ||
        !ops->unlock_reset || !ops->reset_busy || !ops->reset_prepared ||
        !ops->cancel_preparation || !ops->now_ms || !ops->restart)
        return BOOT_RESTART_IDLE;
    if (released_event) {
        state->pending = true;
        state->validating_high = false;
    }
    if (!state->pending) return BOOT_RESTART_IDLE;
    if (!ops->pin_high(context)) {
        discard(state);
        return BOOT_RESTART_LOW;
    }
    if (!ops->try_reset_lock(context)) return BOOT_RESTART_WAIT;
    if (ops->reset_busy(context)) {
        discard(state);
        ops->unlock_reset(context);
        return BOOT_RESTART_RESET_BUSY;
    }
    bool cancelled = false;
    if (ops->reset_prepared(context)) {
        if (!ops->cancel_preparation(context)) {
            discard(state);
            ops->unlock_reset(context);
            return BOOT_RESTART_CANCEL_FAILED;
        }
        cancelled = true;
    }
    if (!ops->pin_high(context)) {
        discard(state);
        ops->unlock_reset(context);
        return BOOT_RESTART_LOW;
    }
    const uint64_t now = ops->now_ms(context);
    /* The app normally polls every 25 ms. A gap of 50 ms could hide a full
     * debounced new press, so it cannot count as observed continuous High. */
    if (!state->validating_high || cancelled ||
        now < state->last_high_sample_ms ||
        now - state->last_high_sample_ms >= BOOT_RESTART_HIGH_MS) {
        state->validating_high = true;
        state->high_since_ms = now;
        state->last_high_sample_ms = now;
        ops->unlock_reset(context);
        return BOOT_RESTART_WAIT;
    }
    state->last_high_sample_ms = now;
    if (now - state->high_since_ms < BOOT_RESTART_HIGH_MS) {
        ops->unlock_reset(context);
        return BOOT_RESTART_WAIT;
    }
    /* No log or blocking operation may be inserted between this raw sample
     * and esp_restart(). A later fresh Low needs a new debounced release. */
    if (!ops->pin_high(context)) {
        discard(state);
        ops->unlock_reset(context);
        return BOOT_RESTART_LOW;
    }
    discard(state);
    ops->restart(context);
    /* The host callback returns; esp_restart() on the device does not. */
    ops->unlock_reset(context);
    return BOOT_RESTART_TRIGGERED;
}
