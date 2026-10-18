#include "katmed_core.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

static constexpr uint64_t minute = 60000;
static constexpr int64_t epoch = INT64_C(1791973800000);
static constexpr uint8_t catalog[KATMED_CATALOG_BYTES] = {0x42, 0x17};

static ClockSample clock_at(uint64_t ms, uint64_t domain, int64_t utc,
                            bool utc_valid) {
    return ClockSample{ms, domain, utc, utc_valid,
                       utc_valid ? CLOCK_SYNCHRONIZED : CLOCK_UNAVAILABLE};
}

static AppState boot(uint64_t device, const ClockSample &start) {
    SharedSettings settings = app_default_settings();
    AppState state{};
    assert(app_init(&state, &settings, &start, EventId{device, 7, 1},
                    75, catalog) == APP_OK);
    return state;
}

static AppResult adopt(AppState &receiver, const StateSnapshot &snapshot,
                       const ClockSample &received) {
    AppEvent event{};
    event.kind = EVENT_ADOPT_SNAPSHOT;
    event.id = EventId{snapshot.state.round.origin.device, 7, 2};
    event.sender = event.id.device;
    event.round = receiver.round;
    event.parent_revision = receiver.revision;
    event.occurred_at = received;
    event.coordinator_accepted = true;
    event.value.snapshot = &snapshot;
    AppEffects effects{};
    return app_apply_event(&receiver, &event, &received, &effects);
}

int main() {
    const ClockSample root_start = clock_at(1000, 111, epoch, true);
    AppState root = boot(91, root_start);
    const ClockSample capture = clock_at(1000 + 32 * minute, 111,
                                          epoch + 32 * static_cast<int64_t>(minute), true);
    StateSnapshot snapshot{};
    assert(app_make_snapshot(&root, &capture, &snapshot) == APP_OK);

    const ClockSample receiver_start = clock_at(2000, 222,
                                                 capture.utc_ms + 5000, true);
    AppState receiver = boot(17, receiver_start);
    assert(adopt(receiver, snapshot, receiver_start) == APP_OK);
    assert(receiver.alarm_active && receiver.start_reason == START_BOOT);
    assert(receiver.round.origin.device == 91);
    assert(receiver.clock.domain == 222 && receiver.countdown_started.domain == 111);
    AlarmAnimation phase{};
    assert(app_alarm_animation(&receiver, &receiver_start, &phase) == APP_OK);
    assert(phase.color == ALARM_COLOR_YELLOW);
    assert(phase.elapsed_ms == 32 * minute + 5000);

    const ClockSample red_time = clock_at(receiver_start.monotonic_ms + 28 * minute,
                                          222, receiver_start.utc_ms + 28 * minute, true);
    assert(app_alarm_animation(&receiver, &red_time, &phase) == APP_OK);
    assert(phase.color == ALARM_COLOR_RED);
    assert(phase.elapsed_ms == 60 * minute + 5000);
    const AlarmAnimation twice = phase;
    assert(app_alarm_animation(&receiver, &red_time, &phase) == APP_OK);
    assert(phase.color == twice.color &&
           phase.phase_permyriad == twice.phase_permyriad &&
           phase.frequency_millihz == twice.frequency_millihz &&
           phase.elapsed_ms == twice.elapsed_ms);

    ClockSample rollback = receiver_start;
    --rollback.monotonic_ms;
    const AlarmAnimation unchanged = phase;
    assert(app_alarm_animation(&receiver, &rollback, &phase) == APP_STALE);
    assert(phase.color == unchanged.color &&
           phase.phase_permyriad == unchanged.phase_permyriad &&
           phase.frequency_millihz == unchanged.frequency_millihz &&
           phase.elapsed_ms == unchanged.elapsed_ms);

    // Identical numeric milliseconds from two unrelated boots cannot supply
    // an ordering when neither source has valid UTC.
    const ClockSample root_untimed = clock_at(1000, 333, 0, false);
    root = boot(91, root_untimed);
    const ClockSample untimed_capture = clock_at(1000 + 2 * minute, 333, 0, false);
    assert(app_make_snapshot(&root, &untimed_capture, &snapshot) == APP_OK);
    const ClockSample untimed_receiver = clock_at(1000 + 2 * minute, 444, 0, false);
    receiver = boot(17, untimed_receiver);
    const AlarmRoundId own_round = receiver.round;
    assert(adopt(receiver, snapshot, untimed_receiver) == APP_TIME_UNAVAILABLE);
    assert(receiver.round.origin.device == own_round.origin.device);
    assert(receiver.alarm_active && receiver.start_reason == START_BOOT);
    std::puts("P7 adopted alarm keeps source age; foreign untimed boot is conservative");
}
