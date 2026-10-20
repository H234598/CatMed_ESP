#ifndef KATMED_UI_H
#define KATMED_UI_H
#include "katmed_types.h"
#include "katmed_interaction.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Local presentation states deliberately do not extend the shared core enum. */
#define UI_VIEW_PLACEHOLDER ((UiView)5)
#define UI_VIEW_STATUS ((UiView)6)
#define UI_VIEW_INTERACTION ((UiView)7)
#define KATMED_UI_DEVICE_NAME_MAX 24u
typedef struct { uint8_t row, column; } UiGrid;
UiView ui_grid_view(UiGrid grid);
void ui_grid_move(UiGrid *grid, int horizontal, int vertical);
/* UI state is local and transient, never part of shared snapshots. */
typedef enum {
    UI_REQUEST_CONFIRM,
    UI_REQUEST_DISMISS_BOOT,
    UI_REQUEST_SET_MEDICATION_SECONDS,
    UI_REQUEST_SET_IMAGE_SECONDS,
    UI_REQUEST_SET_NEXT_LOCAL_TIME,
    UI_REQUEST_SET_ROTATION_MODE,
    UI_REQUEST_SET_ORIENTATION,
    UI_REQUEST_SET_CLOCK_STYLE
} UiRequestKind;
typedef struct {
    UiRequestKind kind;
    union {
        uint32_t seconds;
        struct {
            uint8_t hour;
            uint8_t minute;
            ManualTimePreview preview; /* Exact absolute occurrence confirmed. */
        } local_time;
        RotationMode rotation;
        Orientation orientation;
        ClockStyle clock_style;
    } value;
} UiRequest;
typedef enum {
    UI_SUBMIT_COMMITTED, /* Durable; shared changes also coordinator accepted. */
    UI_SUBMIT_PENDING,
    UI_SUBMIT_PENDING_SHARED, /* Awaiting authenticated coordinator result. */
    UI_SUBMIT_REJECTED,
    UI_SUBMIT_UNAVAILABLE,
    UI_SUBMIT_STALE,
    UI_SUBMIT_DISMISSED /* Transient boot notice closed; no durable mutation. */
} UiSubmitResult;
typedef struct {
    EventId id;
    AlarmRoundId round;
    ClockSample occurred_at;
} UiConflict;
/* Fresh interface data only; strings are empty when an address is unavailable. */
typedef struct {
    bool configured;
    bool connected;
    char device_name[KATMED_UI_DEVICE_NAME_MAX];
    bool device_name_assigned;
    bool device_name_uncertain;
    bool mesh_active;
    uint8_t mesh_channel; /* 0: active radio channel unknown. */
    uint16_t mesh_current_count, mesh_history_count;
    uint8_t mesh_quality;
    bool mesh_current_valid, mesh_history_complete;
    char station_ip[16];
    char gateway[16];
    char dns_ip[16];
    char sync_server_ip[16]; /* Last local SNTP sync, not current reachability. */
    char ap_ip[16];
} UiNetworkStatus;
typedef struct {
    void *context;
    bool (*interaction_input)(void *context, const KatmedInteractionInput *input, uint64_t now_ms);
    void (*interaction_status)(void *context, KatmedInteractionStatus *out, uint64_t now_ms);
    UiSubmitResult (*submit)(void *context, const UiRequest *request);
    /* Read the actual committed runtime model without rendering/reloading Store.
     * Called only after durable success, under the caller's existing UI lock. */
    bool (*read_model)(void *context, AppState *state, LocalSettings *local, ClockSample *clock);
    void (*network_status)(void *context, UiNetworkStatus *status);
    bool (*connected)(void *context); /* Optional; absent means unknown/offline. */
    bool (*last_confirmation)(void *context, ClockSample *historical);
    uint8_t (*pending_count)(void *context);
    bool (*storage_ready)(void *context);
    bool (*durable_divergence)(void *context);
    uint8_t (*conflict_count)(void *context);
    bool (*conflict_at)(void *context, size_t index, UiConflict *conflict);
    /* Arm only while the physical-RESET instruction tile is displayed.
     * Confirmation is generated only after the next boot and stays local. */
    bool (*reset_arm)(void *context);
    bool (*reset_cancel)(void *context);
    bool (*reset_prepare)(void *context, char *token, size_t capacity,
                          size_t *length);
    int (*reset_confirm)(void *context, const char *token, size_t length);
} UiServices;

/* A display render cycle caused by a change of the alarm bar's visibility.
 * The cycle may also contain other invalidated UI areas. Duration covers
 * LVGL's RENDER_START through RENDER_READY, including synchronous flush
 * callbacks, but not an asynchronous physical panel transfer. */
typedef struct {
    bool valid;
    uint32_t completed_cycles;
    uint32_t last_blink_changes;
    uint32_t last_flush_areas;
    uint32_t last_flushed_pixels;
    uint64_t last_render_us;
} UiBlinkRenderMetrics;

UiBlinkRenderMetrics ui_blink_render_metrics(void);

/* Only the physical Settings/Status views show this one-time setup code.
 * Main calls these functions under its existing LVGL lock, never from HTTP. */
void ui_set_pairing_code(const char code[7]);
void ui_clear_pairing_code(void);

/* The caller serializes touch samples and renders. An invalid sample (for
 * example an I/O error) cannot release or confirm a gesture. */
/* Called by the board before LVGL evaluates pointer buttons or scrolling. */
bool ui_lvgl_touch_allowed(bool down, int16_t x, int16_t y, bool valid, uint64_t now_ms);
void ui_set_services(const UiServices *services);
/* Called under the LVGL lock after the pending shared request's authenticated
 * coordinator result is known. Only accepted may show shared-save success. */
void ui_shared_result(bool accepted);
void ui_shared_timeout(void);
void ui_touch_sample(bool touched, int16_t x, int16_t y,
                     bool valid, uint64_t monotonic_ms);
bool ui_reset_offer_from_web(void);
void ui_reset_show_warning(void);
void ui_reset_close_remote(void);
UiView ui_current_view(void);
UiView ui_next_view(UiView current, UiGesture gesture, bool alarm_active);
void ui_render(const AppState *state, const LocalSettings *local,
               UiView view, const ClockSample *clock);
#ifdef __cplusplus
}
#endif
#endif
