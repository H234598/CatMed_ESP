#ifndef KATMED_CONFIG_BRIDGE_H
#define KATMED_CONFIG_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include "katmed_types.h"
#include "katmed_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KATMED_CONFIG_GET,
    KATMED_CONFIG_PREVIEW_TIME,
    KATMED_CONFIG_SAVE,
    KATMED_CONFIG_SETUP_GET, KATMED_CONFIG_SETUP_SAVE, KATMED_CONFIG_SETUP_EXIT, KATMED_CONFIG_HA_GET,
    KATMED_CONFIG_LANGUAGE_GET, KATMED_CONFIG_LANGUAGE_SAVE,
    KATMED_CONFIG_WIFI_STATUS, KATMED_CONFIG_WIFI_LIST, KATMED_CONFIG_WIFI_SAVE,
    KATMED_CONFIG_WIFI_DELETE, KATMED_CONFIG_WIFI_RECONNECT
} KatmedConfigOperation;

typedef enum {
    KATMED_CONFIG_MEDICATION_HOURS,
    KATMED_CONFIG_IMAGE_HOURS,
    KATMED_CONFIG_NEXT_DUE_HHMM,
    KATMED_CONFIG_CLOCK_STYLE,
    KATMED_CONFIG_ORIENTATION,
    KATMED_CONFIG_ROTATION, KATMED_CONFIG_BRIGHTNESS, KATMED_CONFIG_NIGHT_ENABLED,
    KATMED_CONFIG_NIGHT_START, KATMED_CONFIG_NIGHT_END, KATMED_CONFIG_COPY_NIGHT
} KatmedConfigField;

typedef enum {
    KATMED_CONFIG_OK,
    KATMED_CONFIG_INVALID,
    KATMED_CONFIG_STALE,
    KATMED_CONFIG_PENDING,
    KATMED_CONFIG_UNAVAILABLE,
    KATMED_CONFIG_COMMIT_UNKNOWN,
    KATMED_CONFIG_TIME_UNAVAILABLE
} KatmedConfigStatus;

/* Numeric, payload-free explanation for the exchange return path. These
 * values are diagnostic only; they do not change KatmedConfigStatus. */
typedef enum {
    KATMED_CONFIG_BRIDGE_DIAG_RESPONSE = 0,
    KATMED_CONFIG_BRIDGE_DIAG_PRECONDITION = 1,
    KATMED_CONFIG_BRIDGE_DIAG_ADMISSION_TIMEOUT = 2,
    KATMED_CONFIG_BRIDGE_DIAG_QUEUED_CANCELLED = 3,
    KATMED_CONFIG_BRIDGE_DIAG_PROCESSING_TIMEOUT = 4
} KatmedConfigBridgeDiagnostic;

typedef enum {
    KATMED_DUE_NONE,
    KATMED_DUE_ACTIVE,
    KATMED_DUE_PAUSED_BOOT
} KatmedDueState;
typedef enum {
    KATMED_RESTORE_NONE,
    KATMED_RESTORE_EXPIRED,
    KATMED_RESTORE_CONFLICT
} KatmedRestoreStatus;

#define KATMED_CONFIG_VALUE_MAX 36u
#define KATMED_CONFIG_REVISION_MAX 96u
#define KATMED_CONFIG_PREVIEW_TOKEN_MAX 96u

/* Bounded copied WiFi payload; no caller pointers or credentials in replies.
 * Count is checked against the actual Store limit by the owner. */
#define KATMED_CONFIG_WIFI_MAX 4u
typedef struct {
    uint32_t id, priority;
    uint8_t credential_action;
    char ssid[33], password[65];
} KatmedConfigWifiUpdate;
typedef struct {
    uint32_t id, priority;
    bool secured;
    char ssid[33];
} KatmedConfigWifiView;

typedef struct {
    KatmedConfigOperation operation;
    uint8_t language;
    KatmedConfigField field;
    char value[KATMED_CONFIG_VALUE_MAX];
    char revision[KATMED_CONFIG_REVISION_MAX];
    char preview_token[KATMED_CONFIG_PREVIEW_TOKEN_MAX];
    union {
    struct {
        uint8_t mode; /* StoreNetworkMode */
        uint8_t group_action; /* 0 keep, 1 new, 2 explicitly join */
        uint8_t group[16], secret[32];
        char ssid[33], password[64], time_source[64];
        char local_datetime[17]; /* Berlin YYYY-MM-DDTHH:MM; unique civil minute. */
    } setup;
    struct { uint32_t id; uint8_t count; KatmedConfigWifiUpdate profiles[KATMED_CONFIG_WIFI_MAX]; } wifi;
    };
} KatmedConfigRequest;

typedef struct {
    char medication_hours[24];
    char image_hours[24];
    char next_due_hhmm[6];
    KatmedDueState next_due_state;
    KatmedRestoreStatus restore_status;
    ClockStyle clock_style;
    Orientation orientation;
    RotationMode rotation;
    char revision[KATMED_CONFIG_REVISION_MAX];
    bool orientation_auto_supported, orientation_automatic;
    KatmedDisplaySettings display;
    KatmedNightCopyStatus night_copy;
} KatmedConfigSnapshot;

typedef struct {
    char time[6];
    LocalDateTime regular_local;
    LocalDateTime effective_local;
    bool night_advanced;
    bool skipped_nonexistent_time;
    char preview_token[KATMED_CONFIG_PREVIEW_TOKEN_MAX];
} KatmedConfigPreview;

typedef struct {
    KatmedConfigStatus status;
    uint8_t language;
    struct { bool ready, configured, recovery; uint8_t limit, count; KatmedConfigWifiView profiles[KATMED_CONFIG_WIFI_MAX]; } wifi;
    KatmedConfigSnapshot snapshot;
    KatmedConfigPreview preview;
    struct { bool configured; uint8_t mode; bool clock_required; bool setup_open; } setup;
    struct {
        char device_id[13], boot_id[21];
        bool alarm_active, boot_notice, last_dose_known;
        uint8_t alarm_severity; /* 0 none,1 blue,2 yellow,3 red */
        int64_t next_due_ms, last_dose_ms; /* zero means unavailable */
        uint32_t emergency_button_sequence;
    } ha;
} KatmedConfigResponse;

typedef void (*KatmedConfigExecute)(void *context, const KatmedConfigRequest *request,
                                    KatmedConfigResponse *response);

/* One bounded request may be in flight. The HTTP task never receives a pointer
 * into Runtime and never calls LVGL, Core, Mesh or Store. */
bool katmed_config_bridge_start(void);
KatmedConfigStatus katmed_config_exchange(const KatmedConfigRequest *request,
                                          KatmedConfigResponse *response,
                                          uint32_t timeout_ms);
KatmedConfigStatus katmed_config_exchange_diagnostic(
    const KatmedConfigRequest *request, KatmedConfigResponse *response,
    uint32_t timeout_ms, KatmedConfigBridgeDiagnostic *diagnostic);
/* Call at most once per application-loop turn. */
void katmed_config_poll(KatmedConfigExecute execute, void *context);

#ifdef __cplusplus
}
#endif
#endif
