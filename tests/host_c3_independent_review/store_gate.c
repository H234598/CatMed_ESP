#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define KATMED_FULL_STACK 1
#define KATMED_WITH_CORE 1
#include "../../firmware/main/main.c"

static uint8_t durable[KATMED_STORE_BLOB_MAX];
static size_t durable_length;
static unsigned writes, unknown_on_write;
static int64_t microseconds;

static StoreResult read_backend(void *context, uint8_t *out, size_t *length)
{
    (void)context;
    if (!durable_length) return STORE_NOT_FOUND;
    assert(*length >= durable_length);
    memcpy(out, durable, durable_length);
    *length = durable_length;
    return STORE_OK;
}
static StoreResult write_backend(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    ++writes;
    assert(length <= sizeof(durable));
    memcpy(durable, data, length);
    durable_length = length;
    return writes == unknown_on_write ? STORE_COMMIT_UNKNOWN : STORE_OK;
}
int64_t esp_timer_get_time(void) { return microseconds; }
BoardResult board_set_orientation(Orientation orientation)
{ (void)orientation; return BOARD_OK; }
bool katmed_mesh_connected(void) { return false; }
bool katmed_mesh_is_coordinator(void) { return false; }
void katmed_mesh_notify_local_commit(void) {}
KatmedMeshResult katmed_mesh_submit_shared(const AppEvent *event,
                                            const ClockSample *now,
                                            AppEffects *effects)
{ (void)event; (void)now; (void)effects; return MESH_UNAVAILABLE; }

int main(void)
{
    const StoreBackend backend = {NULL, read_backend, write_backend};
    const uint8_t catalog[KATMED_CATALOG_BYTES] = {3, 4, 5};
    StoreRecovery recovery = {0};
    microseconds = 1000000;
    assert(store_open(&runtime.store, backend, UINT64_C(0xAC276E46BC2C),
                      catalog, 75, &recovery) == STORE_OK);
    runtime.store_ready = true;
    runtime.boot = recovery.boot_identity;
    runtime.boot_domain = UINT64_C(0x87654321);
    katmed_time_init(&runtime.time, runtime.boot_domain);
    ClockSample now = clock_sample(runtime.boot_domain);
    assert(app_init(&runtime.state, &recovery.shared, &now, runtime.boot,
                    75, catalog) == APP_OK);
    assert(runtime.state.alarm_active && writes == 1);
    /* The uncertain commit concerns a real due reminder, not the boot notice. */
    runtime.state.start_reason = START_SETTINGS;
    runtime.state.countdown_started = now;
    runtime.state.deadline.regular_monotonic_ms = now.monotonic_ms;
    runtime.state.deadline.effective_monotonic_ms = now.monotonic_ms;
    UiRequest request = {.kind = UI_REQUEST_CONFIRM};
    microseconds = 2000000;
    unknown_on_write = 3;
    assert(submit_ui(&runtime, &request) == UI_SUBMIT_UNAVAILABLE);
    assert(runtime.state.alarm_active && !runtime.state.has_last_confirmation);
    assert(writes == 3);
    assert(submit_ui(&runtime, &request) == UI_SUBMIT_UNAVAILABLE && writes == 3);
    assert(!storage_ready(&runtime));

    /* The backend may really have persisted the candidate despite reporting
     * uncertainty. Only reopening is allowed to resolve that ambiguity. */
    memset(&runtime, 0, sizeof(runtime));
    unknown_on_write = 0;
    assert(store_open(&runtime.store, backend, UINT64_C(0xAC276E46BC2C),
                      catalog, 75, &recovery) == STORE_OK);
    assert(recovery.has_last_confirmation && recovery.pending_count == 1);
    assert(recovery.last_confirmation_id.sequence == 2);
    puts("independent main/store unknown commit: no false UI success or retry; recovery retains pending confirmation");
    return 0;
}
