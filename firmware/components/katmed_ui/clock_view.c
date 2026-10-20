#include "clock_view.h"
#include "katmed_core.h"
#include "katmed_font.h"

#include "clock_font_30.inc"
#include "clock_font_68.inc"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    lv_obj_t *plate;
    lv_obj_t *time;
    lv_obj_t *date;
    lv_obj_t *quality;
    lv_obj_t *dial;
    lv_obj_t *marks[12];
    lv_obj_t *hour_outline;
    lv_obj_t *minute_outline;
    lv_obj_t *hour_hand;
    lv_obj_t *minute_hand;
    lv_obj_t *second_outline;
    lv_obj_t *second_hand;
    lv_obj_t *hub_outline;
    lv_obj_t *hub;
    lv_point_precise_t hour_points[6];
    lv_point_precise_t minute_points[6];
    lv_point_precise_t second_points[2];
} ClockWidgets;

static void cleanup(lv_event_t *event)
{
    lv_free(lv_event_get_user_data(event));
}

static lv_obj_t *clock_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

lv_obj_t *clock_view_create(lv_obj_t *parent)
{
    if (!parent) return NULL;
    lv_obj_t *view = lv_obj_create(parent);
    ClockWidgets *w = lv_malloc(sizeof(*w));
    if (!w) {
        lv_obj_delete(view);
        return NULL;
    }
    memset(w, 0, sizeof(*w));
    lv_obj_set_user_data(view, w);
    lv_obj_add_event_cb(view, cleanup, LV_EVENT_DELETE, w);
    lv_obj_set_scrollable(view, false);
    lv_obj_set_size(view, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(view, 0, 0);
    lv_obj_set_style_bg_opa(view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view, 0, 0);
    lv_obj_set_style_pad_all(view, 0, 0);

    w->plate = lv_obj_create(view);
    lv_obj_set_scrollable(w->plate, false);
    lv_obj_set_size(w->plate, lv_pct(84), lv_pct(78));
    lv_obj_center(w->plate);
    lv_obj_set_style_bg_color(w->plate, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(w->plate, LV_OPA_70, 0);
    lv_obj_set_style_border_width(w->plate, 0, 0);
    lv_obj_set_style_radius(w->plate, 18, 0);
    lv_obj_set_style_pad_all(w->plate, 4, 0);

    w->time = clock_label(w->plate, "--:--");
    lv_obj_set_style_text_font(w->time, &katmed_font_68, 0);
    w->date = clock_label(w->plate, "");
    lv_obj_set_style_text_font(w->date, &katmed_font_30, 0);
    w->quality = clock_label(w->plate, "Uhrzeit nicht verfügbar");
    lv_obj_set_style_text_font(w->quality, &katmed_font_14, 0);
    w->dial = lv_obj_create(view);
    lv_obj_set_scrollable(w->dial, false);
    lv_obj_set_style_bg_opa(w->dial, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(w->dial, lv_color_white(), 0);
    lv_obj_set_style_border_width(w->dial, 2, 0);
    lv_obj_set_style_radius(w->dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(w->dial, 0, 0);
    for (int i = 0; i < 12; ++i) {
        w->marks[i] = lv_line_create(w->dial);
        lv_obj_set_style_line_color(w->marks[i], lv_color_white(), 0);
        lv_obj_set_style_line_width(w->marks[i], i % 3 == 0 ? 3 : 2, 0);
    }
    /* Each black stroke sits immediately below its unchanged colored contour. */
    w->hour_outline = lv_line_create(w->dial);
    w->hour_hand = lv_line_create(w->dial);
    w->minute_outline = lv_line_create(w->dial);
    w->minute_hand = lv_line_create(w->dial);
    lv_obj_set_style_line_color(w->hour_outline, lv_color_black(), 0);
    lv_obj_set_style_line_color(w->minute_outline, lv_color_black(), 0);
    lv_obj_set_style_line_width(w->hour_outline, 5, 0);
    lv_obj_set_style_line_width(w->minute_outline, 5, 0);
    w->second_outline = lv_line_create(w->dial);
    lv_obj_set_style_line_color(w->second_outline, lv_color_black(), 0);
    /* 2px gives only a one-sided rim for axis-aligned LVGL strokes. */
    lv_obj_set_style_line_width(w->second_outline, 3, 0);
    lv_obj_set_style_line_rounded(w->second_outline, true, 0);
    w->second_hand = lv_line_create(w->dial);
    w->hub_outline = lv_obj_create(w->dial);
    lv_obj_set_scrollable(w->hub_outline, false);
    lv_obj_set_clickable(w->hub_outline, false);
    lv_obj_set_style_bg_opa(w->hub_outline, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(w->hub_outline, lv_color_black(), 0);
    lv_obj_set_style_border_width(w->hub_outline, 3, 0);
    lv_obj_set_style_radius(w->hub_outline, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(w->hub_outline, 0, 0);
    w->hub = lv_obj_create(w->dial);
    /* Own transparent contours, using the public factory demo's RGB565 hues. */
    lv_obj_set_style_line_color(w->hour_hand, lv_color_hex(0x6baede), 0);
    lv_obj_set_style_line_color(w->minute_hand, lv_color_hex(0xffba19), 0);
    lv_obj_set_style_line_color(w->second_hand, lv_color_hex(0xbdd731), 0);
    lv_obj_set_style_line_width(w->hour_hand, 3, 0);
    lv_obj_set_style_line_width(w->minute_hand, 3, 0);
    lv_obj_set_style_line_width(w->second_hand, 1, 0);
    lv_obj_set_scrollable(w->hub, false);
    lv_obj_set_style_bg_opa(w->hub, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(w->hub, lv_color_hex(0xbdd731), 0);
    lv_obj_set_style_border_width(w->hub, 2, 0);
    lv_obj_set_style_radius(w->hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(w->hub, 0, 0);
    return view;
}

/* Local coordinates are radial distance and perpendicular offset. */
static void hand_point(lv_point_precise_t *point, int center, double scale,
                       double radial, double across, double angle)
{
    point->x = (lv_value_precise_t)lround(center + scale *
                                        (radial * sin(angle) + across * cos(angle)));
    point->y = (lv_value_precise_t)lround(center + scale *
                                        (-radial * cos(angle) + across * sin(angle)));
}

static void contour(lv_obj_t *hand, lv_point_precise_t points[6], int center,
                    double scale, double length, double angle)
{
    const double radial[6] = {8, length - 9, length, length - 9, 8, 8};
    const double across[6] = {-3, -4, 0, 4, 3, -3};
    for (unsigned i = 0; i < 6; ++i)
        hand_point(&points[i], center, scale, radial[i], across[i], angle);
    lv_line_set_points(hand, points, 6);
}

void clock_view_update(lv_obj_t *view, ClockStyle style, const ClockSample *clock)
{
    if (!view) return;
    ClockWidgets *w = lv_obj_get_user_data(view);
    if (!w) return;
    lv_obj_update_layout(view);
    if (style != DIGITAL_LARGE && style != DIGITAL_DATE && style != ANALOG)
        style = DIGITAL_LARGE;

    LocalDateTime local = {0};
    bool valid = clock && clock->utc_valid && clock->quality != CLOCK_UNAVAILABLE &&
                 app_berlin_local_time(clock->utc_ms, &local) == APP_OK;
    char buffer[32];
    const bool stacked = lv_obj_get_width(view) < 200 &&
                         lv_obj_get_height(view) > lv_obj_get_width(view);
    if (valid) snprintf(buffer, sizeof(buffer), stacked ? "%02u:\n%02u" : "%02u:%02u",
                        local.hour, local.minute);
    else strcpy(buffer, stacked ? "--:\n--" : "--:--");
    lv_label_set_text(w->time, buffer);
    if (valid) snprintf(buffer, sizeof(buffer), "%02u.%02u.%04d", local.day,
                        local.month, local.year);
    else buffer[0] = '\0';
    lv_label_set_text(w->date, buffer);
    lv_label_set_text(w->quality, !valid ? "Uhrzeit nicht verfügbar" :
                      clock->quality == CLOCK_HOLDOVER ? "Zeit nicht synchronisiert" : "");

    bool analog_style = style == ANALOG;
    bool analog = analog_style && valid;
    lv_obj_set_style_text_font(w->time, analog_style ? &katmed_font_26 : &katmed_font_68, 0);
    lv_obj_set_size(w->plate, lv_pct(analog_style ? 84 : 96),
                             lv_pct(analog_style ? 78 : 96));
    lv_obj_set_style_bg_opa(w->plate,
                            analog_style ? LV_OPA_TRANSP : LV_OPA_70, 0);
    lv_obj_set_style_border_width(w->dial, analog_style ? 0 : 2, 0);
    for (int i = 0; i < 12; ++i) {
        lv_obj_set_hidden(w->marks[i], analog_style);
    }
    lv_obj_set_hidden(w->dial, !analog);
    lv_obj_set_hidden(w->time, style == ANALOG && valid);
    lv_obj_set_hidden(w->date, !(style == DIGITAL_DATE && valid));

    lv_obj_update_layout(view);
    int pw = lv_obj_get_content_width(w->plate);
    int ph = lv_obj_get_content_height(w->plate);
    if (pw <= 0 || ph <= 0) return;
    int vw = lv_obj_get_content_width(view);
    int vh = lv_obj_get_content_height(view);
    int diameter = vw < vh ? vw : vh;
    if (analog && diameter > 0) {
        /* Scale against the full visible area, never the smaller digital plate. */
        double scale = diameter / 240.0;
        /* Keep the narrow interior open on the smaller C6 dial as well. */
        int outline_width = diameter < 240 ? 4 : 5;
        lv_obj_set_style_line_width(w->hour_outline, outline_width, 0);
        lv_obj_set_style_line_width(w->minute_outline, outline_width, 0);
        lv_obj_set_size(w->dial, diameter, diameter);
        lv_obj_center(w->dial);
        int c = diameter / 2;
        double turn = 2.0 * 3.14159265358979323846;
        double seconds = local.second;
        contour(w->hour_hand, w->hour_points, c, scale, 58,
                ((local.hour % 12) + (local.minute + seconds / 60.0) / 60.0) * turn / 12.0);
        contour(w->minute_hand, w->minute_points, c, scale, 95,
                (local.minute + seconds / 60.0) * turn / 60.0);
        lv_line_set_points(w->hour_outline, w->hour_points, 6);
        lv_line_set_points(w->minute_outline, w->minute_points, 6);
        hand_point(&w->second_points[0], c, scale, -17, 0, seconds * turn / 60.0);
        hand_point(&w->second_points[1], c, scale, 97, 0, seconds * turn / 60.0);
        lv_line_set_points(w->second_outline, w->second_points, 2);
        lv_line_set_points(w->second_hand, w->second_points, 2);
        int hub_size = (int)lround(10 * scale);
        /* Add only an outer rim; keep the transparent hub opening unchanged. */
        lv_obj_set_size(w->hub_outline, hub_size + 2, hub_size + 2);
        lv_obj_center(w->hub_outline);
        lv_obj_set_size(w->hub, hub_size, hub_size);
        lv_obj_center(w->hub);
    }
    if (analog_style) {
        lv_obj_set_layout(w->plate, LV_LAYOUT_NONE);
        lv_obj_align(w->time, LV_ALIGN_CENTER, 0, 0);
        lv_obj_align(w->quality, LV_ALIGN_BOTTOM_MID, 0, -5);
    } else {
        lv_obj_set_flex_flow(w->plate, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(w->plate, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(w->plate, 2, 0);
    }
}
