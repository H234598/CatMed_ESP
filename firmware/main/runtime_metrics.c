#include "runtime_metrics.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stddef.h>

bool katmed_runtime_metrics_due(KatmedRuntimeMetricsGate *gate, uint64_t now_ms)
{
    if (gate == NULL) return false;
    if (!gate->started || now_ms < gate->last_ms ||
        now_ms - gate->last_ms >= 10000u) {
        gate->last_ms = now_ms;
        gate->started = true;
        return true;
    }
    return false;
}

void katmed_runtime_metrics_log(const KatmedRuntimeMetricInput *input)
{
    if (input == NULL) return;
    ESP_LOGI("katmed_metrics",
        "uptime_ms=%" PRIu64 " pack_hash_us=%" PRIu64 " pack_valid=%u"
        " variant_hash_us=%" PRIu64 " variant_valid=%u"
        " jpeg_decode_us=%" PRIu64 " jpeg_valid=%u"
        " blink_cycle_us=%" PRIu64 " blink_valid=%u"
        " blink_pixels=%" PRIu32 " blink_segments=%" PRIu32
        " blink_cycles=%" PRIu32 " blink_changes=%" PRIu32
        " pack_status=%" PRId32 " variant_status=%" PRId32 " jpeg_status=%" PRId32
        " internal_min_free=%" PRIu32 " internal_largest_block=%" PRIu32
        " main_stack_hwm_bytes=%" PRIu32 " lvgl_stack_hwm_bytes=%" PRIu32
        " lvgl_stack_valid=%u",
        input->uptime_ms, input->pack_hash_us, input->pack_hash_valid,
        input->variant_hash_us, input->variant_hash_valid,
        input->jpeg_decode_us, input->jpeg_decode_valid,
        input->blink_cycle_us, input->blink_cycle_valid,
        input->blink_flush_pixels, input->blink_flush_segments,
        input->blink_cycle_count, input->blink_changes,
        input->pack_hash_status, input->variant_hash_status,
        input->jpeg_decode_status,
        input->internal_min_free_bytes, input->internal_largest_block_bytes,
        input->main_stack_hwm_bytes, input->lvgl_stack_hwm_bytes,
        input->lvgl_stack_valid);
}

void katmed_runtime_evidence_log(const KatmedRuntimeEvidenceState *s,
                                 const KatmedRuntimeEvidenceHealth *h)
{
    if (s == NULL || h == NULL || s->seq != h->seq ||
        s->uptime_ms != h->uptime_ms || s->boot_domain != h->boot_domain) return;
    ESP_LOGI("katmed_state",
        "schema=1 sample_seq=%" PRIu64 " uptime_ms=%" PRIu64 " boot_domain=%" PRIu64
        " round_d=%" PRIu64 " round_e=%" PRIu64 " round_s=%" PRIu64
        " round_o=%" PRIu64 " alarm=%" PRIu32 " reason=%" PRIu32
        " origin_d=%" PRIu64 " origin_e=%" PRIu64 " origin_s=%" PRIu64
        " start_d=%" PRIu64 " start_ms=%" PRIu64 " start_v=%" PRIu32
        " start_utc=%" PRId64 " clock_d=%" PRIu64
        " due_r=%" PRIu64 " due_e=%" PRIu64 " due_local=%" PRIu32
        " due_rv=%" PRIu32 " due_ru=%" PRId64
        " due_ev=%" PRIu32 " due_eu=%" PRId64
        " now_v=%" PRIu32 " now_utc=%" PRId64 " now_q=%" PRIu32
        " utc_source=%" PRIu32 " image_id=%" PRIu32
        " image_rem=%" PRIu64 " image_pending=%" PRIu32,
        s->seq,s->uptime_ms,s->boot_domain,
        s->round_device,s->round_epoch,s->round_sequence,s->round_ordinal,
        s->alarm,s->reason,s->origin_device,s->origin_epoch,s->origin_sequence,
        s->start_domain,s->start_ms,s->start_utc_valid,s->start_utc_ms,
        s->clock_domain,s->regular_ms,s->effective_ms,s->deadline_local,
        s->regular_utc_valid,s->regular_utc_ms,s->effective_utc_valid,
        s->effective_utc_ms,s->now_utc_valid,s->now_utc_ms,s->now_quality,
        s->utc_source,s->image_id,s->image_remaining_ms,s->image_pending);
    ESP_LOGI("katmed_health",
        "schema=1 sample_seq=%" PRIu64 " uptime_ms=%" PRIu64 " boot_domain=%" PRIu64
        " image_v=%" PRIu32 " image_fail=%" PRIu32
        " store_v=%" PRIu32 " store_read=%" PRIu32
        " store_load=%" PRIu32 " store_commit=%" PRIu32
        " mesh_v=%" PRIu32 " mesh_send=%" PRIu32
        " mesh_queue=%" PRIu32 " mesh_level=%" PRIu32
        " mesh_ip_got=%" PRIu32 " mesh_ip_lost=%" PRIu32
        " mesh_up=%" PRIu32 " mesh_down=%" PRIu32
        " mesh_link=%" PRIu32 " mesh_root=%" PRIu32
        " mesh_root_ip=%" PRIu32,
        h->seq,h->uptime_ms,h->boot_domain,h->image_valid,h->image_failures,
        h->store_valid,h->store_read_failures,h->store_load_failures,
        h->store_commit_failures,h->mesh_valid,h->mesh_send_failures,
        h->mesh_queue_drops,h->mesh_level_changes,h->mesh_ip_got,
        h->mesh_ip_lost,h->mesh_session_up,h->mesh_session_down,
        h->mesh_connected,h->mesh_root,h->mesh_root_ip);
}

void katmed_runtime_usb_phase_log(const KatmedUsbPhase *p)
{
    if (p == NULL || p->boot_domain == 0 || p->phase_seq == 0) return;
    ESP_LOGI("katmed_usb_phase",
        "schema=1 phase_seq=%" PRIu64 " uptime_ms=%" PRIu64
        " boot_domain=%" PRIu64 " task=%" PRIu32 " read=%" PRIu32
        " status=%" PRIu32 " queue_ok=%" PRIu32
        " queue_drop=%" PRIu32 " main_take=%" PRIu32
        " reply_ready=%" PRIu32 " reply_call=%" PRIu32,
        p->phase_seq,p->uptime_ms,p->boot_domain,p->task,p->read,p->status,
        p->queue_ok,p->queue_drop,p->main_take,p->reply_ready,p->reply_call);
}
