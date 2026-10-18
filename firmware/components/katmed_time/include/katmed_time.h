#ifndef KATMED_TIME_H
#define KATMED_TIME_H

#include <stdbool.h>
#include <stdint.h>
#include "katmed_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A synchronization is fresh for at most two hours. A valid UTC anchor is
 * retained as holdover after link loss; a new boot starts without UTC. */
#define KATMED_TIME_FRESH_MS (UINT64_C(2) * 60u * 60u * 1000u)

typedef struct {
    uint64_t domain;
    uint64_t anchor_monotonic_ms;
    int64_t anchor_utc_ms;
    ClockQuality source_quality;
    bool utc_known;
    bool linked;
} KatmedTime;

/* domain is the unique, durable boot epoch supplied by the store. */
void katmed_time_init(KatmedTime *time, uint64_t domain);

/* Only a caller that has authenticated and validated the source may accept
 * a mesh synchronization. For local SNTP, call through the IDF adapter.
 * observed_monotonic_ms is this device's receipt time, never a remote clock. */
bool katmed_time_accept_utc(KatmedTime *time, uint64_t observed_monotonic_ms,
                            int64_t utc_ms, ClockQuality source_quality);
void katmed_time_set_link(KatmedTime *time, bool linked);
ClockSample katmed_time_sample(const KatmedTime *time, uint64_t monotonic_ms);

#ifdef __cplusplus
}
#endif
#endif
