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
}

int main() {
    Memory m{};
    Store first{};
    StoreRecovery recovery{};
    assert(store_open(&first, backend(m), device, catalog, 75, &recovery) == STORE_OK);
    AppState first_state = boot(recovery, 1000);
    const ClockSample confirmed_at = clock(recovery.boot_identity.epoch, 2000);
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
    std::puts("historical confirmation retained");
}
