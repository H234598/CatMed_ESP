#include "katmed_core.h"

#include <cassert>
#include <cinttypes>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint64_t minute = 60000;
constexpr uint64_t hour = 60 * minute;
constexpr uint8_t catalog[KATMED_CATALOG_BYTES] = {0x42};
unsigned checks = 0;

#define CHECK(x) do { ++checks; if (!(x)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
    std::abort(); } } while (0)

ClockSample clock_at(uint64_t ms, uint64_t domain = 17) {
    return {ms, domain, 0, false, CLOCK_UNAVAILABLE};
}

AppState boot(uint64_t start = 1000) {
    const ClockSample now = clock_at(start);
    const SharedSettings settings = app_default_settings();
    AppState state{};
    CHECK(app_init(&state, &settings, &now, {1, 17, 1}, 1, catalog) == APP_OK);
    return state;
}

AlarmAnimation sample(const AppState &state, uint64_t now) {
    AlarmAnimation out{};
    const ClockSample at = clock_at(now);
    CHECK(app_alarm_animation(&state, &at, &out) == APP_OK);
    return out;
}

void stages_and_phase() {
    const AppState state = boot();
    CHECK(sample(state, 1000).color == ALARM_COLOR_BLUE);
    CHECK(sample(state, 1000).elapsed_ms == 0);
    CHECK(sample(state, 1000).phase_permyriad == 0);
    CHECK(sample(state, 2300).phase_permyriad == 5000);
    const auto before_yellow = sample(state, 1000 + 30 * minute - 1);
    const auto yellow = sample(state, 1000 + 30 * minute);
    CHECK(before_yellow.color == ALARM_COLOR_BLUE);
    CHECK(yellow.color == ALARM_COLOR_YELLOW);
    CHECK(before_yellow.frequency_millihz == 385);
    CHECK(yellow.frequency_millihz == 577);
    // Phase is continuous to permyriad resolution across colour boundaries.
    CHECK(yellow.phase_permyriad >= before_yellow.phase_permyriad);
    CHECK(yellow.phase_permyriad - before_yellow.phase_permyriad <= 4);
    const auto before_red = sample(state, 1000 + hour - 1);
    const auto red = sample(state, 1000 + hour);
    CHECK(before_red.color == ALARM_COLOR_YELLOW);
    CHECK(red.color == ALARM_COLOR_RED);
    CHECK(red.frequency_millihz == 1000);
    CHECK(red.phase_permyriad == 7692);
    CHECK(red.phase_permyriad >= before_red.phase_permyriad);
    CHECK(red.phase_permyriad - before_red.phase_permyriad <= 7);
    CHECK(sample(state, 1000 + 90 * minute).frequency_millihz == 1500);
    CHECK(sample(state, 1000 + 120 * minute - 1).frequency_millihz == 2000);
    CHECK(sample(state, 1000 + 120 * minute).frequency_millihz == 2000);
    CHECK(sample(state, 1000 + 120 * minute).phase_permyriad == red.phase_permyriad);
    const auto late = sample(state, 1000 + 50ULL * 24 * hour);
    CHECK(late.color == ALARM_COLOR_RED && late.frequency_millihz == 2000);
    const auto late_half = sample(state, 1000 + 50ULL * 24 * hour + 250);
    CHECK(late_half.phase_permyriad == (late.phase_permyriad + 5000) % 10000);
}

void stable_origin_and_domains() {
    AppState state = boot();
    const auto initial = sample(state, 1000 + 35 * minute);
    CHECK(initial.color == ALARM_COLOR_YELLOW);
    // View and connection are deliberately outside AppState and this API.
    // Image changes and repeated render/tick calls do not reset the round.
    state.image_id = 0;
    CHECK(sample(state, 1000 + 35 * minute).phase_permyriad == initial.phase_permyriad);
    ClockSample at = clock_at(1000 + 35 * minute);
    AppEffects effects{};
    CHECK(app_tick(&state, &at, &effects) == APP_OK);
    CHECK(app_tick(&state, &at, &effects) == APP_NO_CHANGE);
    CHECK(sample(state, at.monotonic_ms).phase_permyriad == initial.phase_permyriad);
    auto old = at;
    --old.monotonic_ms;
    AlarmAnimation sentinel{ALARM_COLOR_RED, 9999, 2, 3};
    CHECK(app_alarm_animation(&state, &old, &sentinel) == APP_STALE);
    CHECK(sentinel.phase_permyriad == 9999);
    auto foreign_now = at;
    foreign_now.domain = 99;
    CHECK(app_alarm_animation(&state, &foreign_now, &sentinel) == APP_INVALID);

    AppState remote_boot = boot();
    remote_boot.countdown_started.domain = 99;
    const auto local_now = clock_at(1000 + hour);
    CHECK(app_alarm_animation(&remote_boot, &local_now, &sentinel) == APP_TIME_UNAVAILABLE);
    CHECK(sentinel.phase_permyriad == 9999);
    // An explicitly valid shared UTC relation can date that foreign origin.
    constexpr int64_t epoch_2026_ms = 1791973800000LL;
    remote_boot.countdown_started.utc_valid = true;
    remote_boot.countdown_started.utc_ms = epoch_2026_ms;
    remote_boot.countdown_started.quality = CLOCK_SYNCHRONIZED;
    remote_boot.clock.utc_valid = true;
    remote_boot.clock.utc_ms = epoch_2026_ms + hour;
    remote_boot.clock.quality = CLOCK_SYNCHRONIZED;
    auto shared_now = local_now;
    shared_now.utc_valid = true;
    shared_now.utc_ms = epoch_2026_ms + hour;
    shared_now.quality = CLOCK_SYNCHRONIZED;
    CHECK(app_alarm_animation(&remote_boot, &shared_now, &sentinel) == APP_OK);
    CHECK(sentinel.elapsed_ms == hour && sentinel.color == ALARM_COLOR_RED);
    AppState huge = boot(UINT64_MAX - 2 * hour);
    const auto near_max = clock_at(UINT64_MAX - 1);
    CHECK(app_alarm_animation(&huge, &near_max, &sentinel) == APP_OK);
    CHECK(sentinel.color == ALARM_COLOR_RED && sentinel.frequency_millihz == 2000);
    CHECK(sentinel.elapsed_ms == 2 * hour - 1);
}

void actual_due_and_clipped_snapshot_age() {
    AppState state = boot();
    state.start_reason = START_SETTINGS;
    state.deadline.regular_monotonic_ms = state.clock.monotonic_ms;
    state.deadline.effective_monotonic_ms = state.clock.monotonic_ms;
    auto confirmation = clock_at(2000);
    AppEvent event{};
    event.kind = EVENT_CONFIRM;
    event.id = {1, 17, 2};
    event.sender = 1;
    event.round = state.round;
    event.parent_revision = state.revision;
    event.occurred_at = confirmation;
    AppEffects effects{};
    CHECK(app_apply_event(&state, &event, &confirmation, &effects) == APP_OK);
    CHECK(!state.alarm_active);
    AlarmAnimation sentinel{ALARM_COLOR_RED, 9999, 2, 3};
    CHECK(app_alarm_animation(&state, &confirmation, &sentinel) == APP_NO_CHANGE);
    CHECK(sentinel.phase_permyriad == 9999);
    const uint64_t due = state.deadline.effective_monotonic_ms;
    auto due_clock = clock_at(due);
    CHECK(app_tick(&state, &due_clock, &effects) == APP_OK);
    CHECK(state.alarm_active && effects.alarm_started);
    CHECK(sample(state, due).elapsed_ms == 0);
    CHECK(sample(state, due + 30 * minute).color == ALARM_COLOR_YELLOW);
    CHECK(sample(state, due + hour).color == ALARM_COLOR_RED);
    // A deadline clipped to zero after cross-domain adoption cannot be dated
    // by a local monotonic clock alone. Do not accidentally call it new.
    state.deadline.effective_monotonic_ms = 0;
    auto later = clock_at(due + hour);
    CHECK(app_alarm_animation(&state, &later, &sentinel) == APP_TIME_UNAVAILABLE);
    CHECK(sentinel.phase_permyriad == 9999);
    constexpr int64_t epoch_2026_ms = 1791973800000LL;
    state.deadline.effective_utc = {true, epoch_2026_ms};
    state.deadline.regular_utc = {true, epoch_2026_ms};
    later.utc_valid = true;
    later.utc_ms = epoch_2026_ms + hour;
    later.quality = CLOCK_SYNCHRONIZED;
    CHECK(app_alarm_animation(&state, &later, &sentinel) == APP_OK);
    CHECK(sentinel.elapsed_ms == hour && sentinel.color == ALARM_COLOR_RED);
}
}

int main() {
    stages_and_phase();
    stable_origin_and_domains();
    actual_due_and_clipped_snapshot_age();
    std::printf("alarm_time=PASS checks=%u\n", checks);
    return 0;
}
