#include <cassert>
#include <cstdint>
#include <cstdio>
#include "katmed_core.h"

static constexpr uint64_t minute = 60000;
static ClockSample at(uint64_t ms)
{ return ClockSample{ms, 41, 0, false, CLOCK_UNAVAILABLE}; }

static AlarmAnimation animation(const AppState &state, uint64_t ms)
{
    AlarmAnimation out{};
    ClockSample now = at(ms);
    assert(app_alarm_animation(&state, &now, &out) == APP_OK);
    return out;
}

int main()
{
    ClockSample start = at(1000);
    SharedSettings settings = app_default_settings();
    const uint8_t catalog[KATMED_CATALOG_BYTES] = {7};
    AppState state{};
    assert(app_init(&state, &settings, &start, EventId{7, 41, 1},
                    1, catalog) == APP_OK);
    const AlarmRoundId original_round = state.round;
    assert(animation(state, 1000).color == ALARM_COLOR_BLUE);
    assert(animation(state, 1000).phase_permyriad == 0);
    assert(animation(state, 1000 + 1300).phase_permyriad == 5000);
    assert(animation(state, 1000 + 30 * minute - 1).color == ALARM_COLOR_BLUE);
    auto yellow = animation(state, 1000 + 30 * minute);
    assert(yellow.color == ALARM_COLOR_YELLOW && yellow.frequency_millihz == 577);
    assert(animation(state, 1000 + 60 * minute - 1).color == ALARM_COLOR_YELLOW);
    auto red = animation(state, 1000 + 60 * minute);
    assert(red.color == ALARM_COLOR_RED && red.frequency_millihz == 1000);
    assert(animation(state, 1000 + 90 * minute).frequency_millihz == 1500);
    auto red_end = animation(state, 1000 + 120 * minute);
    assert(red_end.frequency_millihz == 2000);
    assert(red_end.phase_permyriad == red.phase_permyriad);
    assert(animation(state, 1000 + 120 * minute + 250).phase_permyriad ==
           (red_end.phase_permyriad + 5000) % 10000);
    auto very_late = animation(state, 1000 + 50ULL * 24 * 60 * minute);
    assert(very_late.color == ALARM_COLOR_RED && very_late.frequency_millihz == 2000);

    ClockSample tick = at(1000 + 35 * minute);
    AppEffects effects{};
    assert(app_tick(&state, &tick, &effects) == APP_OK);
    assert(state.round.ordinal == original_round.ordinal);
    assert(state.round.origin.device == original_round.origin.device);
    assert(animation(state, tick.monotonic_ms).color == ALARM_COLOR_YELLOW);
    AlarmAnimation untouched{ALARM_COLOR_RED, 9999, 77, 88};
    ClockSample old = tick;
    --old.monotonic_ms;
    assert(app_alarm_animation(&state, &old, &untouched) == APP_STALE);
    assert(untouched.phase_permyriad == 9999 && untouched.elapsed_ms == 88);
    ClockSample foreign = tick;
    foreign.domain = 42;
    assert(app_alarm_animation(&state, &foreign, &untouched) == APP_INVALID);
    assert(untouched.phase_permyriad == 9999);

    AppState huge{};
    ClockSample near_max = at(UINT64_MAX - 2 * 60 * minute);
    assert(app_init(&huge, &settings, &near_max, EventId{8, 41, 1},
                    1, catalog) == APP_OK);
    assert(animation(huge, UINT64_MAX - 1).frequency_millihz == 2000);
    std::puts("independent alarm time stages, phase, round, rollback and 64-bit age: OK");
}
