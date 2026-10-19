#ifndef ESP_PLATFORM
#define _POSIX_C_SOURCE 200809L
#endif
#include "katmed_ui.h"
#include "clock_view.h"
#include "katmed_core.h"
#include "katmed_font.h"
#include "katmed_images.h"
#include "katmed_morse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <time.h>
#endif

typedef enum { EDIT_NONE, EDIT_MEDICATION, EDIT_IMAGE, EDIT_NEXT_TIME } EditKind;
#define ALARM_STRIPS 6u
typedef struct {
    UiServices services;
    UiView view;
    UiGrid grid;
    lv_obj_t *page, *interaction_title, *send_button, *rx_layer, *notice;
    KatmedInteractionStatus interaction;
    uint64_t menu_entry, contact_sequence, active_contact;
    uint64_t overlay_key, overlay_generation, overlay_since_ms;
    uint64_t rx_grant, rx_impulse, rx_until, rx_gap_until;
    uint64_t rx_authority, rx_generation;
    bool send_contact, overlay_contact, edge_contact;
    bool lv_contact, lv_blocked;
    uint64_t lv_overlay_generation;
    int16_t lv_start_x, lv_start_y;
    bool interaction_start_contact;
    bool interaction_alarm_paused;
    uint64_t interaction_activation_ms;

    AppState state;
    LocalSettings local;
    ClockSample clock;
    bool have_state;
    bool clock_initialized;
    bool status_from_home;
    bool touch_armed;
    bool touch_down;
    bool reject_contact;
    bool reject_confirm_motion;
    int16_t start_x, start_y;
    int16_t last_x, last_y;
    uint64_t down_ms;
    lv_obj_t *screen, *background, *fallback;
    lv_obj_t *clock_view, *alarm_bar, *alarm_strips[ALARM_STRIPS], *alarm_text;
    lv_obj_t *settings, *status, *keypad, *feedback, *morse_layer;
    lv_obj_t *entry_label, *preview_label;
    lv_obj_t *style_dialog, *style_title, *style_options[3];
    uint32_t image_id;
    Orientation image_orientation;
    int16_t width, height;
    Orientation screen_orientation;
    EditKind edit;
    ClockStyle pending_style;
    char entry[24];
    bool preview_valid;
    bool shared_pending;
    int8_t shared_result_status; /* 0 unknown, 1 accepted, -1 rejected. */
    ManualTimePreview preview;
    ClockStyle shown_style;
    int64_t shown_clock_tick;
    ClockQuality shown_quality;
    uint64_t status_second;
    uint64_t feedback_until_ms, interaction_loss_until_ms;
    bool interaction_overflow_shown;
    bool confirmed_round;
    AlarmRoundId confirmed_id;
    bool alarm_color_shown;
    AlarmColor shown_alarm_color;
    KatmedMorse morse;
    uint64_t morse_activation_ms;
    bool morse_start_contact;
    bool morse_alarm_paused;
    bool morse_alarm_confirmed;
    bool morse_color_valid;
    bool morse_shown_white;
} UiContext;

static UiContext ui = {.view = BACKGROUND, .grid = {1, 1}, .shown_clock_tick = INT64_MIN};
typedef struct {
    lv_display_t *display;
    UiBlinkRenderMetrics snapshot;
    uint32_t pending_changes;
    uint32_t flush_areas;
    uint32_t flushed_pixels;
    uint64_t render_started_us;
    bool rendering;
} BlinkMetricsState;
static BlinkMetricsState blink_metrics;

static uint64_t metrics_now_us(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

static void display_metrics_event(lv_event_t *event)
{
    if (lv_event_get_target(event) != blink_metrics.display) return;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_RENDER_START:
        if (blink_metrics.pending_changes == 0) break;
        blink_metrics.rendering = true;
        blink_metrics.render_started_us = metrics_now_us();
        blink_metrics.flush_areas = 0;
        blink_metrics.flushed_pixels = 0;
        break;
    case LV_EVENT_FLUSH_START:
        if (blink_metrics.rendering) {
            const lv_area_t *area = lv_event_get_param(event);
            if (area && blink_metrics.flush_areas < UINT32_MAX)
                blink_metrics.flush_areas++;
            if (area) {
                int32_t width = lv_area_get_width(area);
                int32_t height = lv_area_get_height(area);
                if (width > 0 && height > 0) {
                    uint64_t pixels = (uint64_t)width * (uint32_t)height +
                                      blink_metrics.flushed_pixels;
                    blink_metrics.flushed_pixels = pixels > UINT32_MAX ?
                                                   UINT32_MAX : (uint32_t)pixels;
                }
            }
        }
        break;
    case LV_EVENT_RENDER_READY:
        if (blink_metrics.rendering) {
            uint64_t finished_us = metrics_now_us();
            blink_metrics.snapshot.valid = true;
            if (blink_metrics.snapshot.completed_cycles < UINT32_MAX)
                blink_metrics.snapshot.completed_cycles++;
            blink_metrics.snapshot.last_blink_changes = blink_metrics.pending_changes;
            blink_metrics.snapshot.last_flush_areas = blink_metrics.flush_areas;
            blink_metrics.snapshot.last_flushed_pixels = blink_metrics.flushed_pixels;
            blink_metrics.snapshot.last_render_us =
                finished_us >= blink_metrics.render_started_us ?
                finished_us - blink_metrics.render_started_us : 0;
            blink_metrics.pending_changes = 0;
            blink_metrics.rendering = false;
        }
        break;
    case LV_EVENT_REFR_READY:
        /* A visibility change on an inactive screen need not draw anything. */
        if (!blink_metrics.rendering) blink_metrics.pending_changes = 0;
        break;
    case LV_EVENT_DELETE:
        blink_metrics = (BlinkMetricsState){0};
        break;
    default:
        break;
    }
}

static void watch_display(lv_display_t *display)
{
    if (blink_metrics.display == display) return;
    if (blink_metrics.display)
        lv_display_remove_event_cb_with_user_data(blink_metrics.display,
                                                   display_metrics_event, NULL);
    blink_metrics = (BlinkMetricsState){.display = display};
    lv_display_add_event_cb(display, display_metrics_event, LV_EVENT_ALL, NULL);
}

UiBlinkRenderMetrics ui_blink_render_metrics(void)
{
    return blink_metrics.snapshot;
}
static void interaction_open(lv_event_t *event);
static void interaction_cancel(uint64_t now);
static void update_interaction(uint64_t now);
static void change_page(int horizontal, int vertical, uint64_t now);
static void update_status(void);
static void build_settings(void);
static void close_status(lv_event_t *event);

static void destroy_menu(void)
{
    if (ui.settings) lv_obj_delete(ui.settings);
    ui.settings = NULL;
}

static void destroy_settings(void)
{
    if (ui.status) lv_obj_delete(ui.status);
    ui.status = NULL;
    ui.status_from_home = false;
    destroy_menu();
}

static bool same_round(AlarmRoundId a, AlarmRoundId b)
{
    return a.ordinal == b.ordinal && a.origin.device == b.origin.device &&
           a.origin.epoch == b.origin.epoch && a.origin.sequence == b.origin.sequence;
}

static void show_feedback(const char *text)
{
    if (!ui.feedback) return;
    lv_label_set_text(ui.feedback, text);
    lv_obj_remove_flag(ui.feedback, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui.feedback);
    ui.feedback_until_ms = ui.clock.monotonic_ms + 3000u;
}

static const char *result_text(UiSubmitResult result)
{
    switch (result) {
    case UI_SUBMIT_COMMITTED: return "Gespeichert";
    case UI_SUBMIT_PENDING: return "Lokal gespeichert; Gruppe ausstehend";
    case UI_SUBMIT_PENDING_SHARED: return "Koordinator prüft Änderung";
    case UI_SUBMIT_REJECTED: return "Änderung abgelehnt";
    case UI_SUBMIT_STALE: return "Vorschau veraltet; erneut prüfen";
    case UI_SUBMIT_UNAVAILABLE: return "Speichern derzeit nicht verfügbar";
    }
    return "Speichern fehlgeschlagen";
}

static UiSubmitResult submit(const UiRequest *request)
{
    if (!ui.services.submit) return UI_SUBMIT_UNAVAILABLE;
    return ui.services.submit(ui.services.context, request);
}

static UiSubmitResult request_action(const UiRequest *request)
{
    UiSubmitResult result = submit(request);
    /* A locally committed confirmation may still await group adoption. Do not
     * allow another release of the same displayed round to submit it again. */
    if (request->kind == UI_REQUEST_CONFIRM &&
        (result == UI_SUBMIT_COMMITTED || result == UI_SUBMIT_PENDING)) {
        ui.confirmed_round = true;
        ui.confirmed_id = ui.state.round;
        if (ui.morse_alarm_paused) ui.morse_alarm_confirmed = true;
    }
    show_feedback(result_text(result));
    if (result == UI_SUBMIT_PENDING_SHARED) {
        ui.shared_pending = true;
        ui.shared_result_status = 0;
    }
    if (result == UI_SUBMIT_COMMITTED && ui.keypad) {
        lv_obj_delete(ui.keypad);
        ui.keypad = NULL;
        ui.entry_label = NULL;
        ui.preview_label = NULL;
        ui.edit = EDIT_NONE;
        if (ui.view == SETTINGS) build_settings();
        if (ui.feedback) lv_obj_move_foreground(ui.feedback);
    }
    if (result == UI_SUBMIT_STALE) ui.preview_valid = false;
    return result;
}

void ui_shared_result(bool accepted)
{
    if (!ui.shared_pending) return;
    ui.shared_pending = false;
    ui.shared_result_status = accepted ? 1 : -1;
    if (accepted) {
        if (ui.view != ALARM && !ui.state.alarm_active)
            show_feedback("Gemeinsame Änderung gespeichert");
        if (ui.keypad) {
            lv_obj_delete(ui.keypad);
            ui.keypad = NULL;
            ui.entry_label = NULL;
            ui.preview_label = NULL;
            ui.edit = EDIT_NONE;
            if (ui.view == SETTINGS) build_settings();
        }
    } else {
        if (ui.view != ALARM && !ui.state.alarm_active)
            show_feedback("Koordinator hat Änderung nicht übernommen");
    }
    if (ui.status) update_status();
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int width)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, &katmed_font_14, 0);
    lv_obj_set_style_text_color(obj, lv_color_white(), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_label_set_text(obj, text);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int w, int h,
                        lv_event_cb_t cb, void *data)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x22384a), 0);
    lv_obj_set_style_radius(obj, 8, 0);
    if (cb) lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, data);
    lv_obj_t *caption = label(obj, text, w - 8);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(caption);
    return obj;
}

static void format_local(char *out, size_t size, UtcInstant instant)
{
    LocalDateTime t;
    if (!instant.valid || app_berlin_local_time(instant.ms, &t) != APP_OK) {
        snprintf(out, size, "Zeit nicht verfügbar");
        return;
    }
    snprintf(out, size, "%02u.%02u.%04d %02u:%02u %s", t.day, t.month,
             t.year, t.hour, t.minute, t.daylight_saving ? "CEST" : "CET");
}

static void format_hours(char *out, size_t size, uint32_t seconds)
{
    /* Six decimal places round-trip every whole second through app_parse_hours.
     * Integer arithmetic also keeps the firmware independent of float printf. */
    const unsigned long hours = seconds / 3600u;
    const unsigned long fraction =
        ((uint64_t)(seconds % 3600u) * 1000000u + 1800u) / 3600u;
    if (!fraction) snprintf(out, size, "%lu", hours);
    else {
        snprintf(out, size, "%lu.%06lu", hours, fraction);
        size_t length = strlen(out);
        while (length && out[length - 1] == '0') out[--length] = '\0';
    }
}

static bool parse_hhmm(const char *text, unsigned *hour, unsigned *minute)
{
    if (strlen(text) != 5 || text[2] != ':') return false;
    for (int i = 0; i < 5; ++i)
        if (i != 2 && (text[i] < '0' || text[i] > '9')) return false;
    *hour = (unsigned)(text[0] - '0') * 10u + (unsigned)(text[1] - '0');
    *minute = (unsigned)(text[3] - '0') * 10u + (unsigned)(text[4] - '0');
    return *hour < 24 && *minute < 60;
}

static void update_preview(void)
{
    if (!ui.preview_label) return;
    ui.preview_valid = false;
    char text[96];
    if (ui.edit == EDIT_NEXT_TIME) {
        unsigned hour, minute;
        if (!parse_hhmm(ui.entry, &hour, &minute)) {
            lv_label_set_text(ui.preview_label, "HH:MM eingeben");
            return;
        }
        if (app_preview_local_time(&ui.clock, hour, minute, &ui.preview) != APP_OK) {
            lv_label_set_text(ui.preview_label, "Uhrzeit nicht verfügbar");
            return;
        }
        ui.preview_valid = true;
        format_local(text, sizeof(text), ui.preview.effective_utc);
        if (ui.preview.skipped_nonexistent_time) {
            LocalDateTime *regular = &ui.preview.regular_local;
            LocalDateTime *effective = &ui.preview.effective_local;
            lv_label_set_text_fmt(ui.preview_label,
                                  "%02u:%02u heute nicht vorhanden\n"
                                  "Nächster %02u.%02u.%04d %02u:%02u\n"
                                  "Alarm %02u.%02u.%04d %02u:%02u\n%s%s",
                                  hour, minute, regular->day, regular->month,
                                  regular->year, regular->hour, regular->minute,
                                  effective->day, effective->month, effective->year,
                                  effective->hour, effective->minute,
                                  effective->daylight_saving ? "CEST" : "CET",
                                  ui.preview.night_advanced ? " (vorgezogen)" : "");
        } else
            lv_label_set_text_fmt(ui.preview_label, "Wirksam: %s%s", text,
                                  ui.preview.night_advanced ? " (Nachtvorziehung)" : "");
        return;
    }
    uint32_t seconds = 0;
    if (app_parse_hours(ui.entry, &seconds) != APP_OK) {
        lv_label_set_text(ui.preview_label, "Positive Stundenzahl eingeben");
        return;
    }
    ui.preview_valid = true;
    if (ui.edit == EDIT_MEDICATION) {
        Deadline deadline;
        if (app_calculate_deadline(&ui.clock, seconds, &deadline) != APP_OK) {
            lv_label_set_text(ui.preview_label, "Termin nicht berechenbar");
            ui.preview_valid = false;
            return;
        }
        format_local(text, sizeof(text), deadline.effective_utc);
        lv_label_set_text_fmt(ui.preview_label, "Wirksam: %s%s", text,
                              deadline.night_advanced ? " (Nachtvorziehung)" : "");
    } else {
        lv_label_set_text_fmt(ui.preview_label, "Bildwechsel: %lu Sekunden",
                              (unsigned long)seconds);
    }
}

static void refresh_entry(void)
{
    if (ui.entry_label) lv_label_set_text(ui.entry_label,
                                           ui.entry[0] ? ui.entry : "_");
    update_preview();
}

static void key_press(lv_event_t *event)
{
    lv_obj_t *keys = lv_event_get_target(event);
    uint32_t selected = lv_buttonmatrix_get_selected_button(keys);
    const char *key = lv_buttonmatrix_get_button_text(keys, selected);
    if (!key) return;
    size_t length = strlen(ui.entry);
    if (strcmp(key, "<") == 0) {
        if (length) ui.entry[length - 1] = '\0';
    } else if (length + strlen(key) < sizeof(ui.entry)) {
        if (ui.edit == EDIT_NEXT_TIME && strcmp(key, ":") != 0 &&
            (key[0] < '0' || key[0] > '9')) return;
        if (ui.edit != EDIT_NEXT_TIME && strcmp(key, ":") == 0) return;
        strcat(ui.entry, key);
    }
    refresh_entry();
}

static void cancel_edit(lv_event_t *event)
{
    (void)event;
    if (ui.keypad) lv_obj_delete(ui.keypad);
    ui.keypad = NULL;
    ui.entry_label = NULL;
    ui.preview_label = NULL;
    ui.edit = EDIT_NONE;
    if (ui.view == SETTINGS) build_settings();
}

static void save_edit(lv_event_t *event)
{
    (void)event;
    if (!ui.preview_valid) {
        show_feedback("Eingabe ungültig; bitte prüfen");
        return;
    }
    UiRequest request = {0};
    if (ui.edit == EDIT_NEXT_TIME) {
        unsigned hour, minute;
        if (!parse_hhmm(ui.entry, &hour, &minute)) return;
        /* The coordinator must reject an expired absolute preview. */
        request.kind = UI_REQUEST_SET_NEXT_LOCAL_TIME;
        request.value.local_time.hour = (uint8_t)hour;
        request.value.local_time.minute = (uint8_t)minute;
        request.value.local_time.preview = ui.preview;
    } else {
        uint32_t seconds;
        if (app_parse_hours(ui.entry, &seconds) != APP_OK) return;
        request.kind = ui.edit == EDIT_MEDICATION ? UI_REQUEST_SET_MEDICATION_SECONDS :
                                                     UI_REQUEST_SET_IMAGE_SECONDS;
        request.value.seconds = seconds;
    }
    request_action(&request);
}

static void open_edit(EditKind edit)
{
    if (!ui.screen || ui.keypad) return;
    destroy_settings();
    ui.edit = edit;
    ui.entry[0] = '\0';
    if (edit == EDIT_IMAGE)
        format_hours(ui.entry, sizeof(ui.entry), ui.state.settings.image_seconds);
    else if (edit == EDIT_NEXT_TIME) {
        LocalDateTime current;
        if (ui.state.deadline.effective_utc.valid &&
            app_berlin_local_time(ui.state.deadline.effective_utc.ms, &current) == APP_OK)
            snprintf(ui.entry, sizeof(ui.entry), "%02u:%02u", current.hour, current.minute);
    }
    ui.preview_valid = false;
    bool landscape = ui.width > ui.height;
    ui.keypad = lv_obj_create(ui.screen);
    lv_obj_set_size(ui.keypad, ui.width, ui.height);
    lv_obj_set_pos(ui.keypad, 0, 0);
    lv_obj_set_style_bg_color(ui.keypad, lv_color_hex(0x101c27), 0);
    lv_obj_set_style_bg_opa(ui.keypad, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.keypad, 0, 0);
    lv_obj_set_style_pad_all(ui.keypad, 0, 0);
    lv_obj_remove_flag(ui.keypad, LV_OBJ_FLAG_SCROLLABLE);

    bool round = ui.width == 240 && ui.height == 240;
    int keys_w = landscape ? ui.width / 2 : round ? 180 : ui.width - 32;
    int x0 = landscape ? 8 : round ? 30 : 16;
    int y0 = landscape ? 8 : round ? 110 : 125;
    int key_h = landscape ? (ui.height - 16) / 4 - 2 :
                round ? 17 : (ui.height - 170) / 4 - 2;
    if (key_h < 17) key_h = 17;
    static const char *key_map[] = {"1", "2", "3", "<", "\n",
                                    "4", "5", "6", ".", "\n",
                                    "7", "8", "9", ",", "\n",
                                    "0", "0", ":", "<", ""};
    lv_obj_t *keys = lv_buttonmatrix_create(ui.keypad);
    lv_buttonmatrix_set_map(keys, key_map);
    lv_obj_set_size(keys, keys_w, 4 * (key_h + 2));
    lv_obj_set_pos(keys, x0, y0);
    lv_obj_set_style_pad_all(keys, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(keys, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_column(keys, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(keys, lv_color_hex(0x101c27), LV_PART_MAIN);
    lv_obj_set_style_bg_color(keys, lv_color_hex(0x22384a), LV_PART_ITEMS);
    lv_obj_set_style_text_color(keys, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keys, &katmed_font_14, LV_PART_ITEMS);
    lv_obj_add_event_cb(keys, key_press, LV_EVENT_VALUE_CHANGED, NULL);
    int info_x = landscape ? keys_w + 14 : round ? 50 : 20;
    int info_w = landscape ? ui.width - info_x - 8 :
                 round ? 140 : ui.width - 40;
    lv_obj_t *heading = label(ui.keypad, round ?
          (edit == EDIT_NEXT_TIME ? "HH:MM" : edit == EDIT_MEDICATION ? "Medikamente" : "Bilder") :
          edit == EDIT_NEXT_TIME ? "Nächste Zeit HH:MM" :
          edit == EDIT_MEDICATION ? "Medikamente: Stunden" : "Bilder: Stunden",
          info_w);
    lv_obj_set_pos(heading, info_x, landscape ? 8 : round ? 36 : 8);
    if (round) lv_obj_set_width(heading, 76);
    ui.entry_label = label(ui.keypad, "_", info_w);
    if (round) {
        lv_obj_set_width(ui.entry_label, 64);
        /* Keep long configured hour values on one readable line, above preview. */
        lv_label_set_long_mode(ui.entry_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    lv_obj_set_pos(ui.entry_label, round ? info_x + 76 : info_x,
                   landscape ? 38 : round ? 36 : 27);
    ui.preview_label = label(ui.keypad, "", info_w);
    lv_obj_set_style_text_font(ui.preview_label, &katmed_font_10, 0);
    lv_obj_set_pos(ui.preview_label, info_x, landscape ? 62 : round ? 56 : 47);
    int action_y = landscape ? ui.height - 40 : round ? 188 : ui.height - 35;
    int action_w = landscape || round ? (info_w - 4) / 2 : (ui.width - 44) / 2;
    lv_obj_t *save = button(ui.keypad, "Speichern", action_w, 30, save_edit, NULL);
    lv_obj_set_pos(save, info_x, action_y);
    lv_obj_t *cancel = button(ui.keypad, "Abbrechen", action_w, 30, cancel_edit, NULL);
    lv_obj_set_pos(cancel, info_x + action_w + 4, action_y);
    lv_obj_set_width(lv_obj_get_child(save, 0), action_w - 2);
    lv_obj_set_width(lv_obj_get_child(cancel, 0), action_w - 2);
    lv_obj_set_style_text_font(lv_obj_get_child(save, 0), &katmed_font_12, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(cancel, 0), &katmed_font_12, 0);
    refresh_entry();
}

static void edit_button(lv_event_t *event)
{
    open_edit((EditKind)(intptr_t)lv_event_get_user_data(event));
}

static void option_selected(lv_event_t *event)
{
    lv_obj_t *matrix = lv_event_get_target(event);
    uint32_t selected = lv_buttonmatrix_get_selected_button(matrix);
    bool orientation = (intptr_t)lv_event_get_user_data(event) != 0;
    UiRequest request = {0};
    if (orientation) {
        if (selected > ORIENTATION_270) return;
        request.kind = UI_REQUEST_SET_ORIENTATION;
        request.value.orientation = (Orientation)selected;
    } else {
        if (selected > ROTATION_RANDOM) return;
        request.kind = UI_REQUEST_SET_ROTATION_MODE;
        request.value.rotation = (RotationMode)selected;
    }
    request_action(&request);
}

static lv_obj_t *option_matrix(lv_obj_t *parent, const char *const map[],
                               int width, int rows, bool orientation)
{
    lv_obj_t *matrix = lv_buttonmatrix_create(parent);
    lv_buttonmatrix_set_map(matrix, map);
    lv_obj_set_size(matrix, width, rows * 32 + (rows - 1) * 5);
    lv_obj_set_style_pad_all(matrix, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(matrix, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(matrix, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(matrix, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(matrix, lv_color_hex(0x22384a), LV_PART_ITEMS);
    lv_obj_set_style_text_color(matrix, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(matrix, &katmed_font_14, LV_PART_ITEMS);
    for (int i = 0; i < rows; ++i)
        lv_buttonmatrix_set_button_ctrl(matrix, i,
            LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    lv_obj_add_event_cb(matrix, option_selected, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)orientation);
    return matrix;
}

static void style_highlight(void)
{
    for (int i = 0; i < 3; ++i) {
        if (!ui.style_options[i]) continue;
        lv_obj_set_style_bg_color(ui.style_options[i],
            i == (int)ui.pending_style ? lv_color_hex(0x286e86) : lv_color_hex(0x22384a), 0);
    }
}

static void style_select(lv_event_t *event)
{
    ui.pending_style = (ClockStyle)(intptr_t)lv_event_get_user_data(event);
    style_highlight();
}

static void style_cancel(lv_event_t *event)
{
    (void)event;
    if (ui.style_dialog) lv_obj_delete(ui.style_dialog);
    ui.style_dialog = NULL;
    ui.style_title = NULL;
    for (int i = 0; i < 3; ++i) ui.style_options[i] = NULL;
    if (ui.view == SETTINGS) build_settings();
}

static void style_save(lv_event_t *event)
{
    (void)event;
    UiRequest request = {.kind = UI_REQUEST_SET_CLOCK_STYLE};
    request.value.clock_style = ui.pending_style;
    UiSubmitResult result = request_action(&request);
    if (result == UI_SUBMIT_COMMITTED) style_cancel(NULL);
    else if (ui.style_title) lv_label_set_text(ui.style_title, result_text(result));
}

static void style_open(lv_event_t *event)
{
    (void)event;
    if (!ui.screen || ui.style_dialog) return;
    destroy_settings();
    ui.pending_style = ui.local.clock_style;
    if (ui.pending_style > ANALOG) ui.pending_style = DIGITAL_LARGE;
    bool round = ui.width == 240 && ui.height == 240;
    int width = round ? 170 : ui.width - 20;
    if (width > 240) width = 240;
    int height = round ? 170 : ui.height - 20;
    if (height > 200) height = 200;
    ui.style_dialog = lv_obj_create(ui.screen);
    lv_obj_set_size(ui.style_dialog, width, height);
    lv_obj_center(ui.style_dialog);
    lv_obj_set_style_bg_color(ui.style_dialog, lv_color_hex(0x101c27), 0);
    lv_obj_set_style_bg_opa(ui.style_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.style_dialog, 0, 0);
    lv_obj_set_style_pad_all(ui.style_dialog, 0, 0);
    lv_obj_remove_flag(ui.style_dialog, LV_OBJ_FLAG_SCROLLABLE);
    ui.style_title = label(ui.style_dialog, "Uhrdesign wählen", width - 12);
    lv_obj_set_pos(ui.style_title, 6, 4);
    static const char *names[3] = {"Digital groß", "Digital mit Datum", "Analog"};
    int option_h = height < 170 ? 25 : 30;
    int first_y = height < 170 ? 30 : 36;
    for (int i = 0; i < 3; ++i) {
        ui.style_options[i] = button(ui.style_dialog, names[i], width - 12,
                                     option_h, style_select, (void *)(intptr_t)i);
        lv_obj_set_pos(ui.style_options[i], 6, first_y + i * (option_h + 3));
    }
    style_highlight();
    int action_w = (width - 18) / 2;
    lv_obj_t *save = button(ui.style_dialog, "Speichern", action_w, 29,
                            style_save, NULL);
    lv_obj_t *cancel = button(ui.style_dialog, "Abbrechen", action_w, 29,
                              style_cancel, NULL);
    lv_obj_set_pos(save, 6, height - 35);
    lv_obj_set_pos(cancel, 12 + action_w, height - 35);
    lv_obj_set_width(lv_obj_get_child(save, 0), action_w - 2);
    lv_obj_set_width(lv_obj_get_child(cancel, 0), action_w - 2);
    lv_obj_set_style_text_font(lv_obj_get_child(save, 0), &katmed_font_12, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(cancel, 0), &katmed_font_12, 0);
}

static void close_settings(lv_event_t *event)
{
    (void)event;
    ui.view = BACKGROUND;
    ui.grid = (UiGrid){1, 1};
    destroy_settings();
}

static void morse_open(lv_event_t *event)
{
    (void)event;
    if (!ui.screen || ui.view != SETTINGS) return;
#ifdef ESP_PLATFORM
    ui.morse_activation_ms = (uint64_t)esp_timer_get_time() / 1000u;
#else
    ui.morse_activation_ms = ui.clock.monotonic_ms;
#endif
    katmed_morse_start(&ui.morse, ui.morse_activation_ms);
    if (!ui.morse.active) return;
    ui.morse_start_contact = ui.touch_down;
    ui.view = MORSE;
    if (ui.settings) lv_obj_add_flag(ui.settings, LV_OBJ_FLAG_HIDDEN);
    if (ui.morse_layer) {
        lv_obj_set_style_bg_color(ui.morse_layer, lv_color_white(), 0);
        ui.morse_shown_white = true;
        ui.morse_color_valid = true;
        lv_obj_remove_flag(ui.morse_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui.morse_layer);
    }
}

static void status_button(lv_event_t *event)
{
    (void)event;
    if (ui.feedback) lv_obj_add_flag(ui.feedback, LV_OBJ_FLAG_HIDDEN);
    ui.grid.row = 2;
    ui.view = UI_VIEW_STATUS;
    destroy_menu();
    ui.status = lv_obj_create(ui.screen);
    lv_obj_set_size(ui.status, ui.width, ui.height);
    lv_obj_set_pos(ui.status, 0, 0);
    lv_obj_set_style_bg_color(ui.status, lv_color_hex(0x12212c), 0);
    lv_obj_set_style_bg_opa(ui.status, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(ui.status, ui.width == 240 ? 50 :
                            ui.height < 200 ? 6 : 10, 0);
    lv_obj_set_flex_flow(ui.status, LV_FLEX_FLOW_COLUMN);
    if (ui.height < 200) lv_obj_set_style_pad_row(ui.status, 2, 0);
    int row_w = ui.width - (ui.width == 240 ? 100 : 18) - 4;
    lv_obj_t *title = label(ui.status, "Status", row_w);
    if (ui.height < 200)
        lv_obj_set_style_text_font(title, &katmed_font_12, 0);
    button(ui.status, "Zurück", row_w, ui.height < 200 ? 26 : 34,
           close_status, NULL);
    update_status();
}

static void close_status(lv_event_t *event)
{
    (void)event;
    if (ui.status) lv_obj_delete(ui.status);
    ui.status = NULL;
    ui.status_from_home = false;
    ui.grid.row = 1;
    ui.view = ui_grid_view(ui.grid);
    if (ui.view == SETTINGS) build_settings();
}

static void build_settings(void)
{
    if (ui.settings) return;
    ui.settings = lv_obj_create(ui.screen);
    lv_obj_remove_flag(ui.settings, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_size(ui.settings, ui.width, ui.height);
    lv_obj_set_pos(ui.settings, 0, 0);
    lv_obj_set_style_bg_color(ui.settings, lv_color_hex(0x12212c), 0);
    lv_obj_set_style_bg_opa(ui.settings, LV_OPA_90, 0);
    lv_obj_set_style_border_width(ui.settings, 0, 0);
    lv_obj_set_style_pad_left(ui.settings, ui.width == 240 ? 50 : 9, 0);
    lv_obj_set_style_pad_right(ui.settings, ui.width == 240 ? 50 : 9, 0);
    lv_obj_set_style_pad_top(ui.settings, ui.width == 240 ? 38 : 8, 0);
    lv_obj_set_style_pad_bottom(ui.settings, ui.width == 240 ? 38 : 8, 0);
    lv_obj_set_flex_flow(ui.settings, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui.settings, 5, 0);
    int row_w = ui.width - (ui.width == 240 ? 100 : 18) - 4;
    label(ui.settings, "Einstellungen", row_w);
    button(ui.settings, "Seltsam", row_w, 34, morse_open, NULL);
    button(ui.settings, "Morsecode", row_w, 34, interaction_open, NULL);
    button(ui.settings, "Medikamente: Stunden", row_w, 34, edit_button,
           (void *)(intptr_t)EDIT_MEDICATION);
    button(ui.settings, "Bilder: Stunden", row_w, 34, edit_button,
           (void *)(intptr_t)EDIT_IMAGE);
    button(ui.settings, "Nächste Fälligkeit HH:MM", row_w, 40, edit_button,
           (void *)(intptr_t)EDIT_NEXT_TIME);
    button(ui.settings, "Uhrdesign", row_w, 34, style_open, NULL);
    label(ui.settings, "Ausrichtung", row_w);
    static const char *angle_map[] = {"0°", "\n", "90°", "\n",
                                      "180°", "\n", "270°", ""};
    option_matrix(ui.settings, angle_map, row_w, 4, true);
    label(ui.settings, "Bildrotation", row_w);
    static const char *rotation_map[] = {"Aus", "\n", "Nach Namen", "\n",
                                         "Zufall", ""};
    option_matrix(ui.settings, rotation_map, row_w, 3, false);
    lv_obj_t *import = button(ui.settings, "Lade Bilder von URL/Github\nSpäter verfügbar",
                              row_w, 48, NULL, NULL);
    lv_obj_add_state(import, LV_STATE_DISABLED);
    button(ui.settings, "Status", row_w, 34, status_button, NULL);
    button(ui.settings, "Zurück", row_w, 34, close_settings, NULL);
    if (ui.feedback && !lv_obj_has_flag(ui.feedback, LV_OBJ_FLAG_HIDDEN))
        lv_obj_move_foreground(ui.feedback);
}

/* Stable status rows avoid deleting/reallocating every label each second. */
static lv_obj_t *status_label(uint32_t *next, const char *text, int width)
{
    lv_obj_t *item = lv_obj_get_child(ui.status, (int32_t)(*next)++);
    if (!item) return label(ui.status, text, width);
    if (strcmp(lv_label_get_text(item), text) != 0)
        lv_label_set_text(item, text);
    return item;
}

static void update_status(void)
{
    if (!ui.status) return;
    uint32_t next = 2;
    int text_w = ui.width - (ui.width == 240 ? 100 : 20);
    char date[64], line[128];
    ClockSample last = ui.state.last_confirmation;
    bool has_last = ui.state.has_last_confirmation;
    if (!has_last && ui.services.last_confirmation)
        has_last = ui.services.last_confirmation(ui.services.context, &last);
    if (has_last && last.utc_valid)
        format_local(date, sizeof(date), (UtcInstant){true, last.utc_ms});
    else if (has_last) snprintf(date, sizeof(date), "bestätigt, Uhrzeit unbekannt");
    else snprintf(date, sizeof(date), "nicht bekannt");
    snprintf(line, sizeof(line), "Letzte Gabe: %s", date);
    lv_obj_t *item = status_label(&next, line, text_w);
    if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    char hours[32];
    format_hours(hours, sizeof(hours), ui.state.settings.image_seconds);
    snprintf(line, sizeof(line), "Bilderabstand: %s Stunden", hours);
    item = status_label(&next, line, text_w);
    if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    format_local(date, sizeof(date), ui.state.deadline.effective_utc);
    snprintf(line, sizeof(line), "Nächster Alarm: %s", date);
    item = status_label(&next, line, text_w);
    if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    item = status_label(&next, ui.clock.quality == CLOCK_SYNCHRONIZED ? "Uhr: synchronisiert" :
          ui.clock.quality == CLOCK_HOLDOVER ? "Uhr: Zeit nicht synchronisiert" :
                                               "Uhrzeit nicht verfügbar", text_w);
    if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    UiNetworkStatus network = {0};
    if (ui.services.network_status)
        ui.services.network_status(ui.services.context, &network);
    item = status_label(&next, !ui.services.network_status ? "WLAN: unbekannt" :
                 !network.configured ? "WLAN: nicht konfiguriert" :
                 network.connected ? "WLAN: verbunden" : "WLAN: getrennt", text_w);
    if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    if (!ui.services.network_status) snprintf(line, sizeof(line), "Mesh-Funkkanal: unbekannt");
    else if (!network.mesh_active) snprintf(line, sizeof(line), "Mesh-Funkkanal: inaktiv");
    else if (network.mesh_channel >= 1 && network.mesh_channel <= 14)
        snprintf(line, sizeof(line), "Mesh-Funkkanal: %u", network.mesh_channel);
    else snprintf(line, sizeof(line), "Mesh-Funkkanal: unbekannt");
    item = status_label(&next, line, text_w);
    if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    const char *mesh_reason = (network.mesh_quality & 2u) ? "Speicherfehler" :
        (network.mesh_quality & 1u) ? "Speicher voll" :
        (network.mesh_quality & 8u) ? "unvollständig" : "Abgleich";
    if (!network.mesh_active) snprintf(line, sizeof(line), "Mesh aktuell (inkl. Gerät): inaktiv");
    else if (!network.mesh_current_valid) snprintf(line, sizeof(line), "Mesh aktuell (inkl. Gerät): unbekannt");
    else if (network.mesh_quality & (4u | 8u))
        snprintf(line, sizeof(line), "Mesh aktuell (inkl. Gerät): mindestens %u (%s)",
                 network.mesh_current_count, (network.mesh_quality & 8u) ? "unvollständig" : "Wiederaufbau");
    else snprintf(line, sizeof(line), "Mesh aktuell (inkl. Gerät): %u", network.mesh_current_count);
    item = status_label(&next, line, text_w);
    if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    if (!network.mesh_active) snprintf(line, sizeof(line), "Mesh bisher erfasst (ab Einführung): inaktiv");
    else if (!network.mesh_history_complete)
        snprintf(line, sizeof(line), "Mesh bisher erfasst (ab Einführung): mindestens %u (%s)",
                 network.mesh_history_count, mesh_reason);
    else snprintf(line, sizeof(line), "Mesh bisher erfasst (ab Einführung): %u", network.mesh_history_count);
    item = status_label(&next, line, text_w);
    if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    snprintf(line, sizeof(line), "IP: %s", network.configured && network.connected && network.station_ip[0]
             ? network.station_ip : "nicht verfügbar");
    item = status_label(&next, line, text_w);
    if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    snprintf(line, sizeof(line), "Gateway: %s", network.configured && network.connected && network.gateway[0]
             ? network.gateway : "nicht verfügbar");
    item = status_label(&next, line, text_w);
    if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    snprintf(line, sizeof(line), "DNS-IP: %s", network.configured && network.connected && network.dns_ip[0]
             ? network.dns_ip : "unbekannt");
    item = status_label(&next, line, text_w);
    if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    snprintf(line, sizeof(line), "Zeitserver-IP (letzter lokaler Sync): %s",
             network.configured && network.connected && network.sync_server_ip[0]
             ? network.sync_server_ip : "unbekannt");
    item = status_label(&next, line, text_w);
    if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    if (network.configured && network.ap_ip[0]) {
        snprintf(line, sizeof(line), "AP-IP: %s", network.ap_ip);
        item = status_label(&next, line, text_w);
        if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    }
    const uint8_t pending = ui.services.pending_count
        ? ui.services.pending_count(ui.services.context) : 0;
    if (ui.state.confirmation_pending_sync || pending != 0) {
        item = status_label(&next, "Bestätigung noch nicht synchronisiert", text_w);
        if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    }
    if (ui.shared_pending) {
        item = status_label(&next, "Gemeinsame Änderung wird geprüft", text_w);
        if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    }
    if (ui.shared_result_status != 0) {
        item = status_label(&next, ui.shared_result_status > 0 ?
                     "Letzte gemeinsame Änderung übernommen" :
                     "Letzte gemeinsame Änderung abgelehnt", text_w);
        if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    }
    if (ui.services.storage_ready && !ui.services.storage_ready(ui.services.context)) {
        item = status_label(&next, "Speicher: nicht verfügbar", text_w);
        if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    }
    const bool divergent = ui.state.divergent ||
        (ui.services.durable_divergence && ui.services.durable_divergence(ui.services.context));
    if (divergent) {
        item = status_label(&next, "Gruppenzustand weicht ab", text_w);
        if (ui.height < 200) lv_obj_set_style_text_font(item, &katmed_font_12, 0);
    }
    const uint8_t conflicts = ui.services.conflict_count
        ? ui.services.conflict_count(ui.services.context) : 0;
    for (uint8_t i=0;i<conflicts;++i) {
        UiConflict conflict;
        if (!ui.services.conflict_at ||
            !ui.services.conflict_at(ui.services.context,i,&conflict)) continue;
        snprintf(line,sizeof(line),"Nicht übernommen: Runde %llu/%llu, Ereignis %llu/%llu",
                 (unsigned long long)conflict.round.origin.device,
                 (unsigned long long)conflict.round.ordinal,
                 (unsigned long long)conflict.id.device,
                 (unsigned long long)conflict.id.sequence);
        item=status_label(&next,line,text_w);
        if (ui.height<200) lv_obj_set_style_text_font(item,&katmed_font_12,0);
        if (conflict.occurred_at.utc_valid) {
            format_local(date,sizeof(date),(UtcInstant){true,conflict.occurred_at.utc_ms});
            snprintf(line,sizeof(line),"Ereigniszeit: %s (%s)",date,
                     conflict.occurred_at.quality==CLOCK_SYNCHRONIZED
                         ? "synchronisiert" : "nicht synchronisiert");
        } else {
            snprintf(line,sizeof(line),"Ereigniszeit: UTC unbekannt; Domäne %llu, ms %llu",
                     (unsigned long long)conflict.occurred_at.domain,
                     (unsigned long long)conflict.occurred_at.monotonic_ms);
        }
        item=status_label(&next,line,text_w);
        if (ui.height<200) lv_obj_set_style_text_font(item,&katmed_font_12,0);
    }
    while (lv_obj_get_child_count(ui.status) > next)
        lv_obj_delete(lv_obj_get_child(ui.status, next));
}

static void build_screen(int16_t width, int16_t height)
{
    ui.page = ui.interaction_title = ui.send_button = NULL;
    ui.rx_layer = ui.notice = NULL;
    ui.overlay_key = 0;
    ++ui.overlay_generation;
    ui.rx_until = 0;
    ui.screen = lv_screen_active();
    ui.width = width;
    ui.height = height;
    ui.screen_orientation = ui.local.orientation;
    ui.background = NULL;
    ui.fallback = NULL;
    ui.clock_view = NULL;
    ui.alarm_bar = NULL;
    for (unsigned i = 0; i < ALARM_STRIPS; ++i) ui.alarm_strips[i] = NULL;
    ui.alarm_color_shown = false;
    ui.alarm_text = NULL;
    ui.settings = NULL;
    ui.status = NULL;
    ui.keypad = NULL;
    ui.feedback = NULL;
    ui.morse_layer = NULL;
    ui.morse_color_valid = false;
    ui.entry_label = NULL;
    ui.preview_label = NULL;
    ui.style_dialog = NULL;
    ui.style_title = NULL;
    for (int i = 0; i < 3; ++i) ui.style_options[i] = NULL;
    ui.edit = EDIT_NONE;
    ui.preview_valid = false;
    ui.shown_clock_tick = INT64_MIN;
    lv_obj_clean(ui.screen);
    lv_obj_set_style_bg_color(ui.screen, lv_color_hex(0x343b42), 0);
    lv_obj_set_style_pad_all(ui.screen, 0, 0);
    ui.background = lv_image_create(ui.screen);
    lv_obj_set_pos(ui.background, 0, 0);
    ui.fallback = label(ui.screen, "Bild nicht verfügbar", width);
    lv_obj_set_size(ui.fallback, width, height);
    lv_obj_set_style_bg_color(ui.fallback, lv_color_hex(0x555e66), 0);
    lv_obj_set_style_bg_opa(ui.fallback, LV_OPA_COVER, 0);
    lv_obj_set_style_text_align(ui.fallback, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ui.fallback);
    ui.image_id = UINT32_MAX;
    ui.alarm_bar = lv_obj_create(ui.screen);
    int alarm_y = height == 240 ? 30 : height == 320 ? 40 : 22;
    int alarm_end = height == 240 ? 210 : height == 320 ? 280 : 151;
    lv_obj_set_pos(ui.alarm_bar, 0, alarm_y);
    lv_obj_set_size(ui.alarm_bar, width, alarm_end - alarm_y);
    lv_obj_set_style_bg_color(ui.alarm_bar, lv_color_hex(0x1565c0), 0);
    lv_obj_set_style_bg_opa(ui.alarm_bar, 217, 0);
    lv_obj_set_style_border_width(ui.alarm_bar, 0, 0);
    lv_obj_set_style_radius(ui.alarm_bar, 0, 0);
    lv_obj_set_style_pad_all(ui.alarm_bar, 0, 0);
    lv_obj_add_flag(ui.alarm_bar, LV_OBJ_FLAG_HIDDEN);
    for (unsigned i = 0; i < ALARM_STRIPS; ++i) {
        const int top = alarm_y + (alarm_end - alarm_y) * (int)i / (int)ALARM_STRIPS;
        const int bottom = alarm_y + (alarm_end - alarm_y) * (int)(i + 1u) / (int)ALARM_STRIPS;
        lv_obj_t *strip = lv_obj_create(ui.screen);
        ui.alarm_strips[i] = strip;
        lv_obj_set_pos(strip, 0, top);
        lv_obj_set_size(strip, width, bottom - top);
        lv_obj_set_style_bg_color(strip, lv_color_hex(0x1565c0), 0);
        lv_obj_set_style_bg_opa(strip, 217, 0);
        lv_obj_set_style_border_width(strip, 0, 0);
        lv_obj_set_style_radius(strip, 0, 0);
        lv_obj_set_style_pad_all(strip, 0, 0);
        lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);
    }
    ui.alarm_text = label(ui.screen, "Please customize me! This is Alarm!", width == 240 ? 170 : width - 24);
    lv_obj_set_style_bg_color(ui.alarm_text, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui.alarm_text, LV_OPA_70, 0);
    lv_obj_set_style_text_align(ui.alarm_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(ui.alarm_text, 5, 0);
    lv_obj_center(ui.alarm_text);
    lv_obj_add_flag(ui.alarm_text, LV_OBJ_FLAG_HIDDEN);
    ui.feedback = label(ui.screen, "", width == 240 ? 170 : width - 16);
    lv_obj_set_style_bg_color(ui.feedback, lv_color_hex(0x1c2630), 0);
    lv_obj_set_style_bg_opa(ui.feedback, LV_OPA_90, 0);
    lv_obj_set_style_text_align(ui.feedback, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui.feedback, LV_ALIGN_BOTTOM_MID, 0, width == 240 ? -20 : -4);
    lv_obj_add_flag(ui.feedback, LV_OBJ_FLAG_HIDDEN);
    ui.morse_layer = lv_obj_create(ui.screen);
    lv_obj_set_pos(ui.morse_layer, 0, 0);
    lv_obj_set_size(ui.morse_layer, width, height);
    lv_obj_set_style_bg_color(ui.morse_layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui.morse_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.morse_layer, 0, 0);
    lv_obj_set_style_radius(ui.morse_layer, 0, 0);
    lv_obj_set_style_pad_all(ui.morse_layer, 0, 0);
    lv_obj_remove_flag(ui.morse_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(ui.morse_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui.morse_layer, LV_OBJ_FLAG_HIDDEN);
}

static void clock_move(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

static void animate_clock(bool opening)
{
    if (opening && !ui.clock_view && ui.screen) {
        ui.clock_view = clock_view_create(ui.screen);
        ui.clock_initialized = false;
        ui.shown_clock_tick = INT64_MIN;
    }
    if (!ui.clock_view) return;
    lv_anim_delete(ui.clock_view, clock_move);
    if (opening) lv_obj_remove_flag(ui.clock_view, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, ui.clock_view);
    lv_anim_set_exec_cb(&anim, clock_move);
    lv_anim_set_values(&anim, opening ? -ui.width : lv_obj_get_x(ui.clock_view),
                       opening ? 0 : -ui.width);
    lv_anim_set_duration(&anim, 200);
    lv_anim_start(&anim);
}

void ui_set_services(const UiServices *services)
{
    ui.services = services ? *services : (UiServices){0};
}

static bool interaction_input(KatmedInteractionInputKind kind, uint64_t target,
                              uint64_t impulse, uint64_t grant, uint64_t now)
{
    KatmedInteractionInput input = {.kind = kind, .menu_entry = ui.menu_entry,
        .contact_id = ui.active_contact, .target_id = target,
        .impulse_id = impulse, .grant_id = grant};
    return ui.services.interaction_input &&
        ui.services.interaction_input(ui.services.context, &input, now);
}

static int edge_size(void)
{
    int edge = ui.height / 10;
    return edge < 16 ? 16 : edge > 32 ? 32 : edge;
}

static bool at_edge(int y) { return y < edge_size() || y >= ui.height - edge_size(); }

static void interaction_cancel(uint64_t now)
{
    if (ui.send_contact)
        interaction_input(KATMED_INTERACTION_CANCEL, 0, 0, ui.interaction.grant_id, now);
    ui.send_contact = false;
    if (ui.page && ui.view == UI_VIEW_INTERACTION)
        lv_obj_set_style_bg_color(ui.page, lv_color_hex(0x101c27), 0);
}

static void delete_page(void)
{
    if (ui.page) lv_obj_delete(ui.page);
    ui.page = ui.interaction_title = ui.send_button = NULL;
}

static void change_page(int horizontal, int vertical, uint64_t now)
{
    if (ui.view == UI_VIEW_INTERACTION) {
        interaction_cancel(now);
        interaction_input(KATMED_INTERACTION_LEAVE, 0, 0, 0, now);
    }
    if (ui.keypad) cancel_edit(NULL);
    if (ui.style_dialog) style_cancel(NULL);
    destroy_settings();
    delete_page();
    ui_grid_move(&ui.grid, horizontal, vertical);
    UiView next = ui_grid_view(ui.grid);
    if (ui.view == CLOCK && next != CLOCK) animate_clock(false);
    ui.view = next;
    if (next == CLOCK) animate_clock(true);
}

static void interaction_open(lv_event_t *event)
{
    (void)event;
    if (ui.view != SETTINGS) return;
    ++ui.menu_entry;
    if (!ui.menu_entry) ++ui.menu_entry;
    ui.active_contact = 0;
    ui.interaction_start_contact = true;
#ifdef ESP_PLATFORM
    ui.interaction_activation_ms = (uint64_t)esp_timer_get_time()/1000u;
#else
    ui.interaction_activation_ms = ui.clock.monotonic_ms;
#endif
    interaction_input(KATMED_INTERACTION_ENTER, 0, 0, 0, ui.interaction_activation_ms);
    destroy_settings();
    ui.view = UI_VIEW_INTERACTION;
}

static lv_obj_t *flat_layer(void)
{
    lv_obj_t *obj = lv_obj_create(ui.screen);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, ui.width, ui.height);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static void build_page(void)
{
    if (ui.page) return;
    if (ui.view != UI_VIEW_PLACEHOLDER && ui.view != UI_VIEW_INTERACTION) return;
    ui.page = flat_layer();
    lv_obj_set_style_bg_opa(ui.page, ui.view == UI_VIEW_PLACEHOLDER ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.page, lv_color_hex(0x101c27), 0);
    bool round = ui.width == 240 && ui.height == 240;
    if (ui.view == UI_VIEW_PLACEHOLDER) {
        int w = round ? 150 : ui.width - 40;
        if (w > 180) w = 180;
        int h = ui.height < 200 ? 58 : 78;
        lv_obj_t *alarm = button(ui.page, "Alarm", w, h, NULL, NULL);
        lv_obj_set_pos(alarm, (ui.width-w)/2, (ui.height-h)/2-12);
        lv_obj_set_style_bg_color(alarm, lv_color_hex(0xd01818), 0);
        lv_obj_set_style_radius(alarm, 30, 0);
        lv_obj_t *text = label(ui.page, "Nur im Notfall. Drücken kostet!", round ? 160 : ui.width-24);
        lv_obj_set_style_text_font(text, &katmed_font_12, 0);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(text, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(text, LV_OPA_70, 0);
        lv_obj_set_style_pad_all(text, 4, 0);
        lv_obj_set_style_radius(text, 8, 0);
        lv_obj_align(text, LV_ALIGN_CENTER, 0, h/2+14);
        return;
    }
    int upper = ui.height == 240 ? 136 : ui.height == 320 ? 180 : 94;
    ui.interaction_title = label(ui.page, "Verbindung wird geprüft", round ? 170 : ui.width-24);
    lv_obj_set_style_text_align(ui.interaction_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(ui.interaction_title, round ? 35 : 12, round ? 46 : 24);
    int available = ui.height-edge_size()-upper-4;
    int diameter = available < ui.width-24 ? available : ui.width-24;
    if (diameter > 96) diameter = 96;
    ui.send_button = button(ui.page, "Senden", diameter, diameter, NULL, NULL);
    lv_obj_set_pos(ui.send_button, (ui.width-diameter)/2, upper+2);
    lv_obj_set_style_radius(ui.send_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(ui.send_button, 0), &katmed_font_12, 0);
}

static bool send_hit(int x, int y)
{
    if (!ui.send_button || at_edge(y)) return false;
    int diameter = lv_obj_get_width(ui.send_button);
    int dx = x-lv_obj_get_x(ui.send_button)-diameter/2;
    int dy = y-lv_obj_get_y(ui.send_button)-diameter/2;
    return dx*dx+dy*dy <= diameter*diameter/4;
}

static unsigned notice_kind(void)
{
    if (ui.state.alarm_active) return 0;
    if (ui.view == UI_VIEW_INTERACTION && ui.interaction.finish_prompt) return 1;
    if (ui.view == UI_VIEW_INTERACTION && ui.interaction.sender_finished) return 2;
    return ui.interaction.greeting_visible &&
        ui.clock.monotonic_ms < ui.interaction.greeting_until_ms ? 3 : 0;
}

static uint64_t notice_id(unsigned kind)
{
    return kind == 1 ? ui.interaction.prompt_id : kind == 2 ?
           ui.interaction.completion_id : kind == 3 ? ui.interaction.greeting_id : 0;
}

static void update_interaction(uint64_t now)
{
    if (ui.services.interaction_status)
        ui.services.interaction_status(ui.services.context, &ui.interaction, now);
    else memset(&ui.interaction, 0, sizeof(ui.interaction));
    if (ui.interaction.overflow && !ui.interaction_overflow_shown && !ui.state.alarm_active) {
        show_feedback("Impulse: Speicher voll");
        ui.interaction_loss_until_ms = ui.feedback_until_ms;
    }
    ui.interaction_overflow_shown = ui.interaction.overflow && !ui.state.alarm_active;
    if (ui.interaction_title) {
        const char *text = !ui.interaction.connected ? "Mesh nicht verbunden" :
            !ui.interaction.synchronized ? "Mesh wird abgeglichen" :
            ui.interaction.overflow ? "Impulse: Speicher voll" :
            ui.interaction.owns_token ? "Du sendest" :
            ui.interaction.mode == KATMED_INTERACTION_REQUESTING ? "Anfrage läuft" :
            ui.interaction.sender_finished ? "Absender ist fertig" :
            ui.interaction.can_request ? "Bereit zum Senden" : "Anderes Gerät sendet";
        if (strcmp(lv_label_get_text(ui.interaction_title), text))
            lv_label_set_text(ui.interaction_title, text);
    }
    if (ui.rx_authority != ui.interaction.authority_id ||
        ui.rx_generation != ui.interaction.authority_generation) {
        ui.rx_until = ui.rx_gap_until = 0;
        ui.rx_authority = ui.interaction.authority_id;
        ui.rx_generation = ui.interaction.authority_generation;
    }
    if (ui.rx_until && now >= ui.rx_until) {
        ui.rx_until = 0;
        ui.rx_gap_until = now+50; /* retain a visible separation between completed impulses */
    }
    if (ui.send_button) {
        if (ui.interaction.owns_token || ui.interaction.can_request)
            lv_obj_remove_state(ui.send_button, LV_STATE_DISABLED);
        else lv_obj_add_state(ui.send_button, LV_STATE_DISABLED);
    }
    unsigned kind = notice_kind();
    uint64_t key = notice_id(kind);
    static unsigned previous_kind;
    if (key != ui.overlay_key || kind != previous_kind || (kind && !ui.notice)) {
        if (ui.touch_down) {
            interaction_cancel(now);
            ui.reject_contact = true;
        }
        ++ui.overlay_generation;
        ui.overlay_since_ms = now;
        ui.overlay_key = key;
        previous_kind = kind;
        if (ui.notice) lv_obj_delete(ui.notice);
        ui.notice = NULL;
        if (kind) {
            ui.notice = flat_layer();
            lv_obj_set_style_bg_color(ui.notice, lv_color_hex(0x12212c), 0);
            lv_obj_set_style_bg_opa(ui.notice, LV_OPA_COVER, 0);
            bool round = ui.width == 240;
            int w = round ? 170 : ui.width-20;
            lv_obj_t *title = label(ui.notice, kind == 3 ? "Telacore" :
                                   kind == 1 ? "Fertig?" : "Absender ist fertig", w);
            lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(title, kind == 3 ? &katmed_font_26 : &katmed_font_14, 0);
            lv_obj_set_pos(title, (ui.width-w)/2, ui.height/2-55);
            if (kind == 3) {
                lv_obj_t *text = label(ui.notice, "Neues Gerät im Mesh", w);
                lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_pos(text, (ui.width-w)/2, ui.height/2-12);
                text = label(ui.notice, "Herzlich willkommen", w);
                lv_obj_set_style_text_font(text, &katmed_font_12, 0);
                lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_pos(text, (ui.width-w)/2, ui.height/2+25);
            } else {
                lv_obj_t *yes = button(ui.notice, kind == 1 ? "Ja" : "OK", 62, 32, NULL, NULL);
                lv_obj_set_pos(yes, ui.width/2-(kind == 1 ? 68 : 31), ui.height/2+15);
                if (kind == 1) {
                    lv_obj_t *more = button(ui.notice, "Weiter", 62, 32, NULL, NULL);
                    lv_obj_set_pos(more, ui.width/2+6, ui.height/2+15);
                }
            }
        }
    }
    if (ui.page && ui.view == UI_VIEW_INTERACTION) {
        bool sending = ui.send_contact && ui.touch_down && ui.interaction.owns_token &&
            ui.interaction.key_down && ui.interaction.connected && ui.interaction.synchronized &&
            ui.interaction.menu_entry == ui.menu_entry && !ui.state.alarm_active && !kind;
        lv_obj_set_style_bg_color(ui.page, lv_color_hex(sending ? 0x87cefa : 0x101c27), 0);
    }
    if (ui.notice) lv_obj_move_foreground(ui.notice);
    bool white = !ui.state.alarm_active && !ui.interaction.owns_token &&
        ((ui.interaction.receive_white && now < ui.interaction.receive_until_ms) || now < ui.rx_until);
    if (!white && now >= ui.rx_gap_until && ui.interaction.impulse_count) {
        KatmedInteractionImpulse impulse = ui.interaction.impulses[0];
        if (interaction_input(KATMED_INTERACTION_CONSUME_IMPULSE, 0,
                              impulse.impulse_id, impulse.grant_id, now) &&
            !ui.state.alarm_active && now < impulse.valid_until_ms) {
            ui.rx_grant = impulse.grant_id;
            ui.rx_impulse = impulse.impulse_id;
            uint32_t duration = impulse.duration_ms < 50 ? 50 : impulse.duration_ms;
            ui.rx_until = now+duration;
            if (ui.rx_until > impulse.valid_until_ms) ui.rx_until = impulse.valid_until_ms;
            white = true;
        }
    }
    if (ui.state.alarm_active || !ui.interaction.connected) {
        white = false;
        ui.rx_until = 0;
    }
    if (white && !ui.rx_layer) {
        ui.rx_layer = flat_layer();
        lv_obj_set_style_bg_color(ui.rx_layer, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(ui.rx_layer, LV_OPA_COVER, 0);
    }
    if (ui.rx_layer) {
        lv_obj_set_height(ui.rx_layer, ui.view == UI_VIEW_INTERACTION ?
            (ui.height == 240 ? 136 : ui.height == 320 ? 180 : 94) : ui.height);
        if (white && ui.view != MORSE) {
            lv_obj_remove_flag(ui.rx_layer, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(ui.rx_layer);
        } else lv_obj_add_flag(ui.rx_layer, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui.notice) lv_obj_move_foreground(ui.notice);
    if (!ui.state.alarm_active && ui.feedback && now < ui.interaction_loss_until_ms)
        lv_obj_move_foreground(ui.feedback);
}

bool ui_lvgl_touch_allowed(bool down, int16_t x, int16_t y, bool valid, uint64_t now)
{
    (void)now;
    if (!valid) { ui.lv_blocked = true; return false; }
    if (!down) {
        bool allowed = !ui.lv_blocked && ui.lv_overlay_generation == ui.overlay_generation;
        ui.lv_contact = false;
        ui.lv_blocked = false;
        return allowed;
    }
    if (!ui.lv_contact) {
        ui.lv_contact = true;
        ui.lv_start_x = x; ui.lv_start_y = y;
        ui.lv_overlay_generation = ui.overlay_generation;
        ui.lv_blocked = ui.state.alarm_active || notice_kind() ||
            ui.view == MORSE || ui.view == UI_VIEW_INTERACTION || at_edge(y) ||
            (ui.view != SETTINGS && ui.view != UI_VIEW_STATUS);
    }
    if (ui.lv_overlay_generation != ui.overlay_generation ||
        (abs(x-ui.lv_start_x) >= 40 && abs(x-ui.lv_start_x) >= 2*abs(y-ui.lv_start_y)))
        ui.lv_blocked = true;
    return !ui.lv_blocked;
}

UiView ui_current_view(void)
{
    return ui.view;
}

static void finish_morse_start_contact(void)
{
    ui.morse_start_contact = false;
    ui.touch_down = false;
    ui.reject_contact = false;
    ui.touch_armed = true;
}

void ui_touch_sample(bool touched, int16_t x, int16_t y,
                     bool valid, uint64_t monotonic_ms)
{
    unsigned modal = notice_kind();
    if (modal) {
        if (!valid) { ui.reject_contact = true; return; }
        if (touched) {
            if (!ui.touch_down) {
                ui.touch_down = true;
                ui.overlay_contact = ui.touch_armed && monotonic_ms > ui.overlay_since_ms;
                ui.start_x = x; ui.start_y = y; ui.down_ms = monotonic_ms;
            }
            ui.last_x = x; ui.last_y = y;
            if (abs(x-ui.start_x) >= 20 || abs(y-ui.start_y) >= 20)
                ui.reject_contact = true;
        } else {
            bool tap = ui.touch_down && ui.overlay_contact && !ui.reject_contact &&
                monotonic_ms >= ui.down_ms+30 && monotonic_ms <= ui.down_ms+600;
            if (tap) {
                KatmedInteractionInputKind kind = KATMED_INTERACTION_DISMISS_GREETING;
                bool hit = modal == 3;
                if (modal != 3 && ui.last_y >= ui.height/2+15 &&
                    ui.last_y < ui.height/2+47) {
                    if (modal == 2) {
                        hit = abs(ui.last_x-ui.width/2) <= 31;
                        kind = KATMED_INTERACTION_ACK_FINISHED;
                    } else if (ui.last_x >= ui.width/2-68 && ui.last_x < ui.width/2-6) {
                        hit = true; kind = KATMED_INTERACTION_FINISH;
                    } else if (ui.last_x >= ui.width/2+6 && ui.last_x < ui.width/2+68) {
                        hit = true; kind = KATMED_INTERACTION_CONTINUE;
                    }
                }
                if (hit) interaction_input(kind, notice_id(modal), 0,
                                           ui.interaction.grant_id, monotonic_ms);
            }
            ui.touch_down = ui.overlay_contact = ui.reject_contact = false;
            ui.touch_armed = true;
        }
        return;
    }
    if (ui.view == UI_VIEW_INTERACTION && valid &&
        monotonic_ms <= ui.interaction_activation_ms) {
        if (!touched && ui.interaction_start_contact) {
            ui.interaction_start_contact = false;
            ui.touch_down = false;
            ui.touch_armed = true;
        }
        return;
    }
    if (ui.view == UI_VIEW_INTERACTION && ui.interaction_start_contact) {
        if (valid && !touched) {
            ui.interaction_start_contact = false;
            ui.touch_down = false;
            ui.touch_armed = true;
        }
        return;
    }
    if (ui.send_contact) {
        if (!valid || (touched && !send_hit(x, y))) {
            interaction_cancel(monotonic_ms);
            ui.reject_contact = true;
        } else if (!touched) {
            interaction_input(KATMED_INTERACTION_UP, 0, 0,
                              ui.interaction.grant_id, monotonic_ms);
            ui.send_contact = false;
            ui.touch_down = false;
            ui.touch_armed = true;
            if (ui.page && ui.view == UI_VIEW_INTERACTION)
                lv_obj_set_style_bg_color(ui.page, lv_color_hex(0x101c27), 0);
            return;
        } else return;
    }
    if (ui.view == MORSE && valid) {
        /* The board sampler can deliver an older queued sample after LVGL's
         * click-on-release callback has already started this mode. */
        if (monotonic_ms <= ui.morse_activation_ms) {
            if (!touched && ui.morse_start_contact)
                finish_morse_start_contact();
            return;
        }
        if (ui.morse_start_contact) {
            if (!touched) finish_morse_start_contact();
            return;
        }
        if (touched && !ui.touch_down) {
            katmed_morse_stop(&ui.morse);
            ui.view = BACKGROUND;
            ui.grid = (UiGrid){1, 1};
            ui.touch_down = true;
            ui.reject_contact = true;
            ui.reject_confirm_motion = true;
            if (ui.morse_layer) lv_obj_add_flag(ui.morse_layer, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        if (!touched) {
            ui.touch_armed = true;
            return;
        }
        return;
    }
    if (!valid) {
        if (ui.touch_down) ui.reject_contact = true;
        else ui.touch_armed = false;
        return;
    }
    if (!touched) {
        if (!ui.touch_down) {
            ui.touch_armed = true;
            return;
        }
        ui.touch_down = false;
        if (ui.reject_contact) {
            ui.reject_contact = false;
            ui.reject_confirm_motion = false;
            ui.touch_armed = true;
            return;
        }
        if (monotonic_ms < ui.down_ms) return;
        uint64_t duration = monotonic_ms - ui.down_ms;
        int dx = (int)ui.last_x - ui.start_x;
        int dy = (int)ui.last_y - ui.start_y;
        if (duration < 30 || duration > 1000) return;
        if (ui.view == ALARM) {
            if (!ui.confirmed_round && !ui.reject_confirm_motion &&
                duration <= 600 && abs(dx) < 20 && abs(dy) < 20) {
                UiRequest request = {.kind = UI_REQUEST_CONFIRM};
                request_action(&request);
            }
            ui.reject_confirm_motion = false;
            return;
        }
        bool scrollable = ui.view == SETTINGS || ui.view == UI_VIEW_STATUS || ui.keypad || ui.style_dialog;
        if ((!scrollable || ui.edge_contact) && abs(dy) >= 40 && abs(dy) >= 2*abs(dx)) {
            change_page(0, dy > 0 ? -1 : 1, monotonic_ms);
            return;
        }
        if (ui.edge_contact || ui.keypad || ui.style_dialog || ui.view == UI_VIEW_INTERACTION) return;
        if (abs(dx) >= 40 && abs(dx) >= 2 * abs(dy)) {
            change_page(dx < 0 ? 1 : -1, 0, monotonic_ms);
        }
        return;
    }
    if (!ui.touch_armed) {
        ui.reject_contact = true;
        ui.touch_down = true;
        return;
    }
    if (!ui.touch_down) {
        ui.touch_down = true;
        ui.reject_confirm_motion = false;
        ui.start_x = x;
        ui.start_y = y;
        ui.down_ms = monotonic_ms;
        ui.edge_contact = at_edge(y);
        if (ui.view == UI_VIEW_INTERACTION && !ui.edge_contact && send_hit(x, y) &&
            (ui.interaction.can_request || ui.interaction.owns_token)) {
            ui.active_contact = ++ui.contact_sequence;
            ui.send_contact = interaction_input(KATMED_INTERACTION_DOWN, 0, 0,
                                                 ui.interaction.grant_id, monotonic_ms);
        }
    }
    if (abs((int)x - ui.start_x) >= 20 ||
        abs((int)y - ui.start_y) >= 20)
        ui.reject_confirm_motion = true;
    ui.last_x = x;
    ui.last_y = y;
}

static lv_color_t alarm_color(AlarmColor color)
{
    switch (color) {
    case ALARM_COLOR_YELLOW: return lv_color_hex(0xffc107);
    case ALARM_COLOR_RED: return lv_color_hex(0xd50000);
    case ALARM_COLOR_BLUE:
    default: return lv_color_hex(0x1565c0);
    }
}

static void set_bar_visible(lv_obj_t *bar, bool visible)
{
    if (!bar || lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN) != visible) return;
    if (blink_metrics.pending_changes < UINT32_MAX) ++blink_metrics.pending_changes;
    if (visible) lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
}

static void update_alarm_bar(const AppState *state, const ClockSample *clock)
{
    AlarmAnimation animation;
    const bool timed = state->alarm_active &&
        app_alarm_animation(state, clock, &animation) == APP_OK;
    const AlarmColor color = timed ? animation.color : ALARM_COLOR_BLUE;
    if (!ui.alarm_color_shown || ui.shown_alarm_color != color) {
        lv_color_t actual = alarm_color(color);
        lv_obj_set_style_bg_color(ui.alarm_bar, actual, 0);
        for (unsigned i = 0; i < ALARM_STRIPS; ++i)
            lv_obj_set_style_bg_color(ui.alarm_strips[i], actual, 0);
        ui.shown_alarm_color = color;
        ui.alarm_color_shown = true;
    }
    /* A foreign or unavailable clock never invents a new round phase. The
     * static blue fallback keeps the alarm and its text visibly present. */
    set_bar_visible(ui.alarm_bar, state->alarm_active && !timed);
    const int top = lv_obj_get_y(ui.alarm_bar);
    const int height = lv_obj_get_height(ui.alarm_bar);
    unsigned fill = 0, erased = 0;
    if (timed) {
        if (animation.phase_permyriad < 5000u)
            fill = ((unsigned)animation.phase_permyriad * (unsigned)height + 4999u) / 5000u;
        else {
            fill = (unsigned)height;
            erased = ((unsigned)animation.phase_permyriad - 5000u) * (unsigned)height / 5000u;
        }
    }
    /* Keep the six bounded render objects, but move the leading/trailing
     * edge in pixels rather than jumping a whole 30-pixel C3 strip. */
    for (unsigned i = 0; i < ALARM_STRIPS; ++i) {
        unsigned start = (unsigned)height * i / ALARM_STRIPS;
        unsigned end = (unsigned)height * (i + 1u) / ALARM_STRIPS;
        if (start < erased) start = erased;
        if (end > fill) end = fill;
        bool visible = timed && end > start;
        lv_obj_t *strip = ui.alarm_strips[i];
        if (visible && (lv_obj_get_y(strip) != top + (int)start ||
                        lv_obj_get_height(strip) != (int)(end - start))) {
            if (blink_metrics.pending_changes < UINT32_MAX) ++blink_metrics.pending_changes;
            lv_obj_set_pos(strip, 0, top + (int)start);
            lv_obj_set_height(strip, (int)(end - start));
        }
        set_bar_visible(strip, visible);
    }
    if (state->alarm_active) lv_obj_remove_flag(ui.alarm_text, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ui.alarm_text, LV_OBJ_FLAG_HIDDEN);
}

void ui_render(const AppState *state, const LocalSettings *local,
               UiView view, const ClockSample *clock)
{
    if (!state || !local || !clock) return;
    bool alarm_started = state->alarm_active && ui.view != ALARM;
    bool round_changed = ui.have_state && ui.state.alarm_active && state->alarm_active &&
                         !same_round(ui.state.round, state->round);
    if (round_changed && ui.touch_down) ui.reject_contact = true;
    if ((round_changed || alarm_started) && ui.feedback)
        lv_obj_add_flag(ui.feedback, LV_OBJ_FLAG_HIDDEN);
    ui.state = *state;
    ui.local = *local;
    ui.clock = *clock;
    ui.have_state = true;
    if (!state->alarm_active || !same_round(state->round, ui.confirmed_id))
        ui.confirmed_round = false;
    if (state->alarm_active) {
        if (alarm_started && ui.view == MORSE) {
            katmed_morse_pause(&ui.morse, clock->monotonic_ms);
            ui.morse_alarm_paused = ui.morse.active && ui.morse.paused;
            ui.morse_alarm_confirmed = false;
        }
        if (alarm_started) {
            ui.interaction_alarm_paused = ui.view == UI_VIEW_INTERACTION;
            interaction_cancel(clock->monotonic_ms);
            ++ui.overlay_generation;
        }
        ui.view = ALARM;
        if (alarm_started) {
            if (ui.touch_down) ui.reject_contact = true;
            if (ui.clock_view) {
                lv_anim_delete(ui.clock_view, clock_move);
                lv_obj_add_flag(ui.clock_view, LV_OBJ_FLAG_HIDDEN);
            }
            if (ui.keypad) cancel_edit(NULL);
            if (ui.style_dialog) style_cancel(NULL);
        }
    } else if (ui.view == ALARM) {
        if (ui.morse_alarm_paused && ui.morse_alarm_confirmed) {
            katmed_morse_resume(&ui.morse, clock->monotonic_ms);
            ui.view = ui.morse.active ? MORSE : BACKGROUND;
            if (ui.view == MORSE && ui.morse_layer)
                lv_obj_move_foreground(ui.morse_layer);
        } else {
            if (ui.morse_alarm_paused) katmed_morse_stop(&ui.morse);
            ui.view = BACKGROUND;
            ui.grid = (UiGrid){1, 1};
        }
        if (ui.interaction_alarm_paused) {
            ui.grid = (UiGrid){1, 2};
            ui.view = UI_VIEW_INTERACTION;
            ui.interaction_start_contact = true;
        }
        ui.interaction_alarm_paused = false;
        ui.morse_alarm_paused = false;
        ui.morse_alarm_confirmed = false;
    } else if (view != ui.view && view != ALARM) {
        /* Main may explicitly select a view; touch navigation is read through
         * ui_current_view() before the next call. */
        ui.view = view;
        if (view == BACKGROUND || view == CLOCK || view == SETTINGS)
            ui.grid = (UiGrid){1, view == CLOCK ? 0 : view == SETTINGS ? 2 : 1};
        else if (view == UI_VIEW_STATUS) ui.grid.row = 2;
        else if (view == UI_VIEW_PLACEHOLDER) ui.grid.row = 0;
    }

    lv_display_t *display = lv_display_get_default();
    if (!display) return;
    watch_display(display);
    int16_t width = (int16_t)lv_display_get_horizontal_resolution(display);
    int16_t height = (int16_t)lv_display_get_vertical_resolution(display);
    bool rebuild = !ui.screen || ui.screen != lv_screen_active() ||
                   width != ui.width || height != ui.height ||
                   local->orientation != ui.screen_orientation;
    bool editing_before_rebuild = rebuild && (ui.keypad || ui.style_dialog);
    if (rebuild) {
        interaction_cancel(clock->monotonic_ms);
        if (ui.touch_down) ui.reject_contact = true;
        build_screen(width, height);
        if (editing_before_rebuild) show_feedback("Eingabe nach Drehung verworfen");
    }
    if (ui.view != SETTINGS && ui.view != UI_VIEW_STATUS) {
        if (ui.keypad) cancel_edit(NULL);
        if (ui.style_dialog) style_cancel(NULL);
        destroy_settings();
    }
    if (ui.page && ui.view != UI_VIEW_PLACEHOLDER && ui.view != UI_VIEW_INTERACTION) delete_page();
    build_page();
    if (ui.view == UI_VIEW_STATUS && !ui.status) status_button(NULL);
    if (ui.view == CLOCK && !ui.clock_view) {
        ui.clock_view = clock_view_create(ui.screen);
        ui.clock_initialized = false;
        ui.shown_clock_tick = INT64_MIN;
        if (ui.clock_view) lv_obj_set_x(ui.clock_view, 0);
    } else if (ui.clock_view &&
               (ui.view == ALARM || ui.view == SETTINGS || ui.view == UI_VIEW_PLACEHOLDER ||
                ui.view == UI_VIEW_STATUS || ui.view == UI_VIEW_INTERACTION ||
                (ui.view == BACKGROUND && lv_obj_get_x(ui.clock_view) <= -width))) {
        lv_anim_delete(ui.clock_view, clock_move);
        lv_obj_delete(ui.clock_view);
        ui.clock_view = NULL;
        ui.shown_clock_tick = INT64_MIN;
    }
    if (ui.view == SETTINGS && !ui.keypad && !ui.style_dialog && !ui.status)
        build_settings();
    if (ui.image_id != state->image_id || ui.image_orientation != local->orientation) {
        katmed_images_present(ui.background, ui.fallback, state->image_id,
                              local->orientation);
        ui.image_id = state->image_id;
        ui.image_orientation = local->orientation;
    }
    if (ui.clock_view) {
        /* Analog seconds need a real UTC second update; digital labels remain minute based. */
        int64_t clock_tick = clock->utc_valid ? clock->utc_ms /
                         (local->clock_style == ANALOG ? 1000 : 60000) : INT64_MIN;
        if (!ui.clock_initialized || ui.shown_clock_tick != clock_tick || ui.shown_quality != clock->quality ||
            ui.shown_style != local->clock_style) {
            clock_view_update(ui.clock_view, local->clock_style, clock);
            ui.shown_clock_tick = clock_tick;
            ui.shown_quality = clock->quality;
            ui.shown_style = local->clock_style;
            ui.clock_initialized = true;
        }
        if (ui.view != CLOCK && ui.view != ALARM &&
            lv_obj_get_x(ui.clock_view) <= -width)
            lv_obj_add_flag(ui.clock_view, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui.status &&
        !lv_obj_has_flag(ui.status, LV_OBJ_FLAG_HIDDEN) &&
        ui.status_second != clock->monotonic_ms / 1000u) {
        update_status();
        ui.status_second = clock->monotonic_ms / 1000u;
    }
    if (ui.alarm_bar) update_alarm_bar(state, clock);
    if (ui.feedback && clock->monotonic_ms >= ui.feedback_until_ms)
        lv_obj_add_flag(ui.feedback, LV_OBJ_FLAG_HIDDEN);
    if (ui.morse_layer) {
        if (ui.view == MORSE && ui.morse.active) {
            bool white = katmed_morse_update(&ui.morse, clock->monotonic_ms);
            if (!ui.morse_color_valid || ui.morse_shown_white != white) {
                lv_obj_set_style_bg_color(ui.morse_layer,
                                          white ? lv_color_white() : lv_color_black(), 0);
                ui.morse_color_valid = true;
                ui.morse_shown_white = white;
            }
            if (lv_obj_has_flag(ui.morse_layer, LV_OBJ_FLAG_HIDDEN))
                lv_obj_remove_flag(ui.morse_layer, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (!lv_obj_has_flag(ui.morse_layer, LV_OBJ_FLAG_HIDDEN))
                lv_obj_add_flag(ui.morse_layer, LV_OBJ_FLAG_HIDDEN);
        }
    }
    update_interaction(clock->monotonic_ms);
}
