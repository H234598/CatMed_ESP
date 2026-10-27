#ifndef KATMED_MESH_H
#define KATMED_MESH_H

#include "katmed_store.h"
#include "katmed_interaction.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All public calls run on the serial application loop, except the SDK receive
 * callback, which only enqueues bounded bytes. A successful shared result is
 * issued only after coordinator acceptance and durable local application. */
typedef enum {
    MESH_ACCEPTED,
    MESH_PENDING,
    MESH_REJECTED,
    MESH_UNAVAILABLE,
    MESH_STORE_ERROR
} KatmedMeshResult;

typedef enum {
    MESH_SHARED_ACCEPTED,
    MESH_SHARED_REJECTED,
    MESH_SHARED_TIMEOUT
} KatmedMeshSharedOutcome;

typedef struct {
    Store *store;
    AppState *state;
    void *context;
    void (*shared_result)(void *context, EventId id, KatmedMeshSharedOutcome outcome);
    void (*utc_received)(void *context, uint64_t observed_mono_ms,
                         int64_t utc_ms, ClockQuality source_quality);
    bool (*time_shareable)(void *context); /* False for a merely manual local anchor. */
    /* Optional three startup-only checkpoints on the calling App loop.
     * Main supplies its existing numeric heap sampler; no readiness implied. */
    void (*startup_heap_sample)(const char *stage);
} KatmedMeshServices;

KatmedMeshResult katmed_mesh_start(const StoreProvision *provision,
                                   const KatmedMeshServices *services);
/* Stop logical message processing immediately. A provisioning change requires
 * a controlled reboot if the bound SDK cannot safely reinitialize in place. */
void katmed_mesh_stop(void);
/* Explicit user action; attempts priority 1 without disturbing a live link. */
bool katmed_mesh_schedule_reconnect(void);
/* Separate history backend; bind before start, on the serial main loop. */
void katmed_mesh_history_backend(StoreBackend backend);
typedef struct {
    uint16_t current, history;
    uint8_t quality;
    bool active, current_valid, history_complete;
} KatmedMeshCounts;
void katmed_mesh_counts(KatmedMeshCounts *out);
void katmed_mesh_poll(const ClockSample *now);
bool katmed_mesh_connected(void);
/* True while this provisioned Mesh is active; channel=0 means unavailable.
 * Reads the current shared Wi-Fi/ESP-NOW radio channel, never configuration. */
bool katmed_mesh_radio_channel(uint8_t *channel);
bool katmed_mesh_is_coordinator(void);
/* Root role AND a current IP_EVENT_STA_GOT_IP without a later LOST_IP. */
bool katmed_mesh_root_has_ip(void);
typedef struct {
    uint32_t sdk_send_failures;
    uint32_t rx_queue_drops;
    uint32_t observed_level_changes;
    uint32_t ip_got_transitions;
    uint32_t ip_lost_transitions;
    uint32_t authenticated_session_up;
    uint32_t authenticated_session_down;
    /* Direct STA reads, independent of Mesh role and IP event counters.
     * Interpret only when station_valid=1. DHCP: 0 INIT, 1 STARTED, 2 STOPPED.
     * IPv4 is API-nonzero; when down the SDK may return a cached address.
     * Separate SDK reads are not an atomic snapshot during transitions. */
    uint32_t station_valid, station_up, station_ipv4_nonzero, station_dhcp_status;
} KatmedMeshDiagnostics;
/* Adapter-observed events only. These are not all radio or SDK failures. */
bool katmed_mesh_diagnostics(KatmedMeshDiagnostics *out);
KatmedMeshResult katmed_mesh_submit_shared(const AppEvent *event,
                                           const ClockSample *now,
                                           AppEffects *effects);
void katmed_mesh_notify_local_commit(void);
KatmedMeshResult katmed_mesh_copy_night(uint64_t now_ms);
void katmed_mesh_night_status(KatmedNightCopyStatus *status);
/* Transient interactions. These never submit medical events. Receiver state
 * is device-wide; only sending and completion dialogs require menu_entry. */
bool katmed_mesh_interaction_input(const KatmedInteractionInput *input,
                                   uint64_t monotonic_ms);
void katmed_mesh_interaction_status(KatmedInteractionStatus *out,
                                    uint64_t monotonic_ms);

#ifdef __cplusplus
}
#endif
#endif
