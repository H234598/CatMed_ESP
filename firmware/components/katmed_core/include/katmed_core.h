#ifndef KATMED_CORE_H
#define KATMED_CORE_H
#include "katmed_types.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The core is a serial, side-effect-free reducer. Call on a candidate AppState;
 * if persistence_required, commit durably before publishing UI success or ACK.
 * Transport authenticates source/authority and provides valid causal parents.
 * It must retain unsynchronised events durably. These IO duties are not hidden
 * inside the reducer. Monotonic time is uint64_t in milliseconds. */
SharedSettings app_default_settings(void);
AppResult app_init(AppState *state, const SharedSettings *settings,
                   const ClockSample *now, EventId boot_identity,
                   uint32_t image_count, const uint8_t catalog_id[KATMED_CATALOG_BYTES]);
AppResult app_apply_event(AppState *state, const AppEvent *event,
                          const ClockSample *now, AppEffects *effects);
AppResult app_tick(AppState *state, const ClockSample *now, AppEffects *effects);
/* Close only the transient boot notice. This never records a medication dose,
 * changes a deadline, or requests persistence or a mesh broadcast. */
AppResult app_dismiss_boot_alarm(AppState *state, const ClockSample *now,
                                 AppEffects *effects);
AppResult app_make_snapshot(const AppState *state, const ClockSample *now,
                            StateSnapshot *snapshot);
AppResult app_calculate_deadline(const ClockSample *start, uint32_t seconds,
                                 Deadline *deadline);
AppResult app_preview_local_time(const ClockSample *now, unsigned hour,
                                 unsigned minute, ManualTimePreview *preview);
AppResult app_berlin_local_time(int64_t utc_ms, LocalDateTime *local);
/* Strict Berlin civil minute; rejects nonexistent and ambiguous DST minutes. */
AppResult app_parse_local_datetime(const char *text, int64_t *utc_ms);
AppResult app_parse_hours(const char *text, uint32_t *seconds);

/* Presentation only: a complete cycle fills and then empties top-to-bottom.
 * 0..4999 is filling, 5000..9999 is emptying. No deadline or persisted state
 * is changed. `now` must belong to state->clock's local monotonic domain.
 * A foreign boot start without comparable UTC returns APP_TIME_UNAVAILABLE;
 * the caller must keep the alarm active and use a conservative visual fallback.
 */
typedef enum {
    ALARM_COLOR_BLUE,
    ALARM_COLOR_YELLOW,
    ALARM_COLOR_RED
} AlarmColor;
typedef struct {
    AlarmColor color;
    uint16_t phase_permyriad;
    uint32_t frequency_millihz; /* Rounded display value, not phase source. */
    uint64_t elapsed_ms;
} AlarmAnimation;
AppResult app_alarm_animation(const AppState *state, const ClockSample *now,
                              AlarmAnimation *out);

#ifdef __cplusplus
}
#endif
#endif
