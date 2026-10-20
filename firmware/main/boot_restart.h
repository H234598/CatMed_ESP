#ifndef KATMED_BOOT_RESTART_H
#define KATMED_BOOT_RESTART_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool pending;
    bool validating_high;
    uint64_t high_since_ms;
    uint64_t last_high_sample_ms;
} BootRestartState;

typedef struct {
    bool (*pin_high)(void *context);
    bool (*try_reset_lock)(void *context);
    void (*unlock_reset)(void *context);
    bool (*reset_busy)(void *context);
    bool (*reset_prepared)(void *context);
    bool (*cancel_preparation)(void *context);
    uint64_t (*now_ms)(void *context);
    void (*restart)(void *context);
} BootRestartOps;

typedef enum {
    BOOT_RESTART_IDLE,
    BOOT_RESTART_WAIT,
    BOOT_RESTART_LOW,
    BOOT_RESTART_RESET_BUSY,
    BOOT_RESTART_CANCEL_FAILED,
    BOOT_RESTART_TRIGGERED,
} BootRestartResult;

/* Called on each app tick with the debounced press/release event. The lock
 * attempt must be nonblocking. A fresh 50 ms High validation follows any
 * persistent cancellation; the pin is read once more immediately before
 * restart while reset transitions are excluded by the lock. */
BootRestartResult boot_restart_step(BootRestartState *state,
                                    const BootRestartOps *ops, void *context,
                                    bool released_event);

#endif
