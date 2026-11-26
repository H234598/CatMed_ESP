#ifndef KATMED_CONFIG_BRIDGE_DIAG_H
#define KATMED_CONFIG_BRIDGE_DIAG_H

#include <stdint.h>
#include "katmed_render_trace.h"

/* Tick values are monotonic FreeRTOS ticks modulo TickType_t wrap. Presence is
 * explicit: zero can be a real value, and absent events must not be inferred.
 * All fields belong to one exchange, never to a prior global request. */
typedef enum {
    KATMED_CONFIG_TRACE_GATE_START = 1u << 0,
    KATMED_CONFIG_TRACE_GATE_RETURN = 1u << 1,
    KATMED_CONFIG_TRACE_ENQUEUE = 1u << 2,
    KATMED_CONFIG_TRACE_POLL_ENTER = 1u << 3,
    KATMED_CONFIG_TRACE_POLL_COMPLETE = 1u << 4,
    KATMED_CONFIG_TRACE_WAIT_RETURN = 1u << 5,
    KATMED_CONFIG_TRACE_DECISION = 1u << 6,
    KATMED_CONFIG_TRACE_PHASE_ENQUEUE = 1u << 7,
    KATMED_CONFIG_TRACE_PHASE_DECISION = 1u << 8,
    KATMED_CONFIG_TRACE_PHASE_SEEN = 1u << 9,
    KATMED_CONFIG_TRACE_RENDER_INNER = 1u << 10
} KatmedConfigTraceValid;

typedef enum {
    KATMED_CONFIG_RENDER_INNER_NONE = 0,
    KATMED_CONFIG_RENDER_INNER_ACTIVE = 1,
    KATMED_CONFIG_RENDER_INNER_ENDED = 2,
    KATMED_CONFIG_RENDER_INNER_MULTI = 3
} KatmedConfigRenderInnerPeriod;

typedef enum {
    KATMED_CONFIG_RENDER_STATUS_LOCK = 1u << 0,
    KATMED_CONFIG_RENDER_STATUS_SELECT = 1u << 1,
    KATMED_CONFIG_RENDER_STATUS_PRESENT = 1u << 2
} KatmedConfigRenderStatusValid;

typedef enum {
    KATMED_CONFIG_TRACE_SLOT_IDLE = 0,
    KATMED_CONFIG_TRACE_SLOT_QUEUED = 1,
    KATMED_CONFIG_TRACE_SLOT_PROCESSING = 2,
    KATMED_CONFIG_TRACE_SLOT_DONE = 3
} KatmedConfigTraceSlot;

typedef enum {
    KATMED_CONFIG_OWNER_PHASE_UNKNOWN = 0,
    KATMED_CONFIG_OWNER_PHASE_PRE_POLL = 1,
    KATMED_CONFIG_OWNER_PHASE_POLL = 2,
    KATMED_CONFIG_OWNER_PHASE_WEB = 3,
    KATMED_CONFIG_OWNER_PHASE_OTHER = 4,
    KATMED_CONFIG_OWNER_PHASE_RENDER_CALL = 5,
    KATMED_CONFIG_OWNER_PHASE_SLEEP = 6
} KatmedConfigOwnerPhase;

typedef struct {
    uint32_t valid;
    uint32_t tick_period_ms;
    uint32_t gate_start_tick, gate_return_tick, enqueue_tick;
    uint32_t poll_enter_tick, poll_complete_tick, wait_return_tick, decision_tick;
    uint32_t wait_elapsed_ms, render_overlap_ms, owner_phase_seen_mask;
    uint8_t gate_acquired, wait_signalled, slot_at_decision;
    uint8_t owner_phase_at_enqueue, owner_phase_at_decision;
    /* Only the one render period bound to this request. All inner statuses and
     * open flags describe trace freeze, which can precede Web decision after
     * Owner completion. Pre-enqueue marks completed work from this same period
     * without counting its duration as request overlap. MULTI suppresses it. */
    uint32_t inner_seen_mask, inner_multi_mask;
    uint32_t inner_pre_enqueue_mask;
    uint32_t inner_open_enqueue_mask, inner_open_freeze_mask;
    uint32_t inner_overlap_ms[KATMED_RENDER_INNER_COUNT];
    uint8_t inner_period_at_freeze, inner_status_valid;
    uint8_t inner_lock_success, inner_select_status, inner_present_status;
} KatmedConfigBridgeTrace;

#endif
