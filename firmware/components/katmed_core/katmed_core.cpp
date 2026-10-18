#include "katmed_core.h"

#include <cstring>

namespace {

constexpr uint64_t kSecond = 1000;
constexpr int64_t kDay = 86400000;
constexpr uint64_t kMaxDuration = uint64_t{UINT32_MAX} * kSecond;
// Controlled initial calibration; a physical full cycle is not measured yet.
constexpr uint64_t kBlueCycleMs = 2600;
constexpr uint64_t kStageMs = 1800000;
constexpr uint64_t kRedRampMs = 3600000;
// Europe/Berlin has used these EU transition rules since 1996. Deliberately
// reject historical dates and dates beyond our supported, reviewed range.
// Source: IANA tzdb europe, EU rules (March/October last Sunday, 01:00 UTC).
constexpr int kFirstYear = 1996;
constexpr int kLastYear = 2099;

bool add(uint64_t a, uint64_t b, uint64_t &out) {
    if (b > UINT64_MAX - a) return false;
    out = a + b;
    return true;
}

int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yo = static_cast<unsigned>(y - era * 400);
    const unsigned mp = m > 2 ? m - 3 : m + 9;
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;
    return static_cast<int64_t>(era) * 146097 + yo * 365 + yo / 4 -
           yo / 100 + doy - 719468;
}

LocalDateTime civil_from_ms(int64_t ms) {
    const int64_t days = ms / kDay;
    int64_t z = days + 719468;
    const int64_t era = z / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yo = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = static_cast<int>(yo) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yo + yo / 4 - yo / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    const uint64_t tod = static_cast<uint64_t>(ms % kDay);
    LocalDateTime out{};
    out.year = y; out.month = m; out.day = d;
    out.hour = static_cast<unsigned>(tod / 3600000);
    out.minute = static_cast<unsigned>((tod / 60000) % 60);
    out.second = static_cast<unsigned>((tod / 1000) % 60);
    out.millisecond = static_cast<unsigned>(tod % 1000);
    return out;
}

unsigned last_sunday(int year, unsigned month) {
    const int64_t last = days_from_civil(year, month, 31);
    return 31 - static_cast<unsigned>((last + 4) % 7);
}

bool utc_in_range(int64_t utc) {
    return utc >= days_from_civil(kFirstYear, 1, 1) * kDay &&
           utc < days_from_civil(kLastYear + 1, 1, 1) * kDay - 3600000;
}

bool utc_shift(int64_t utc, uint64_t duration, bool forward, int64_t &out) {
    if (duration > static_cast<uint64_t>(INT64_MAX)) return false;
    const int64_t d = static_cast<int64_t>(duration);
    if (forward ? utc > INT64_MAX - d : utc < INT64_MIN + d) return false;
    out = forward ? utc + d : utc - d;
    return utc_in_range(out);
}

bool id_valid(const EventId &id) {
    return id.device != 0 && id.epoch != 0 && id.sequence != 0;
}
bool id_equal(const EventId &a, const EventId &b) {
    return a.device == b.device && a.epoch == b.epoch && a.sequence == b.sequence;
}
bool id_empty(const EventId &id) {
    return id.device == 0 && id.epoch == 0 && id.sequence == 0;
}
bool round_equal(const AlarmRoundId &a, const AlarmRoundId &b) {
    return a.ordinal == b.ordinal && id_equal(a.origin, b.origin);
}
bool round_empty(const AlarmRoundId &round) {
    return round.ordinal == 0 && id_empty(round.origin);
}
bool revision_equal(const StateRevision &a, const StateRevision &b) {
    return a.number == b.number && a.coordinator == b.coordinator;
}
bool clock_valid(const ClockSample &c) {
    return c.domain != 0 && c.quality >= CLOCK_UNAVAILABLE &&
           c.quality <= CLOCK_SYNCHRONIZED &&
           (c.utc_valid ? (c.quality != CLOCK_UNAVAILABLE && utc_in_range(c.utc_ms))
                        : (c.quality == CLOCK_UNAVAILABLE && c.utc_ms == 0));
}
bool clock_equal(const ClockSample &a, const ClockSample &b) {
    return a.monotonic_ms == b.monotonic_ms && a.domain == b.domain &&
           a.utc_valid == b.utc_valid && a.utc_ms == b.utc_ms && a.quality == b.quality;
}
bool clock_empty(const ClockSample &c) {
    return c.monotonic_ms == 0 && c.domain == 0 && c.utc_ms == 0 &&
           !c.utc_valid && c.quality == CLOCK_UNAVAILABLE;
}
bool instant_valid(const UtcInstant &u) {
    return u.valid ? utc_in_range(u.ms) : u.ms == 0;
}
bool settings_valid(const SharedSettings &s, bool initial) {
    return s.medication_seconds != 0 && s.image_seconds != 0 &&
           s.rotation >= ROTATION_OFF && s.rotation <= ROTATION_RANDOM &&
           (s.revision.coordinator != 0 || (initial && s.revision.number == 0));
}

bool state_valid(const AppState &s) {
    if (s.schema_version != KATMED_SCHEMA_VERSION || !settings_valid(s.settings, false) ||
        s.revision.coordinator == 0 || s.settings.revision.number > s.revision.number ||
        !id_valid(s.round.origin) || s.round.ordinal == 0 || !id_valid(s.countdown_origin) ||
        !clock_valid(s.clock) ||
        !clock_valid(s.countdown_started) || s.start_reason < START_BOOT ||
        s.start_reason >= START_SNAPSHOT || s.recent_event_count > KATMED_RECENT_EVENTS ||
        s.recent_event_next >= KATMED_RECENT_EVENTS ||
        (s.recent_event_count < KATMED_RECENT_EVENTS &&
         s.recent_event_next != s.recent_event_count) ||
        s.deadline.effective_monotonic_ms > s.deadline.regular_monotonic_ms ||
        s.deadline.regular_monotonic_ms - s.deadline.effective_monotonic_ms > kMaxDuration ||
        (s.deadline.regular_monotonic_ms > s.clock.monotonic_ms &&
         s.deadline.regular_monotonic_ms - s.clock.monotonic_ms > kMaxDuration) ||
        !instant_valid(s.deadline.regular_utc) || !instant_valid(s.deadline.effective_utc) ||
        s.deadline.regular_utc.valid != s.deadline.effective_utc.valid ||
        (s.deadline.regular_utc.valid &&
         s.deadline.effective_utc.ms > s.deadline.regular_utc.ms) ||
        s.image_remaining_ms > uint64_t{s.settings.image_seconds} * kSecond ||
        s.image_due_pending != (s.image_remaining_ms == 0) ||
        (s.image_count == 0 ? s.image_id != 0 : s.image_id >= s.image_count) ||
        (s.image_count == 0 && !s.catalog_mismatch) ||
        (!s.alarm_active && s.start_reason == START_BOOT) ||
        (!s.alarm_active && s.clock.monotonic_ms >= s.deadline.effective_monotonic_ms) ||
        (s.start_reason != START_BOOT && !s.has_last_confirmation) ||
        (s.start_reason == START_BOOT && (s.has_last_confirmation || s.round.ordinal != 1)) ||
        (s.confirmation_pending_sync && !s.has_last_confirmation)) return false;
    if (s.countdown_started.domain == s.clock.domain &&
        s.countdown_started.monotonic_ms > s.clock.monotonic_ms) return false;
    if ((s.start_reason == START_BOOT && !id_equal(s.countdown_origin, s.round.origin)) ||
        (s.start_reason == START_CONFIRMATION &&
         !id_equal(s.countdown_origin, s.last_confirmation_id))) return false;
    // Adoption preserves the actual start reason; START_SNAPSHOT is not an
    // alternative reason that could bypass the original interval's limits.
    if (s.start_reason == START_BOOT) {
        if (s.deadline.regular_monotonic_ms != 0 || s.deadline.effective_monotonic_ms != 0 ||
            s.deadline.regular_utc.valid || s.deadline.effective_utc.valid ||
            s.deadline.night_advanced) return false;
    } else {
        const uint64_t duration = s.start_reason == START_MANUAL ? 48ULL * 3600000
            : uint64_t{s.settings.medication_seconds} * kSecond;
        uint64_t latest = 0;
        if (s.deadline.regular_monotonic_ms > s.clock.monotonic_ms &&
            s.deadline.regular_monotonic_ms - s.clock.monotonic_ms > duration) return false;
        if (s.countdown_started.domain == s.clock.domain &&
            (!add(s.countdown_started.monotonic_ms, duration, latest) ||
             s.deadline.regular_monotonic_ms > latest)) return false;
        if (s.countdown_started.utc_valid && s.deadline.regular_utc.valid &&
            s.deadline.regular_utc.ms > s.countdown_started.utc_ms + static_cast<int64_t>(duration))
            return false;
        if (s.start_reason == START_MANUAL && !s.countdown_started.utc_valid) return false;
        if (s.start_reason == START_CONFIRMATION &&
            !clock_equal(s.countdown_started, s.last_confirmation)) return false;
        if (s.alarm_active && s.clock.monotonic_ms < s.deadline.effective_monotonic_ms)
            return false;
    }
    if (s.has_last_confirmation) {
        if (!id_valid(s.last_confirmation_id) || !clock_valid(s.last_confirmation) ||
            !id_valid(s.last_confirmed_round.origin) || s.last_confirmed_round.ordinal == 0 ||
            s.last_confirmed_round.ordinal == UINT64_MAX ||
            s.round.ordinal != s.last_confirmed_round.ordinal + 1 ||
            !id_equal(s.round.origin, s.last_confirmation_id) ||
            (s.last_confirmation.domain == s.clock.domain &&
             s.last_confirmation.monotonic_ms > s.clock.monotonic_ms)) return false;
    } else if (!id_empty(s.last_confirmation_id) || !clock_empty(s.last_confirmation) ||
               !round_empty(s.last_confirmed_round)) {
        return false;
    }
    if (!clock_empty(s.last_settings_change) && !clock_valid(s.last_settings_change))
        return false;
    if (s.last_settings_change.domain == s.clock.domain &&
        s.last_settings_change.monotonic_ms > s.clock.monotonic_ms) return false;
    if (s.has_time_sync) {
        if (!id_valid(s.last_time_sync_id) || !clock_valid(s.last_time_sync) ||
            !s.last_time_sync.utc_valid || s.last_time_sync.domain != s.clock.domain ||
            s.last_time_sync.monotonic_ms > s.clock.monotonic_ms) return false;
    } else if (!id_empty(s.last_time_sync_id) || !clock_empty(s.last_time_sync)) {
        return false;
    }
    for (unsigned i = 0; i < KATMED_RECENT_EVENTS; ++i) {
        if (i >= s.recent_event_count) {
            if (!id_empty(s.recent_events[i])) return false;
            continue;
        }
        if (!id_valid(s.recent_events[i])) return false;
        for (unsigned j = 0; j < i; ++j)
            if (id_equal(s.recent_events[i], s.recent_events[j])) return false;
    }
    return true;
}

AppResult elapsed(const ClockSample &older, const ClockSample &newer, uint64_t &age) {
    if (older.domain == newer.domain) {
        if (older.monotonic_ms > newer.monotonic_ms) return APP_STALE;
        age = newer.monotonic_ms - older.monotonic_ms;
        return APP_OK;
    }
    if (!older.utc_valid || !newer.utc_valid) return APP_TIME_UNAVAILABLE;
    if (older.utc_ms > newer.utc_ms) return APP_STALE;
    age = static_cast<uint64_t>(newer.utc_ms - older.utc_ms);
    return APP_OK;
}

uint64_t remaining(uint64_t due, uint64_t now) { return due > now ? due - now : 0; }

AppResult local_to_utc(int year, unsigned month, unsigned day, unsigned hour,
                       unsigned minute, int64_t candidates[2], unsigned &count) {
    count = 0;
    if (year < kFirstYear || year > kLastYear) return APP_INVALID;
    const int64_t naive = days_from_civil(year, month, day) * kDay +
                          static_cast<int64_t>(hour) * 3600000 + minute * 60000;
    const int offsets[2] = {7200, 3600};
    for (int offset : offsets) {
        const int64_t utc = naive - offset * kSecond;
        LocalDateTime local{};
        if (app_berlin_local_time(utc, &local) == APP_OK && local.year == year &&
            local.month == month && local.day == day && local.hour == hour &&
            local.minute == minute && local.second == 0) candidates[count++] = utc;
    }
    return APP_OK;
}

AppResult night_time(int64_t start, int64_t regular, int64_t &effective, bool &advanced) {
    LocalDateTime local{};
    AppResult result = app_berlin_local_time(regular, &local);
    if (result != APP_OK) return result;
    const uint64_t time = uint64_t{local.hour} * 3600000 + local.minute * 60000 +
                          local.second * 1000 + local.millisecond;
    effective = regular;
    advanced = false;
    if (time >= 60000 && time <= 21600000) {
        int64_t midnight[2]{};
        unsigned count = 0;
        result = local_to_utc(local.year, local.month, local.day, 0, 0, midnight, count);
        if (result != APP_OK || count != 1) return APP_INVALID;
        if (midnight[0] > start) {
            effective = midnight[0];
            advanced = true;
        }
    }
    return APP_OK;
}

void remember(AppState &s, const EventId &id) {
    s.recent_events[s.recent_event_next] = id;
    s.recent_event_next = static_cast<uint8_t>((s.recent_event_next + 1) % KATMED_RECENT_EVENTS);
    if (s.recent_event_count < KATMED_RECENT_EVENTS) ++s.recent_event_count;
}
bool remembered(const AppState &s, const EventId &id) {
    for (unsigned i = 0; i < s.recent_event_count; ++i)
        if (id_equal(s.recent_events[i], id)) return true;
    return s.has_last_confirmation && id_equal(s.last_confirmation_id, id);
}

AppResult utc_for_deadline(const ClockSample &now, uint64_t mono, UtcInstant &utc) {
    if (!now.utc_valid) return APP_OK;
    int64_t value = 0;
    if (!utc_shift(now.utc_ms, mono >= now.monotonic_ms ? mono - now.monotonic_ms
                                                     : now.monotonic_ms - mono,
                   mono >= now.monotonic_ms, value)) return APP_OVERFLOW;
    utc = {true, value};
    return APP_OK;
}

// Reconciliation may shorten a monotonic deadline, never extend it. First
// valid UTC uses the remaining monotonic duration, with no retroactive night
// advance. An active alarm is latched until a real confirmation/adoption.
AppResult reconcile_deadline(AppState &s, const ClockSample &now) {
    if (s.start_reason == START_BOOT || !now.utc_valid) return APP_OK;
    Deadline &d = s.deadline;
    if (s.alarm_active && d.regular_utc.valid) return APP_OK;
    if (!s.alarm_active && d.regular_utc.valid) {
        uint64_t projected = 0;
        const uint64_t regular_left = d.regular_utc.ms > now.utc_ms
            ? static_cast<uint64_t>(d.regular_utc.ms - now.utc_ms) : 0;
        const uint64_t effective_left = d.effective_utc.ms > now.utc_ms
            ? static_cast<uint64_t>(d.effective_utc.ms - now.utc_ms) : 0;
        if (!add(now.monotonic_ms, regular_left, projected)) return APP_OVERFLOW;
        if (projected < d.regular_monotonic_ms) d.regular_monotonic_ms = projected;
        if (!add(now.monotonic_ms, effective_left, projected)) return APP_OVERFLOW;
        if (projected < d.effective_monotonic_ms) d.effective_monotonic_ms = projected;
    }
    AppResult r = utc_for_deadline(now, d.regular_monotonic_ms, d.regular_utc);
    if (r != APP_OK) return r;
    r = utc_for_deadline(now, d.effective_monotonic_ms, d.effective_utc);
    if (r != APP_OK) return r;
    if (!s.alarm_active) {
        int64_t night = 0;
        bool advanced = false;
        r = night_time(now.utc_ms, d.regular_utc.ms, night, advanced);
        if (r != APP_OK) return r;
        if (advanced) {
            uint64_t night_mono = 0;
            if (!add(now.monotonic_ms, static_cast<uint64_t>(night - now.utc_ms), night_mono))
                return APP_OVERFLOW;
            if (night_mono < d.effective_monotonic_ms) {
                d.effective_monotonic_ms = night_mono;
                d.effective_utc = {true, night};
            }
        }
    }
    d.night_advanced = d.effective_monotonic_ms < d.regular_monotonic_ms;
    return APP_OK;
}

AppResult advance(AppState &s, const ClockSample &now, AppEffects &fx) {
    if (!clock_valid(now) || now.domain != s.clock.domain) return APP_INVALID;
    if (now.monotonic_ms < s.clock.monotonic_ms) return APP_STALE;
    AppResult r = reconcile_deadline(s, now);
    if (r != APP_OK) return r;
    const bool becomes_due = !s.alarm_active && now.monotonic_ms >= s.deadline.effective_monotonic_ms;
    if (!s.alarm_active && s.settings.rotation != ROTATION_OFF) {
        uint64_t until = now.monotonic_ms;
        if (becomes_due && s.deadline.effective_monotonic_ms < until)
            until = s.deadline.effective_monotonic_ms;
        const uint64_t delta = until > s.clock.monotonic_ms ? until - s.clock.monotonic_ms : 0;
        if (delta >= s.image_remaining_ms) {
            s.image_remaining_ms = 0;
            s.image_due_pending = true;
        } else {
            s.image_remaining_ms -= delta;
        }
    }
    if (becomes_due) {
        s.alarm_active = true;
        fx.alarm_started = true;
        fx.broadcast_required = true;
    }
    // One bounded step. RANDOM never makes an independent local selection.
    if (!s.alarm_active && s.image_due_pending && s.settings.rotation == ROTATION_NAME_ORDER) {
        if (s.image_count > 1) {
            s.image_id = s.image_id + 1 == s.image_count ? 0 : s.image_id + 1;
            fx.image_changed = true;
            fx.broadcast_required = true;
        }
        s.image_remaining_ms = uint64_t{s.settings.image_seconds} * kSecond;
        s.image_due_pending = false;
    }
    s.clock = now;
    return APP_OK;
}

AppResult start_deadline(AppState &s, const ClockSample &start, const ClockSample &now,
                         uint32_t seconds) {
    uint64_t age = 0;
    AppResult r = elapsed(start, now, age);
    if (r != APP_OK) return r;
    ClockSample calculation = start;
    if (!calculation.utc_valid && now.utc_valid) {
        if (!utc_shift(now.utc_ms, age, false, calculation.utc_ms)) return APP_OVERFLOW;
        calculation.utc_valid = true;
        calculation.quality = now.quality;
    }
    Deadline d{};
    r = app_calculate_deadline(&calculation, seconds, &d);
    if (r != APP_OK) return r;
    if (!start.utc_valid && now.utc_valid && d.night_advanced &&
        d.effective_utc.ms <= now.utc_ms) {
        // UTC became known only now. Reconstructing the original start is
        // evidence for duration, not permission for a retroactive night alarm.
        d.effective_utc = d.regular_utc;
        d.effective_monotonic_ms = d.regular_monotonic_ms;
        d.night_advanced = false;
    }
    const uint64_t regular_duration = d.regular_monotonic_ms - calculation.monotonic_ms;
    const uint64_t effective_duration = d.effective_monotonic_ms - calculation.monotonic_ms;
    // Preserve past deadlines when the event can be represented in this boot
    // domain. Clamping to delivery time would count an already active alarm as
    // image-running time after a delayed offline confirmation.
    const uint64_t local_start = age <= now.monotonic_ms ? now.monotonic_ms - age : now.monotonic_ms;
    const uint64_t regular = age <= now.monotonic_ms ? regular_duration : remaining(regular_duration, age);
    const uint64_t effective = age <= now.monotonic_ms ? effective_duration : remaining(effective_duration, age);
    if (!add(local_start, regular, d.regular_monotonic_ms) ||
        !add(local_start, effective, d.effective_monotonic_ms))
        return APP_OVERFLOW;
    s.deadline = d;
    s.countdown_started = start;
    return APP_OK;
}

bool rebase_due(uint64_t now, uint64_t remaining_at_capture, uint64_t age, uint64_t &due) {
    if (remaining_at_capture >= age) return add(now, remaining_at_capture - age, due);
    const uint64_t past = age - remaining_at_capture;
    due = past > now ? 0 : now - past;
    return true;
}

bool snapshot_valid(const StateSnapshot &snap) {
    const AppState &s = snap.state;
    return snap.schema_version == KATMED_SCHEMA_VERSION && state_valid(s) &&
           clock_valid(snap.captured_at) && clock_equal(snap.captured_at, s.clock) &&
           std::memcmp(snap.catalog_id, s.catalog_id, KATMED_CATALOG_BYTES) == 0 &&
           snap.medication_regular_remaining_ms == remaining(s.deadline.regular_monotonic_ms,
                                                             snap.captured_at.monotonic_ms) &&
           snap.medication_effective_remaining_ms == remaining(s.deadline.effective_monotonic_ms,
                                                               snap.captured_at.monotonic_ms) &&
           snap.image_remaining_ms == s.image_remaining_ms &&
           snap.medication_regular_remaining_ms <= kMaxDuration &&
           snap.medication_effective_remaining_ms <= snap.medication_regular_remaining_ms;
}

AppResult adopt(AppState &local, const StateSnapshot &snap, const ClockSample &now,
                AppEffects &fx) {
    if (!snapshot_valid(snap)) return APP_INVALID;
    const AppState &remote = snap.state;
    /* Independent cold starts have different Boot rounds. After an
     * authenticated coordinator snapshot, a fresh Boot alarm may join that
     * same alarm without manufacturing a confirmation or a new deadline. */
    const bool boot_convergence = local.start_reason == START_BOOT &&
        remote.start_reason == START_BOOT && local.alarm_active &&
        remote.alarm_active && !local.has_last_confirmation &&
        !remote.has_last_confirmation &&
        !id_equal(local.round.origin,remote.round.origin);
    if (remote.revision.number < local.revision.number) return APP_STALE;
    if (remote.revision.number == local.revision.number &&
        remote.revision.coordinator != local.revision.coordinator &&
        !(boot_convergence && remote.revision.number == 0)) return APP_CONFLICT;
    const bool same_revision = revision_equal(remote.revision, local.revision);
    const bool same_countdown = id_equal(remote.countdown_origin, local.countdown_origin);
    if (same_countdown && (!clock_equal(remote.countdown_started, local.countdown_started) ||
                          remote.start_reason != local.start_reason)) return APP_CONFLICT;
    if (same_revision && !boot_convergence &&
        (!round_equal(remote.round, local.round) ||
        !same_countdown ||
        remote.has_last_confirmation != local.has_last_confirmation ||
        (local.has_last_confirmation && !id_equal(local.last_confirmation_id, remote.last_confirmation_id)) ||
        remote.settings.medication_seconds != local.settings.medication_seconds ||
        remote.settings.image_seconds != local.settings.image_seconds ||
        remote.settings.rotation != local.settings.rotation)) return APP_CONFLICT;
    if (local.has_last_confirmation) {
        if (!remote.has_last_confirmation) return APP_STALE;
        if (id_equal(local.last_confirmation_id, remote.last_confirmation_id)) {
            if (!clock_equal(local.last_confirmation, remote.last_confirmation) ||
                !round_equal(local.last_confirmed_round, remote.last_confirmed_round))
                return APP_CONFLICT;
        } else {
            uint64_t since = 0;
            AppResult order = elapsed(local.last_confirmation, remote.last_confirmation, since);
            if (order != APP_OK) return order == APP_TIME_UNAVAILABLE ? APP_CONFLICT : order;
            if (since == 0 || remote.round.ordinal <= local.round.ordinal) return APP_CONFLICT;
        }
    }
    if (local.alarm_active && local.start_reason != START_BOOT && !remote.alarm_active) {
        if (!remote.has_last_confirmation ||
            !round_equal(remote.last_confirmed_round, local.round) ||
            !id_equal(remote.round.origin, remote.last_confirmation_id)) return APP_CONFLICT;
        uint64_t since_due = 0;
        ClockSample due = local.clock;
        due.monotonic_ms = local.deadline.effective_monotonic_ms;
        due.utc_valid = local.deadline.effective_utc.valid;
        due.utc_ms = local.deadline.effective_utc.ms;
        due.quality = due.utc_valid ? CLOCK_HOLDOVER : CLOCK_UNAVAILABLE;
        if (elapsed(due, remote.last_confirmation, since_due) != APP_OK) return APP_CONFLICT;
    }
    uint64_t age = 0;
    AppResult r = elapsed(snap.captured_at, now, age);
    if (r != APP_OK) return r;
    AppState next = remote;
    if (remote.start_reason == START_BOOT) {
        /* Boot has no medicine deadline. Rebasing its zero remaining time to
         * the receiver's current monotonic value would forge one. */
        next.deadline.regular_monotonic_ms = 0;
        next.deadline.effective_monotonic_ms = 0;
    } else if (!rebase_due(now.monotonic_ms, snap.medication_regular_remaining_ms, age,
                           next.deadline.regular_monotonic_ms) ||
               !rebase_due(now.monotonic_ms, snap.medication_effective_remaining_ms, age,
                           next.deadline.effective_monotonic_ms)) return APP_OVERFLOW;
    if (same_countdown && local.start_reason != START_BOOT) {
        if (local.deadline.regular_monotonic_ms < next.deadline.regular_monotonic_ms)
            next.deadline.regular_monotonic_ms = local.deadline.regular_monotonic_ms;
        if (local.deadline.effective_monotonic_ms < next.deadline.effective_monotonic_ms)
            next.deadline.effective_monotonic_ms = local.deadline.effective_monotonic_ms;
        next.alarm_active = local.alarm_active || remote.alarm_active;
    }
    if (!remote.alarm_active && remote.settings.rotation != ROTATION_OFF) {
        const uint64_t running_age = age < snap.medication_effective_remaining_ms
            ? age : snap.medication_effective_remaining_ms;
        next.image_remaining_ms = remaining(snap.image_remaining_ms, running_age);
        next.image_due_pending = next.image_remaining_ms == 0;
    }
    if (same_revision) {
        // app_tick does not issue global revisions. A delayed snapshot with the
        // same revision must never roll a locally advanced scene backwards.
        const bool same_image = next.image_id == local.image_id;
        next.image_id = local.image_id;
        if (!same_image || local.image_remaining_ms < next.image_remaining_ms)
            next.image_remaining_ms = local.image_remaining_ms;
        next.image_due_pending = next.image_remaining_ms == 0;
    }
    const bool previous_alarm = local.alarm_active;
    const uint32_t previous_image = local.image_id;
    // A sync measurement belongs to this receiver's clock, not the sender's.
    next.has_time_sync = local.has_time_sync;
    next.last_time_sync = local.last_time_sync;
    next.last_time_sync_id = local.last_time_sync_id;
    next.clock = now;
    next.catalog_mismatch = remote.catalog_mismatch || remote.image_count != local.image_count ||
                           std::memcmp(remote.catalog_id, local.catalog_id, KATMED_CATALOG_BYTES) != 0;
    // Keep local catalog identity so a foreign image never becomes a valid index.
    if (next.catalog_mismatch) {
        next.image_count = local.image_count;
        next.image_id = local.image_id;
        std::memcpy(next.catalog_id, local.catalog_id, KATMED_CATALOG_BYTES);
    }
    r = advance(next, now, fx);
    if (r != APP_OK) return r;
    fx.alarm_ended = previous_alarm && !next.alarm_active;
    fx.alarm_started = !previous_alarm && next.alarm_active;
    fx.image_changed = previous_image != next.image_id;
    local = next;
    return APP_OK;
}

AppResult finish(AppState *destination, const AppState &next, AppEffects *effects,
                  AppEffects fx, bool persistent) {
    if (!state_valid(next)) return APP_INVALID;
    fx.state_changed = true;
    fx.persistence_required = persistent;
    *destination = next;
    if (effects) *effects = fx;
    return APP_OK;
}

} // namespace

extern "C" {

SharedSettings app_default_settings(void) {
    return {KATMED_DEFAULT_MEDICATION_SECONDS, KATMED_DEFAULT_IMAGE_SECONDS,
            ROTATION_NAME_ORDER, {0, 0}};
}

AppResult app_alarm_animation(const AppState *state, const ClockSample *now,
                              AlarmAnimation *out) {
    if (!state || !now || !out || !state_valid(*state) || !clock_valid(*now) ||
        now->domain != state->clock.domain) return APP_INVALID;
    if (!state->alarm_active) return APP_NO_CHANGE;
    if (now->monotonic_ms < state->clock.monotonic_ms) return APP_STALE;

    uint64_t age = 0;
    if (state->start_reason == START_BOOT) {
        const AppResult r = elapsed(state->countdown_started, *now, age);
        if (r != APP_OK) return r;
    } else if (state->deadline.effective_monotonic_ms != 0) {
        if (now->monotonic_ms < state->deadline.effective_monotonic_ms)
            return APP_STALE;
        age = now->monotonic_ms - state->deadline.effective_monotonic_ms;
    } else {
        // Snapshot rebasing clips a deadline older than the current boot to
        // zero. Only a genuine UTC deadline can recover its original age.
        if (!state->deadline.effective_utc.valid || !now->utc_valid)
            return APP_TIME_UNAVAILABLE;
        if (now->utc_ms < state->deadline.effective_utc.ms) return APP_STALE;
        age = static_cast<uint64_t>(now->utc_ms - state->deadline.effective_utc.ms);
    }

    AlarmAnimation value{};
    value.elapsed_ms = age;
    if (age < kStageMs) {
        value.color = ALARM_COLOR_BLUE;
        value.frequency_millihz = 385; // 1000 / 2.6 seconds, rounded.
        value.phase_permyriad = static_cast<uint16_t>(
            (age % kBlueCycleMs) * 10000 / kBlueCycleMs);
    } else if (age < 2 * kStageMs) {
        value.color = ALARM_COLOR_YELLOW;
        value.frequency_millihz = 577; // 1.5 * blue, rounded.
        const uint64_t numerator = 2 * kStageMs + 3 * (age - kStageMs);
        value.phase_permyriad = static_cast<uint16_t>(
            (numerator % (2 * kBlueCycleMs)) * 10000 / (2 * kBlueCycleMs));
    } else {
        value.color = ALARM_COLOR_RED;
        const uint64_t red_age = age - 2 * kStageMs;
        const uint64_t base_numerator = 2 * kStageMs + 3 * kStageMs;
        const uint64_t base_phase =
            (base_numerator % (2 * kBlueCycleMs)) * 10000 / (2 * kBlueCycleMs);
        if (red_age < kRedRampMs) {
            // Integral of f(t) = 1 + t/3600 s cycles per second. The whole
            // cycle terms vanish modulo 10000; products stay below uint64_t.
            const uint64_t linear_phase = (red_age % 1000) * 10;
            const uint64_t quadratic_phase =
                ((red_age * red_age) % 7200000000ULL) * 10000 / 7200000000ULL;
            value.phase_permyriad = static_cast<uint16_t>(
                (base_phase + linear_phase + quadratic_phase) % 10000);
            value.frequency_millihz = static_cast<uint32_t>(
                1000 + (red_age * 1000 + kRedRampMs / 2) / kRedRampMs);
        } else {
            const uint64_t after_ramp = red_age - kRedRampMs;
            value.phase_permyriad = static_cast<uint16_t>(
                (base_phase + (after_ramp % 500) * 20) % 10000);
            value.frequency_millihz = 2000;
        }
    }
    *out = value;
    return APP_OK;
}

AppResult app_berlin_local_time(int64_t utc_ms, LocalDateTime *local) {
    if (!local || !utc_in_range(utc_ms)) return APP_INVALID;
    const LocalDateTime utc = civil_from_ms(utc_ms);
    const int64_t spring = days_from_civil(utc.year, 3, last_sunday(utc.year, 3)) * kDay + 3600000;
    const int64_t autumn = days_from_civil(utc.year, 10, last_sunday(utc.year, 10)) * kDay + 3600000;
    const bool dst = utc_ms >= spring && utc_ms < autumn;
    LocalDateTime value = civil_from_ms(utc_ms + (dst ? 7200000 : 3600000));
    value.daylight_saving = dst;
    value.utc_offset_seconds = dst ? 7200 : 3600;
    *local = value;
    return APP_OK;
}

AppResult app_calculate_deadline(const ClockSample *start, uint32_t seconds, Deadline *deadline) {
    if (!start || !deadline || !clock_valid(*start) || seconds == 0) return APP_INVALID;
    Deadline value{};
    const uint64_t duration = uint64_t{seconds} * kSecond;
    if (!add(start->monotonic_ms, duration, value.regular_monotonic_ms)) return APP_OVERFLOW;
    value.effective_monotonic_ms = value.regular_monotonic_ms;
    if (start->utc_valid) {
        int64_t regular = 0;
        if (!utc_shift(start->utc_ms, duration, true, regular)) return APP_OVERFLOW;
        int64_t effective = 0;
        AppResult r = night_time(start->utc_ms, regular, effective, value.night_advanced);
        if (r != APP_OK) return r;
        value.regular_utc = {true, regular};
        value.effective_utc = {true, effective};
        if (!add(start->monotonic_ms, static_cast<uint64_t>(effective - start->utc_ms),
                 value.effective_monotonic_ms)) return APP_OVERFLOW;
    }
    *deadline = value;
    return APP_OK;
}

AppResult app_preview_local_time(const ClockSample *now, unsigned hour, unsigned minute,
                                 ManualTimePreview *preview) {
    if (!now || !preview || !clock_valid(*now) || hour > 23 || minute > 59) return APP_INVALID;
    if (!now->utc_valid) return APP_TIME_UNAVAILABLE;
    LocalDateTime local{};
    AppResult r = app_berlin_local_time(now->utc_ms, &local);
    if (r != APP_OK) return r;
    const int64_t first_day = days_from_civil(local.year, local.month, local.day);
    ManualTimePreview value{};
    for (int day = 0; day < 3; ++day) {
        const LocalDateTime date = civil_from_ms((first_day + day) * kDay);
        int64_t candidates[2]{};
        unsigned count = 0;
        r = local_to_utc(date.year, date.month, date.day, hour, minute, candidates, count);
        if (r != APP_OK) return r;
        if (count == 0) value.skipped_nonexistent_time = true;
        for (unsigned i = 0; i < count; ++i) {
            if (candidates[i] <= now->utc_ms) continue;
            value.regular_utc = {true, candidates[i]};
            int64_t effective = 0;
            r = night_time(now->utc_ms, candidates[i], effective, value.night_advanced);
            if (r != APP_OK) return r;
            value.effective_utc = {true, effective};
            if (app_berlin_local_time(candidates[i], &value.regular_local) != APP_OK ||
                app_berlin_local_time(effective, &value.effective_local) != APP_OK) return APP_INVALID;
            *preview = value;
            return APP_OK;
        }
    }
    return APP_INVALID;
}

AppResult app_parse_hours(const char *text, uint32_t *seconds) {
    if (!text || !seconds) return APP_INVALID;
    uint64_t hours = 0, numerator = 0, denominator = 1;
    bool separator = false, digit = false, fraction_digit = false;
    unsigned length = 0, decimals = 0;
    for (const char *p = text; *p != '\0'; ++p) {
        if (++length > 32) return APP_INVALID;
        if (*p == '.' || *p == ',') {
            if (separator) return APP_INVALID;
            separator = true;
            continue;
        }
        if (*p < '0' || *p > '9') return APP_INVALID;
        digit = true;
        const unsigned n = static_cast<unsigned>(*p - '0');
        if (!separator) {
            if (hours > UINT32_MAX / 3600u / 10u + 1u) return APP_OVERFLOW;
            hours = hours * 10 + n;
            if (hours > UINT32_MAX / 3600u) return APP_OVERFLOW;
        } else {
            fraction_digit = true;
            if (++decimals > 9) return APP_INVALID;
            numerator = numerator * 10 + n;
            denominator *= 10;
        }
    }
    if (!digit || (separator && !fraction_digit)) return APP_INVALID;
    // Round half up to a whole second using integers; never binary float.
    const uint64_t fraction = (numerator * 3600 + denominator / 2) / denominator;
    const uint64_t value = hours * 3600 + fraction;
    if (value == 0) return APP_INVALID;
    if (value > UINT32_MAX) return APP_OVERFLOW;
    *seconds = static_cast<uint32_t>(value);
    return APP_OK;
}

AppResult app_init(AppState *state, const SharedSettings *settings, const ClockSample *now,
                   EventId boot_identity, uint32_t image_count,
                   const uint8_t catalog_id[KATMED_CATALOG_BYTES]) {
    if (!state || !settings || !now || !catalog_id || !settings_valid(*settings, true) ||
        !clock_valid(*now) || !id_valid(boot_identity)) return APP_INVALID;
    AppState value{};
    value.schema_version = KATMED_SCHEMA_VERSION;
    value.settings = *settings;
    if (value.settings.revision.coordinator == 0) value.settings.revision.coordinator = boot_identity.device;
    value.revision = value.settings.revision;
    value.round = {boot_identity, 1};
    value.alarm_active = true;
    value.start_reason = START_BOOT;
    value.clock = *now;
    value.countdown_started = *now;
    value.countdown_origin = boot_identity;
    value.image_count = image_count;
    value.image_remaining_ms = uint64_t{settings->image_seconds} * kSecond;
    value.catalog_mismatch = image_count == 0;
    std::memcpy(value.catalog_id, catalog_id, KATMED_CATALOG_BYTES);
    *state = value;
    return APP_OK;
}

AppResult app_tick(AppState *state, const ClockSample *now, AppEffects *effects) {
    if (effects) *effects = {};
    if (!state || !now || !state_valid(*state)) return APP_INVALID;
    AppState next = *state;
    AppEffects fx{};
    AppResult r = advance(next, *now, fx);
    if (r != APP_OK) return r;
    if (clock_equal(state->clock, next.clock) && !fx.alarm_started && !fx.image_changed &&
        state->image_remaining_ms == next.image_remaining_ms &&
        state->image_due_pending == next.image_due_pending &&
        state->deadline.effective_monotonic_ms == next.deadline.effective_monotonic_ms &&
        state->deadline.regular_utc.valid == next.deadline.regular_utc.valid)
        return APP_NO_CHANGE;
    return finish(state, next, effects, fx, false);
}

AppResult app_make_snapshot(const AppState *state, const ClockSample *now, StateSnapshot *snapshot) {
    if (!state || !now || !snapshot || !state_valid(*state)) return APP_INVALID;
    AppState next = *state;
    AppEffects fx{};
    const AppResult r = advance(next, *now, fx);
    if (r != APP_OK) return r;
    StateSnapshot value{};
    value.schema_version = KATMED_SCHEMA_VERSION;
    value.state = next;
    value.captured_at = *now;
    std::memcpy(value.catalog_id, next.catalog_id, KATMED_CATALOG_BYTES);
    value.medication_regular_remaining_ms = remaining(next.deadline.regular_monotonic_ms, now->monotonic_ms);
    value.medication_effective_remaining_ms = remaining(next.deadline.effective_monotonic_ms, now->monotonic_ms);
    value.image_remaining_ms = next.image_remaining_ms;
    *snapshot = value;
    return APP_OK;
}

AppResult app_apply_event(AppState *state, const AppEvent *event,
                          const ClockSample *now, AppEffects *effects) {
    if (effects) *effects = {};
    if (!state || !event || !now || !state_valid(*state) || !clock_valid(*now) ||
        now->domain != state->clock.domain || event->kind < EVENT_CONFIRM ||
        event->kind > EVENT_SET_IMAGE || !id_valid(event->id) ||
        event->sender != event->id.device || !clock_valid(event->occurred_at) ||
        !id_valid(event->round.origin) || event->round.ordinal == 0 ||
        event->parent_revision.coordinator == 0) return APP_INVALID;
    if (remembered(*state, event->id) || (state->has_time_sync &&
        id_equal(state->last_time_sync_id, event->id))) return APP_DUPLICATE;
    if (event->parent_revision.number < state->revision.number) return APP_STALE;
    if (!revision_equal(event->parent_revision, state->revision)) return APP_CONFLICT;
    if (!round_equal(event->round, state->round)) return APP_STALE;
    if (event->kind != EVENT_CONFIRM && event->kind != EVENT_TIME_SYNC &&
        !event->coordinator_accepted) return APP_NOT_AUTHORIZED;
    uint64_t event_age = 0;
    AppResult r = elapsed(event->occurred_at, *now, event_age);
    if (r != APP_OK) return r;
    // A tick may already have rotated images or entered an alarm. We retain no
    // unbounded presentation history and must not claim a historical settings
    // change succeeded with a different effect. The transport can fetch the
    // coordinator's complete snapshot. A true confirmation is different: its
    // alarm had paused the image clock, so its original time remains usable.
    if (event->kind != EVENT_CONFIRM && event->kind != EVENT_TIME_SYNC &&
        event->kind != EVENT_ADOPT_SNAPSHOT &&
        (event_age > now->monotonic_ms ||
         now->monotonic_ms - event_age < state->clock.monotonic_ms)) return APP_STALE;
    ClockSample processing = *now;
    if (event->kind == EVENT_TIME_SYNC) {
        const ClockSample &sample = event->value.time_sync;
        if (!clock_valid(sample) || !sample.utc_valid) return APP_INVALID;
        if (sample.domain != now->domain) return APP_TIME_UNAVAILABLE;
        if (sample.monotonic_ms > now->monotonic_ms) return APP_STALE;
        if (state->has_time_sync && sample.monotonic_ms <= state->last_time_sync.monotonic_ms)
            return APP_STALE;
        if (!utc_shift(sample.utc_ms, now->monotonic_ms - sample.monotonic_ms,
                       true, processing.utc_ms)) return APP_OVERFLOW;
        processing.utc_valid = true;
        processing.quality = sample.quality;
    }
    AppState next = *state;
    AppEffects fx{};
    ClockSample before_event = processing;
    if (event->kind != EVENT_TIME_SYNC && event->kind != EVENT_ADOPT_SNAPSHOT &&
        event_age <= processing.monotonic_ms &&
        processing.monotonic_ms - event_age >= state->clock.monotonic_ms) {
        before_event.monotonic_ms -= event_age;
        if (before_event.utc_valid && !utc_shift(processing.utc_ms, event_age, false,
                                                before_event.utc_ms)) return APP_OVERFLOW;
    }
    r = advance(next, before_event, fx);
    if (r != APP_OK) return r;
    bool revision_change = true;
    switch (event->kind) {
    case EVENT_CONFIRM: {
        if (!next.alarm_active) return APP_STALE;
        if (next.round.ordinal == UINT64_MAX) return APP_OVERFLOW;
        const uint64_t due = next.start_reason == START_BOOT
            ? next.countdown_started.monotonic_ms : next.deadline.effective_monotonic_ms;
        if (event_age > processing.monotonic_ms || processing.monotonic_ms - event_age < due)
            return APP_STALE;
        r = start_deadline(next, event->occurred_at, processing, next.settings.medication_seconds);
        if (r != APP_OK) return r;
        next.has_last_confirmation = true;
        next.last_confirmation_id = event->id;
        next.last_confirmed_round = next.round;
        next.last_confirmation = event->occurred_at;
        next.confirmation_pending_sync = !event->coordinator_accepted;
        next.round = {event->id, next.round.ordinal + 1};
        next.start_reason = START_CONFIRMATION;
        next.countdown_origin = event->id;
        next.alarm_active = false;
        fx.alarm_ended = true;
        // The image timer was paused. Resume at the actual confirmation time,
        // not at delayed delivery time, then process at most one pending image.
        next.clock = processing;
        next.clock.monotonic_ms -= event_age;
        if (next.clock.utc_valid && !utc_shift(processing.utc_ms, event_age, false,
                                               next.clock.utc_ms)) return APP_OVERFLOW;
        r = advance(next, processing, fx);
        if (r != APP_OK) return r;
        break;
    }
    case EVENT_SET_MEDICATION_INTERVAL:
        if (event->value.interval_seconds == 0) return APP_INVALID;
        if (next.alarm_active) return APP_CONFLICT;
        next.settings.medication_seconds = event->value.interval_seconds;
        r = start_deadline(next, event->occurred_at, processing, event->value.interval_seconds);
        if (r != APP_OK) return r;
        next.start_reason = START_SETTINGS;
        next.countdown_origin = event->id;
        next.last_settings_change = event->occurred_at;
        r = advance(next, processing, fx);
        if (r != APP_OK) return r;
        break;
    case EVENT_SET_IMAGE_INTERVAL:
        if (event->value.interval_seconds == 0) return APP_INVALID;
        next.settings.image_seconds = event->value.interval_seconds;
        next.image_remaining_ms = uint64_t{event->value.interval_seconds} * kSecond;
        if (before_event.monotonic_ms == processing.monotonic_ms &&
            !next.alarm_active && next.settings.rotation != ROTATION_OFF)
            next.image_remaining_ms = remaining(next.image_remaining_ms, event_age);
        next.image_due_pending = next.image_remaining_ms == 0;
        next.last_settings_change = event->occurred_at;
        r = advance(next, processing, fx);
        if (r != APP_OK) return r;
        break;
    case EVENT_SET_ROTATION_MODE:
        if (event->value.rotation < ROTATION_OFF || event->value.rotation > ROTATION_RANDOM)
            return APP_INVALID;
        next.settings.rotation = event->value.rotation;
        next.last_settings_change = event->occurred_at;
        r = advance(next, processing, fx);
        if (r != APP_OK) return r;
        break;
    case EVENT_SET_NEXT_LOCAL_TIME: {
        if (!event->value.local_time.selected_utc.valid ||
            !instant_valid(event->value.local_time.selected_utc) ||
            !event->value.local_time.selected_effective_utc.valid ||
            !instant_valid(event->value.local_time.selected_effective_utc)) return APP_INVALID;
        if (next.alarm_active) return APP_CONFLICT;
        ManualTimePreview preview{};
        r = app_preview_local_time(&processing, event->value.local_time.hour,
                                    event->value.local_time.minute, &preview);
        if (r != APP_OK) return r;
        if (preview.regular_utc.ms != event->value.local_time.selected_utc.ms ||
            preview.effective_utc.ms != event->value.local_time.selected_effective_utc.ms)
            return APP_STALE;
        next.deadline.regular_utc = preview.regular_utc;
        next.deadline.effective_utc = preview.effective_utc;
        next.deadline.night_advanced = preview.night_advanced;
        if (!add(processing.monotonic_ms, static_cast<uint64_t>(preview.regular_utc.ms - processing.utc_ms),
                 next.deadline.regular_monotonic_ms) ||
            !add(processing.monotonic_ms, static_cast<uint64_t>(preview.effective_utc.ms - processing.utc_ms),
                 next.deadline.effective_monotonic_ms)) return APP_OVERFLOW;
        next.countdown_started = processing;
        next.start_reason = START_MANUAL;
        next.countdown_origin = event->id;
        next.last_settings_change = event->occurred_at;
        break;
    }
    case EVENT_TIME_SYNC:
        revision_change = false;
        next.has_time_sync = true;
        next.last_time_sync = event->value.time_sync;
        next.last_time_sync_id = event->id;
        break;
    case EVENT_ADOPT_SNAPSHOT:
        if (!event->value.snapshot) return APP_INVALID;
        r = adopt(next, *event->value.snapshot, processing, fx);
        if (r != APP_OK) return r;
        revision_change = false;
        break;
    case EVENT_SET_IMAGE:
        if (next.alarm_active || next.settings.rotation == ROTATION_OFF) return APP_CONFLICT;
        if (next.catalog_mismatch || next.image_count == 0 || event->value.image_id >= next.image_count)
            return APP_INVALID;
        if (next.settings.rotation == ROTATION_RANDOM && next.image_count > 1 &&
            event->value.image_id == next.image_id) return APP_INVALID;
        fx.image_changed = next.image_id != event->value.image_id;
        next.image_id = event->value.image_id;
        next.image_remaining_ms = uint64_t{next.settings.image_seconds} * kSecond;
        next.image_due_pending = false;
        break;
    }
    r = advance(next, processing, fx);
    if (r != APP_OK) return r;
    if (revision_change) {
        if (next.revision.number == UINT64_MAX) return APP_OVERFLOW;
        ++next.revision.number;
        if (event->kind == EVENT_SET_MEDICATION_INTERVAL || event->kind == EVENT_SET_IMAGE_INTERVAL ||
            event->kind == EVENT_SET_ROTATION_MODE || event->kind == EVENT_SET_NEXT_LOCAL_TIME)
            next.settings.revision = next.revision;
    }
    if (!remembered(next, event->id)) remember(next, event->id);
    fx.broadcast_required = fx.broadcast_required || revision_change;
    const bool persistent = event->kind != EVENT_TIME_SYNC &&
        (event->kind != EVENT_ADOPT_SNAPSHOT || !revision_equal(state->revision, next.revision) ||
         state->confirmation_pending_sync != next.confirmation_pending_sync);
    return finish(state, next, effects, fx, persistent);
}

} // extern "C"
