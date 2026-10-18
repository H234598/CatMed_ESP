#include "katmed_time.h"

#include <limits.h>
#include <string.h>
#include "katmed_core.h"

void katmed_time_init(KatmedTime *time, uint64_t domain)
{
    if (!time) return;
    memset(time, 0, sizeof(*time));
    time->domain = domain;
}

bool katmed_time_accept_utc(KatmedTime *time, uint64_t observed_monotonic_ms,
                            int64_t utc_ms, ClockQuality source_quality)
{
    LocalDateTime local;
    if (!time || !time->domain ||
        (source_quality != CLOCK_HOLDOVER && source_quality != CLOCK_SYNCHRONIZED) ||
        (time->utc_known && observed_monotonic_ms <= time->anchor_monotonic_ms) ||
        app_berlin_local_time(utc_ms, &local) != APP_OK) return false;
    time->anchor_monotonic_ms = observed_monotonic_ms;
    time->anchor_utc_ms = utc_ms;
    time->source_quality = source_quality;
    time->utc_known = true;
    return true;
}

void katmed_time_set_link(KatmedTime *time, bool linked)
{
    if (time) time->linked = linked;
}

ClockSample katmed_time_sample(const KatmedTime *time, uint64_t monotonic_ms)
{
    ClockSample result = {
        .monotonic_ms = monotonic_ms,
        .domain = time ? time->domain : 0,
        .utc_ms = 0,
        .utc_valid = false,
        .quality = CLOCK_UNAVAILABLE,
    };
    if (!time || !time->utc_known || monotonic_ms < time->anchor_monotonic_ms)
        return result;
    const uint64_t elapsed = monotonic_ms - time->anchor_monotonic_ms;
    if (elapsed > INT64_MAX || time->anchor_utc_ms > INT64_MAX - (int64_t)elapsed)
        return result;
    const int64_t utc_ms = time->anchor_utc_ms + (int64_t)elapsed;
    LocalDateTime local;
    if (app_berlin_local_time(utc_ms, &local) != APP_OK) return result;
    result.utc_ms = utc_ms;
    result.utc_valid = true;
    result.quality = time->source_quality == CLOCK_SYNCHRONIZED &&
                     time->linked && elapsed <= KATMED_TIME_FRESH_MS
        ? CLOCK_SYNCHRONIZED : CLOCK_HOLDOVER;
    return result;
}
