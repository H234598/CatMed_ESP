#ifndef KATMED_RUNTIME_METRICS_H
#define KATMED_RUNTIME_METRICS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t last_ms;
    bool started;
} KatmedRuntimeMetricsGate;

typedef struct {
    uint64_t uptime_ms;
    uint64_t pack_hash_us;
    uint64_t variant_hash_us;
    uint64_t jpeg_decode_us;
    uint64_t blink_cycle_us;
    uint32_t blink_flush_pixels;
    uint32_t blink_flush_segments;
    uint32_t blink_cycle_count;
    uint32_t blink_changes;
    uint32_t internal_min_free_bytes;
    uint32_t internal_largest_block_bytes;
    uint32_t main_stack_hwm_bytes;
    uint32_t lvgl_stack_hwm_bytes;
    bool pack_hash_valid;
    bool variant_hash_valid;
    bool jpeg_decode_valid;
    bool blink_cycle_valid;
    bool lvgl_stack_valid;
    int32_t pack_hash_status;
    int32_t variant_hash_status;
    int32_t jpeg_decode_status;
    uint32_t image_failed_presentations;
    bool image_failure_valid;
    uint32_t variant_sequence;
    uint32_t variant_io_stage;
    uint32_t variant_io_status;
    /* Separate retained JPEG and touch producers, not one transaction. */
    bool native_diagnostic_valid, jpeg_detail_valid;
    uint32_t jpeg_sequence, jpeg_stage, jpeg_mcus;
    uint32_t touch_init, touch_probe, touch_sequence, touch_transfer, touch_sample;
} KatmedRuntimeMetricInput;

/* The two additional lines describe one serialized post-reducer sample.
 * Values are copied from the core/adapter, never independently recomputed. */
typedef struct {
    uint64_t seq, uptime_ms, boot_domain;
    uint64_t round_device, round_epoch, round_sequence, round_ordinal;
    uint64_t origin_device, origin_epoch, origin_sequence;
    uint64_t start_domain, start_ms;
    int64_t start_utc_ms;
    uint64_t clock_domain;
    uint64_t regular_ms, effective_ms;
    int64_t regular_utc_ms, effective_utc_ms, now_utc_ms;
    uint32_t alarm, reason, start_utc_valid, deadline_local;
    uint32_t regular_utc_valid, effective_utc_valid, now_utc_valid;
    uint32_t now_quality, utc_source;
    uint32_t image_id, image_pending;
    uint64_t image_remaining_ms;
} KatmedRuntimeEvidenceState;

typedef struct {
    uint64_t seq, uptime_ms, boot_domain;
    uint32_t image_valid, image_failures;
    uint32_t store_valid, store_read_failures, store_load_failures,
        store_commit_failures;
    uint32_t mesh_valid, mesh_send_failures, mesh_queue_drops,
        mesh_level_changes, mesh_ip_got, mesh_ip_lost,
        mesh_session_up, mesh_session_down;
    uint32_t mesh_connected, mesh_root, mesh_root_ip;
} KatmedRuntimeEvidenceHealth;

/* One bounded snapshot of the actual KM1 STATUS input/reply path. Counters
 * saturate; reply_call does not attest USB transmission or host receipt. */
typedef struct {
    uint64_t phase_seq, uptime_ms, boot_domain;
    uint32_t task, read, status, queue_ok, queue_drop;
    uint32_t main_take, reply_ready, reply_call;
} KatmedUsbPhase;

/* First observation is immediate; subsequent observations are at least ten
 * seconds apart. A backwards clock sample opens a fresh interval. */
bool katmed_runtime_metrics_due(KatmedRuntimeMetricsGate *gate, uint64_t now_ms);

/* Samples SDK and LVGL diagnostics without writing application state. */
void katmed_runtime_metrics_collect(uint64_t now_ms, KatmedRuntimeMetricInput *out);

/* Bounded, numeric diagnostics only; never includes credentials or KM1 data. */
void katmed_runtime_metrics_log(const KatmedRuntimeMetricInput *input);
void katmed_runtime_evidence_log(const KatmedRuntimeEvidenceState *state,
                                 const KatmedRuntimeEvidenceHealth *health);
void katmed_runtime_usb_phase_log(const KatmedUsbPhase *phase);

#endif
