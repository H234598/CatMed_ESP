#ifndef KATMED_TYPES_H
#define KATMED_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared in-memory contracts, not a wire or NVS layout. Codecs must validate
 * versions, lengths, enums and integer bounds field by field. No pointers or
 * local presentation settings may be sent as a StateSnapshot. */
#define KATMED_SCHEMA_VERSION 1u
#define KATMED_RECENT_EVENTS 16u
#define KATMED_CATALOG_BYTES 32u
#define KATMED_DEFAULT_MEDICATION_SECONDS 43200u
#define KATMED_DEFAULT_IMAGE_SECONDS 21600u
#define KATMED_MAX_INTERVAL_SECONDS UINT32_MAX

typedef enum { CLOCK_UNAVAILABLE, CLOCK_HOLDOVER, CLOCK_SYNCHRONIZED } ClockQuality;
typedef struct {
    uint64_t monotonic_ms;
    uint64_t domain; /* Unique boot/time domain; never compare unrelated clocks. */
    int64_t utc_ms;
    bool utc_valid;
    ClockQuality quality;
} ClockSample;

typedef struct {
    uint64_t device;
    uint64_t epoch; /* Persisted epoch or fresh unique boot identity. */
    uint64_t sequence; /* Event producer guarantees uniqueness across reboot. */
} EventId;
typedef struct { EventId origin; uint64_t ordinal; } AlarmRoundId;
typedef struct {
    uint64_t number;
    uint64_t coordinator; /* Equal numbers from different owners are conflicts. */
} StateRevision;

typedef enum { ROTATION_OFF, ROTATION_NAME_ORDER, ROTATION_RANDOM } RotationMode;
typedef struct {
    uint32_t medication_seconds;
    uint32_t image_seconds;
    RotationMode rotation;
    StateRevision revision;
} SharedSettings;
typedef enum { DIGITAL_LARGE, DIGITAL_DATE, ANALOG } ClockStyle;
typedef enum { ORIENTATION_0, ORIENTATION_90, ORIENTATION_180, ORIENTATION_270 } Orientation;
typedef struct {
    Orientation orientation;
    ClockStyle clock_style;
    uint64_t device;
} LocalSettings;
typedef enum { BACKGROUND, CLOCK, SETTINGS, ALARM, MORSE } UiView;
typedef enum { NONE, SWIPE_LEFT, SWIPE_RIGHT } UiGesture;

typedef enum { START_BOOT, START_CONFIRMATION, START_SETTINGS, START_MANUAL, START_SNAPSHOT } StartReason;
typedef struct { bool valid; int64_t ms; } UtcInstant;
typedef struct {
    uint64_t regular_monotonic_ms;
    uint64_t effective_monotonic_ms;
    UtcInstant regular_utc;
    UtcInstant effective_utc;
    bool night_advanced;
} Deadline;

typedef struct {
    uint32_t schema_version;
    SharedSettings settings;
    StateRevision revision;
    AlarmRoundId round;
    bool alarm_active;
    StartReason start_reason;
    ClockSample clock;
    ClockSample countdown_started;
    EventId countdown_origin; /* Changes only for an intentional medication restart. */
    bool has_time_sync;
    ClockSample last_time_sync;
    EventId last_time_sync_id;
    Deadline deadline;
    bool has_last_confirmation;
    EventId last_confirmation_id;
    AlarmRoundId last_confirmed_round;
    ClockSample last_confirmation;
    ClockSample last_settings_change;
    uint32_t image_id;
    uint32_t image_count;
    uint64_t image_remaining_ms;
    bool image_due_pending;
    bool confirmation_pending_sync;
    bool divergent;
    bool catalog_mismatch;
    uint8_t catalog_id[KATMED_CATALOG_BYTES];
    EventId recent_events[KATMED_RECENT_EVENTS];
    uint8_t recent_event_count;
    uint8_t recent_event_next;
} AppState;

typedef struct {
    uint32_t schema_version;
    uint8_t catalog_id[KATMED_CATALOG_BYTES];
    AppState state;
    ClockSample captured_at;
    /* Remaining times are measured at captured_at. Adoption requires a known
     * UTC age or an explicitly validated monotonic-domain relationship. */
    uint64_t medication_regular_remaining_ms;
    uint64_t medication_effective_remaining_ms;
    uint64_t image_remaining_ms;
} StateSnapshot;

typedef enum {
    EVENT_CONFIRM,
    EVENT_SET_MEDICATION_INTERVAL,
    EVENT_SET_IMAGE_INTERVAL,
    EVENT_SET_ROTATION_MODE,
    EVENT_SET_NEXT_LOCAL_TIME,
    EVENT_TIME_SYNC,
    EVENT_ADOPT_SNAPSHOT,
    EVENT_SET_IMAGE
} AppEventKind;
typedef struct {
    AppEventKind kind;
    EventId id;
    uint64_t sender;
    AlarmRoundId round;
    StateRevision parent_revision;
    ClockSample occurred_at;
    bool coordinator_accepted; /* Set by authenticated transport, not UI. */
    union {
        uint32_t interval_seconds;
        RotationMode rotation;
        /* The UI must explicitly confirm the previewed absolute occurrence. */
        struct {
            uint8_t hour;
            uint8_t minute;
            UtcInstant selected_utc;
            UtcInstant selected_effective_utc;
        } local_time;
        ClockSample time_sync;
        const StateSnapshot *snapshot; /* Borrowed only for app_apply_event. */
        uint32_t image_id;
    } value;
} AppEvent;

typedef enum {
    APP_OK,
    APP_NO_CHANGE,
    APP_DUPLICATE,
    APP_STALE,
    APP_CONFLICT,
    APP_INVALID,
    APP_TIME_UNAVAILABLE,
    APP_NOT_AUTHORIZED,
    APP_OVERFLOW
} AppResult;
typedef struct {
    bool state_changed;
    bool alarm_started;
    bool alarm_ended;
    bool image_changed;
    bool persistence_required;
    bool broadcast_required;
} AppEffects;
typedef struct {
    int year;
    unsigned month, day, hour, minute, second;
    unsigned millisecond;
    int utc_offset_seconds;
    bool daylight_saving;
} LocalDateTime;
typedef struct {
    UtcInstant regular_utc;
    UtcInstant effective_utc;
    LocalDateTime regular_local;
    LocalDateTime effective_local;
    bool night_advanced;
    bool skipped_nonexistent_time;
} ManualTimePreview;

#ifdef __cplusplus
}
#endif
#endif
