#ifndef KATMED_STORE_H
#define KATMED_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "katmed_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KATMED_STORE_PENDING_MAX 8u
#define KATMED_STORE_CONFLICT_MAX 8u
#define KATMED_STORE_BLOB_MAX 4096u
#define KATMED_STORE_WIFI_PROFILE_MAX 4u
#define KATMED_STORE_USB_LINE_MAX 512u
#define KATMED_STORE_USB_REPLY_MAX 256u
#define KATMED_DEVICE_NAME_MAX 17u
#define KATMED_MESH_NAME_MAX_ENTRIES 64u /* Bounded durable lifetime registry. */
#define KATMED_MESH_NAME_MAX_INDEX UINT16_MAX /* Exhaustion is explicit; indexes never wrap. */

#define KATMED_DEFAULT_DEVICE_NAME "KatmedESP"

typedef enum {
    STORE_MESH_NAME_INITIAL = 0,
    STORE_MESH_NAME_ASSIGNED = 1
} StoreMeshNameState;

typedef struct {
    uint64_t device;
    uint16_t index;
} StoreMeshNameEntry;

typedef enum {
    STORE_OK, STORE_NOT_FOUND, STORE_INVALID, STORE_CORRUPT,
    STORE_UNSUPPORTED, STORE_FULL, STORE_IO, STORE_COMMIT_UNKNOWN,
    STORE_DUPLICATE, STORE_STALE, STORE_UNAVAILABLE
} StoreResult;

/* write_commit must atomically replace the one blob or report uncertainty.
 * A failed/uncertain commit poisons the Store until it is reopened. */
typedef struct {
    void *context;
    StoreResult (*read)(void *context, uint8_t *buffer, size_t *length);
    StoreResult (*write_commit)(void *context, const uint8_t *buffer, size_t length);
} StoreBackend;

typedef struct {
    EventId id;
    uint64_t sender;
    AlarmRoundId round;
    StateRevision parent_revision;
    ClockSample occurred_at;
} StorePendingConfirm;

typedef struct {
    bool valid;
    EventId id;
    AlarmRoundId round;
    ClockSample occurred_at;
} StoreConfirmationHistory;

/* A coordinator-accepted manual deadline retained while the boot alarm takes
 * priority. It is only a durable projection of the existing Core event. */
typedef struct {
    bool pending;
    uint8_t hour, minute;
    UtcInstant regular_utc, effective_utc;
    StateRevision revision;
    EventId source;
    uint8_t bound_group[16];
} StoreManualIntent;

typedef struct {
    uint8_t group[16];
    uint8_t secret[32];
    uint8_t ssid[32];
    uint8_t ssid_length;
    uint8_t password[63];
    uint8_t password_length;
    uint8_t time_source[63];
    uint8_t time_source_length;
    bool configured;
} StoreProvision;

typedef enum {
    STORE_WIFI_CREDENTIAL_KEEP = 0,
    STORE_WIFI_CREDENTIAL_REPLACE,
    STORE_WIFI_CREDENTIAL_OPEN,
} StoreWifiCredentialAction;

typedef struct {
    uint32_t id;
    uint32_t priority;
    uint8_t ssid[32];
    uint8_t ssid_length;
    uint8_t password[63];
    uint8_t password_length;
    bool secured;
} StoreWifiProfile;

typedef struct {
    uint32_t id; /* zero allocates a new stable ID */
    uint32_t priority;
    uint8_t ssid[32];
    uint8_t ssid_length;
    StoreWifiCredentialAction credential_action;
    uint8_t password[63];
    uint8_t password_length;
} StoreWifiProfileUpdate;

typedef enum {
    STORE_GROUP_ORIGIN_UNKNOWN = 0, /* Existing v1-v5 records have no proof. */
    STORE_GROUP_ORIGIN_FRESH_UNBOUND = 1, /* Only a newly created empty backend. */
    STORE_GROUP_ORIGIN_FIRST_BOUND = 2 /* First binding has committed. */
} StoreGroupOrigin;

typedef struct {
    uint64_t device;
    uint64_t epoch;
    uint64_t next_sequence;
    uint8_t catalog_id[KATMED_CATALOG_BYTES];
    uint32_t image_count;
    SharedSettings shared;
    LocalSettings local;
    bool has_history;
    AppState history; /* Previous boot domain: never use as active runtime state. */
    StorePendingConfirm pending[KATMED_STORE_PENDING_MAX];
    uint8_t pending_count;
    StorePendingConfirm conflicts[KATMED_STORE_CONFLICT_MAX]; /* Durable, nonapplicable confirmations. */
    uint8_t conflict_count;
    bool group_divergent; /* Independent of one historical boot state. */
    StoreProvision provision;
    uint8_t bound_group[16]; /* Durable provenance, retained by RESET CONFIG. */
    StoreConfirmationHistory last_confirmation; /* Independent of the latest boot-domain state. */
    StoreGroupOrigin group_origin;
    StoreManualIntent manual_intent;
    StoreMeshNameState mesh_name_state;
    char mesh_name[KATMED_DEVICE_NAME_MAX];
    uint16_t mesh_name_index;
    StoreMeshNameEntry mesh_names[KATMED_MESH_NAME_MAX_ENTRIES];
    uint8_t mesh_name_count;
    uint32_t mesh_name_next_index; /* 65536 is the durable exhausted sentinel. */
    StoreWifiProfile wifi_profiles[KATMED_STORE_WIFI_PROFILE_MAX];
    uint8_t wifi_profile_count;
    uint32_t next_wifi_profile_id;
} StoreRecord;

typedef struct {
    StoreBackend backend;
    StoreRecord record;
    StoreRecord staged;
    uint8_t scratch[KATMED_STORE_BLOB_MAX];
    uint8_t current_catalog[KATMED_CATALOG_BYTES];
    uint32_t current_image_count;
    StoreResult last_error;
    bool open;
    bool uncertain;
    bool catalog_mismatch;
    /* Per Store instance; read-only diagnostics, never serialized to NVS. */
    uint32_t backend_read_failures;
    uint32_t decoded_load_failures;
    uint32_t backend_commit_failures;
} Store;

typedef struct {
    uint32_t backend_read_failures;
    uint32_t decoded_load_failures;
    uint32_t backend_commit_failures;
} StoreDiagnostics;

/* Valid only while the same Store instance remains open. */
bool store_diagnostics(const Store *store, StoreDiagnostics *out);

typedef struct {
    SharedSettings shared;
    LocalSettings local;
    EventId boot_identity; /* Durably reserved, unique across reboot. */
    bool has_last_confirmation;
    EventId last_confirmation_id;
    AlarmRoundId last_confirmed_round;
    ClockSample last_confirmation; /* Historical time domain only. */
    uint8_t pending_count;
    bool catalog_mismatch;
    bool configured;
    StoreManualIntent manual_intent;
} StoreRecovery;

typedef struct {
    const char *firmware_version;
    const char *build_id;
    uint64_t device;
    ClockQuality clock_quality;
    uint8_t catalog_id[KATMED_CATALOG_BYTES];
} StoreUsbStatusInfo;

StoreResult store_open(Store *store, StoreBackend backend, uint64_t device,
                       const uint8_t catalog_id[KATMED_CATALOG_BYTES],
                       uint32_t image_count, StoreRecovery *recovery);
StoreResult store_get_recovery(const Store *store, StoreRecovery *recovery);
StoreResult store_next_event_id(Store *store, EventId *id);
/* Atomically burns a contiguous ID range before use. Unused IDs are skipped
 * after reboot; an uncertain commit yields no usable ID and poisons Store. */
StoreResult store_reserve_event_ids(Store *store, uint32_t count, EventId *first);
StoreResult store_apply_event(Store *store, AppState *active, const AppEvent *event,
                              const ClockSample *now, bool queue_offline,
                              AppEffects *effects, AppResult *core_result);
StoreResult store_set_local(Store *store, LocalSettings *active,
                            const LocalSettings *requested);
StoreResult store_clear_manual_intent(Store *store);
StoreResult store_ack_pending(Store *store, AppState *active, EventId id,
                              bool coordinator_accepted);
StoreResult store_pending_at(const Store *store, size_t index,
                             StorePendingConfirm *pending);
/* ACK status 2 is permitted only after one of these commits succeeds or a
 * byte-identical conflict is already durable. Neither alters the alarm. */
StoreResult store_record_conflict(Store *store, AppState *active,
                                  const StorePendingConfirm *conflict,
                                  const ClockSample *now);
StoreResult store_reject_pending(Store *store, AppState *active, EventId id);
StoreResult store_mark_divergent(Store *store, AppState *active);
StoreResult store_conflict_count(const Store *store, uint8_t *count);
StoreResult store_conflict_at(const Store *store, size_t index,
                              StorePendingConfirm *conflict);
bool store_durable_divergent(const Store *store);
const char *store_mesh_name(const Store *store);
uint16_t store_mesh_name_index(const Store *store);
uint32_t store_mesh_name_next_index(const Store *store);
bool store_mesh_name_has_device(const Store *store, uint64_t device);
StoreResult store_mesh_name_assign(Store *store, uint64_t device, uint16_t index);
StoreResult store_mesh_name_merge(Store *store, AppState *active,
                                  const StoreMeshNameEntry *entries,
                                  uint8_t count);
/* Authenticated current-coordinator snapshot replaces a leaf's stale view. */
StoreResult store_mesh_name_adopt(Store *store,
                                  const StoreMeshNameEntry *entries,
                                  uint8_t count);
StoreResult store_get_status(const Store *store, bool *configured,
                             uint8_t *pending_count, StoreResult *last_error);
StoreResult store_usb_handle_line(Store *store, const char *line, size_t length,
                                  const StoreUsbStatusInfo *status,
                                  char *reply, size_t reply_capacity);
StoreResult store_usb_handle_line_with_wifi_defaults(
    Store *store, const char *line, size_t length,
    const StoreUsbStatusInfo *status,
    const uint8_t *default_ssid, size_t default_ssid_length,
    const uint8_t *default_password, size_t default_password_length,
    char *reply, size_t reply_capacity);
StoreResult store_wifi_profiles_load(const Store *store, StoreWifiProfile *out,
                                     size_t capacity, size_t *count);
StoreResult store_wifi_profiles_save_atomic(Store *store,
                                            const StoreWifiProfileUpdate *profiles,
                                            size_t count);
StoreResult store_wifi_profile_delete_atomic(Store *store, uint32_t id);
size_t store_wifi_profile_limit(void);

#ifdef __cplusplus
}
#endif
#endif
