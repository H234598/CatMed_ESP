#include "katmed_store.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint64_t device = UINT64_C(0xAC276E46BC2C);
uint8_t catalog[KATMED_CATALOG_BYTES] = {1};
struct Memory {
    uint8_t blob[KATMED_STORE_BLOB_MAX]{};
    size_t length = 0;
    unsigned commits = 0;
};
StoreResult read_blob(void *ctx, uint8_t *out, size_t *length) {
    auto &m = *static_cast<Memory *>(ctx);
    if (!m.length) return STORE_NOT_FOUND;
    if (m.length > *length) return STORE_CORRUPT;
    std::memcpy(out, m.blob, m.length);
    *length = m.length;
    return STORE_OK;
}
StoreResult write_blob(void *ctx, const uint8_t *data, size_t length) {
    auto &m = *static_cast<Memory *>(ctx);
    if (length > sizeof(m.blob)) return STORE_FULL;
    std::memcpy(m.blob, data, length);
    m.length = length;
    ++m.commits;
    return STORE_OK;
}
StoreBackend backend(Memory &m) { return {&m, read_blob, write_blob}; }
ClockSample clock(uint64_t epoch, uint64_t ms) {
    return {ms, epoch, 0, false, CLOCK_UNAVAILABLE};
}
AppState boot(const StoreRecovery &r, uint64_t ms) {
    AppState state{};
    const ClockSample now = clock(r.boot_identity.epoch, ms);
    assert(app_init(&state, &r.shared, &now, r.boot_identity, 75, catalog) == APP_OK);
    return state;
}
// Use the same valid Core/Store flow as the qualified rebind fixture.
// Closing a boot notice must not create a medication confirmation.
void prepare_due_alarm(Store &store, AppState &state, ClockSample &now) {
    assert(state.alarm_active && state.start_reason == START_BOOT);
    AppEvent boot_confirm{};
    boot_confirm.kind = EVENT_CONFIRM;
    assert(store_next_event_id(&store, &boot_confirm.id) == STORE_OK);
    boot_confirm.sender = device;
    boot_confirm.round = state.round;
    boot_confirm.parent_revision = state.revision;
    boot_confirm.occurred_at = now;
    const StoreRecord record_before = store.record;
    const AppState state_before = state;
    const auto &memory = *static_cast<Memory *>(store.backend.context);
    const unsigned commits_before = memory.commits;
    AppEffects effects{};
    AppResult core = APP_INVALID;
    assert(store_apply_event(&store, &state, &boot_confirm, &now,
                             true, &effects, &core) == STORE_STALE);
    assert(core == APP_STALE && !store.uncertain &&
           std::memcmp(&record_before, &store.record, sizeof(record_before)) == 0 &&
           std::memcmp(&state_before, &state, sizeof(state_before)) == 0 &&
           memory.commits == commits_before &&
           !effects.persistence_required && !effects.broadcast_required);
    assert(app_dismiss_boot_alarm(&state, &now, &effects) == APP_OK);
    assert(!state.alarm_active && !state.has_last_confirmation &&
           !effects.persistence_required && !effects.broadcast_required &&
           memory.commits == commits_before);

    AppEvent interval{};
    interval.kind = EVENT_SET_MEDICATION_INTERVAL;
    assert(store_next_event_id(&store, &interval.id) == STORE_OK);
    interval.sender = device;
    interval.round = state.round;
    interval.parent_revision = state.revision;
    interval.occurred_at = now;
    interval.coordinator_accepted = true;
    interval.value.interval_seconds = 1;
    assert(store_apply_event(&store, &state, &interval, &now,
                             false, &effects, &core) == STORE_OK);
    assert(core == APP_OK && effects.persistence_required &&
           state.start_reason == START_SETTINGS && !state.alarm_active &&
           !store.record.last_confirmation.valid && store.record.pending_count == 0);
    now.monotonic_ms += 1000;
    assert(app_tick(&state, &now, &effects) == APP_OK);
    assert(state.alarm_active && state.start_reason == START_SETTINGS &&
           !state.has_last_confirmation);
}
}

int main() {
    Memory m{};
    Store first{};
    StoreRecovery recovery{};
    assert(store_open(&first, backend(m), device, catalog, 75, &recovery) == STORE_OK);
    AppState first_state = boot(recovery, 1000);
    ClockSample confirmed_at = clock(recovery.boot_identity.epoch, 1000);
    prepare_due_alarm(first, first_state, confirmed_at);
    EventId confirm_id{};
    assert(store_next_event_id(&first, &confirm_id) == STORE_OK);
    AppEvent confirm{};
    confirm.kind = EVENT_CONFIRM;
    confirm.id = confirm_id;
    confirm.sender = device;
    confirm.round = first_state.round;
    confirm.parent_revision = first_state.revision;
    confirm.occurred_at = confirmed_at;
    AppEffects effects{};
    AppResult core{};
    const ClockSample delivered_at = clock(recovery.boot_identity.epoch, 3500);
    assert(store_apply_event(&first, &first_state, &confirm, &delivered_at,
                             true, &effects, &core) == STORE_OK);
    assert(core == APP_OK && first_state.has_last_confirmation &&
           first_state.start_reason == START_CONFIRMATION);
    StorePendingConfirm queued{};
    assert(store_pending_at(&first, 0, &queued) == STORE_OK);
    assert(queued.occurred_at.monotonic_ms == confirmed_at.monotonic_ms);

    Store second{};
    assert(store_open(&second, backend(m), device, catalog, 75, &recovery) == STORE_OK);
    assert(recovery.has_last_confirmation && recovery.pending_count == 1);
    assert(recovery.last_confirmation.monotonic_ms == confirmed_at.monotonic_ms);
    const EventId historical_id = recovery.last_confirmation_id;
    AppState second_state = boot(recovery, 1000);
    const ClockSample change_at = clock(recovery.boot_identity.epoch, 2000);
    EventId change_id{};
    assert(store_next_event_id(&second, &change_id) == STORE_OK);
    AppEvent change{};
    change.kind = EVENT_SET_IMAGE_INTERVAL;
    change.id = change_id;
    change.sender = device;
    change.round = second_state.round;
    change.parent_revision = second_state.revision;
    change.occurred_at = change_at;
    change.coordinator_accepted = true;
    change.value.interval_seconds = second_state.settings.image_seconds + 1;
    assert(store_apply_event(&second, &second_state, &change, &change_at,
                             false, &effects, &core) == STORE_OK);

    Store third{};
    assert(store_open(&third, backend(m), device, catalog, 75, &recovery) == STORE_OK);
    if (!recovery.has_last_confirmation ||
        recovery.last_confirmation_id.device != historical_id.device ||
        recovery.last_confirmation_id.epoch != historical_id.epoch ||
        recovery.last_confirmation_id.sequence != historical_id.sequence) {
        std::fprintf(stderr, "historical confirmation lost after a valid persistent event\n");
        return 1;
    }
    assert(recovery.pending_count == 1 &&
           recovery.last_confirmation.monotonic_ms == confirmed_at.monotonic_ms &&
           recovery.last_confirmation.domain == confirmed_at.domain);
    std::puts("boot confirmation rejected without mutation; valid due-alarm confirmation retained across reopens and persistent settings change");
}
