#include "katmed_ui.h"
#include "clock_view.h"
#include "katmed_images.h"
#include "katmed_core.h"
#include "lvgl.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int width, height;
    uint16_t *frame;
    int submit_calls, store_calls, network_calls;
    UiSubmitResult result;
    UiRequest last_request;
    UiNetworkStatus network;
} Host;

static uint16_t test_image[320 * 320];
static lv_image_dsc_t image_dsc;

KatmedImageStatus katmed_images_present(lv_obj_t *image, lv_obj_t *fallback,
                                        uint32_t image_id, Orientation orientation)
{
    (void)image_id;
    assert(orientation >= ORIENTATION_0 && orientation <= ORIENTATION_270);
    int width = lv_display_get_horizontal_resolution(lv_display_get_default());
    int height = lv_display_get_vertical_resolution(lv_display_get_default());
    int source_width = orientation == ORIENTATION_90 || orientation == ORIENTATION_270 ?
                       height : width;
    int source_height = orientation == ORIENTATION_90 || orientation == ORIENTATION_270 ?
                        width : height;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int sx = x, sy = y;
            if (orientation == ORIENTATION_90) {
                sx = y;
                sy = source_height - 1 - x;
            } else if (orientation == ORIENTATION_180) {
                sx = source_width - 1 - x;
                sy = source_height - 1 - y;
            } else if (orientation == ORIENTATION_270) {
                sx = source_width - 1 - y;
                sy = x;
            }
            unsigned r = (unsigned)(sx * 15 / source_width);
            unsigned g = (unsigned)(sy * 45 / source_height) + 12;
            unsigned b = ((sx / 24 + sy / 24) & 1) ? 22 : 14;
            test_image[y * width + x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    image_dsc.header.w = (uint16_t)width;
    image_dsc.header.h = (uint16_t)height;
    image_dsc.header.stride = (uint16_t)(width * 2);
    image_dsc.data_size = (uint32_t)(width * height * 2);
    image_dsc.data = (const uint8_t *)test_image;
    lv_image_set_src(image, &image_dsc);
    lv_obj_add_flag(fallback, LV_OBJ_FLAG_HIDDEN);
    return KATMED_IMAGE_OK;
}

static UiSubmitResult submit(void *context, const UiRequest *request)
{
    Host *host = context;
    assert(request);
    host->last_request = *request;
    host->submit_calls++;
    if (request->kind == UI_REQUEST_CONFIRM) host->store_calls++;
    else host->network_calls++;
    return host->result;
}

static bool connected(void *context)
{
    (void)context;
    return false;
}

static void network_status(void *context, UiNetworkStatus *status)
{
    *status = ((Host *)context)->network;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    Host *host = lv_display_get_user_data(display);
    int length = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; ++y) {
        memcpy(host->frame + y * host->width + area->x1,
               pixels + (y - area->y1) * length * 2, (size_t)length * 2);
    }
    lv_display_flush_ready(display);
}

static unsigned status_row_creations, status_row_deletions;
static void status_row_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CHILD_CREATED) ++status_row_creations;
    if (lv_event_get_code(event) == LV_EVENT_DELETE) ++status_row_deletions;
}

static void draw(void)
{
    for (int i = 0; i < 8; ++i) {
        lv_tick_inc(40);
        lv_timer_handler();
    }
}

static void snapshot(const Host *host, const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n%d %d\n255\n", host->width, host->height);
    for (int i = 0; i < host->width * host->height; ++i) {
        uint16_t p = host->frame[i];
        unsigned char rgb[3] = {
            (unsigned char)(((p >> 11) & 31) * 255 / 31),
            (unsigned char)(((p >> 5) & 63) * 255 / 63),
            (unsigned char)((p & 31) * 255 / 31)};
        assert(fwrite(rgb, 1, 3, file) == 3);
    }
    assert(fclose(file) == 0);
}

static void swipe(int x0, int x1, uint64_t t)
{
    ui_touch_sample(false, 0, 0, true, t);
    ui_touch_sample(true, (int16_t)x0, 100, true, t + 10);
    ui_touch_sample(true, (int16_t)x1, 100, true, t + 100);
    ui_touch_sample(false, 0, 0, true, t + 130);
}

static lv_obj_t *find_label(lv_obj_t *node, const char *text)
{
    if (lv_obj_check_type(node, &lv_label_class) &&
        strcmp(lv_label_get_text(node), text) == 0) return node;
    for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i) {
        lv_obj_t *found = find_label(lv_obj_get_child(node, i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_label_prefix(lv_obj_t *node, const char *text)
{
    if (lv_obj_check_type(node, &lv_label_class) &&
        strncmp(lv_label_get_text(node), text, strlen(text)) == 0) return node;
    for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i) {
        lv_obj_t *found = find_label_prefix(lv_obj_get_child(node, i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_alarm_bar(lv_obj_t *screen)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (lv_obj_get_style_bg_opa(child, 0) == 217) return child;
    }
    return NULL;
}

static unsigned visible_alarm_strips(lv_obj_t *screen, lv_obj_t *fallback_bar)
{
    unsigned count = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (child != fallback_bar && lv_obj_get_style_bg_opa(child, 0) == 217 &&
            !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) ++count;
    }
    return count;
}

static lv_obj_t *find_clock_view(lv_obj_t *screen)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (find_label(child, "12:30") || find_label(child, "00:00") ||
            find_label(child, "--:--") || find_label(child,"12:\n30") ||
            find_label(child,"00:\n00") || find_label(child,"--:\n--")) return child;
    }
    return NULL;
}

static bool click_matrix_button(lv_obj_t *node, const char *text)
{
    if (lv_obj_check_type(node, &lv_buttonmatrix_class)) {
        for (uint32_t i = 0; i < 16; ++i) {
            const char *key = lv_buttonmatrix_get_button_text(node, i);
            if (key && strcmp(key, text) == 0) {
                lv_buttonmatrix_set_selected_button(node, i);
                lv_obj_send_event(node, LV_EVENT_VALUE_CHANGED, NULL);
                return true;
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i) {
        if (click_matrix_button(lv_obj_get_child(node, i), text)) return true;
    }
    return false;
}

static lv_obj_t *find_matrix_with_first_button(lv_obj_t *node, const char *text)
{
    if (lv_obj_check_type(node, &lv_buttonmatrix_class)) {
        const char *first = lv_buttonmatrix_get_button_text(node, 0);
        if (first && strcmp(first, text) == 0) return node;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i) {
        lv_obj_t *found = find_matrix_with_first_button(lv_obj_get_child(node, i), text);
        if (found) return found;
    }
    return NULL;
}

static bool pointer_down;
static lv_point_t pointer_point;

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = pointer_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point = pointer_point;
}

static void click_label(lv_obj_t *screen, const char *text)
{
    if ((strcmp(text, "Status") == 0 && ui_current_view() == SETTINGS && !find_label(screen, text)) ||
        (strcmp(text, "Zurück") == 0 && ui_current_view() == UI_VIEW_STATUS)) {
        /* These navigation buttons were removed: exercise the actual edge swipe. */
        static uint64_t nav_time = UINT64_C(10000000);
        nav_time += 1000;
        int height=lv_display_get_vertical_resolution(lv_display_get_default());
        bool down=ui_current_view()==UI_VIEW_STATUS;
        ui_touch_sample(false,0,0,true,nav_time);
        ui_touch_sample(true,100,down?1:height-2,true,nav_time+30);
        ui_touch_sample(true,100,down?100:height-100,true,nav_time+130);
        ui_touch_sample(false,0,0,true,nav_time+230);
        return;
    }
    if (strlen(text) == 1 && strchr("0123456789.,:<", text[0]) &&
        click_matrix_button(screen, text)) return;
    lv_obj_t *caption = find_label(screen, text);
    if (caption) {
        lv_obj_send_event(lv_obj_get_parent(caption), LV_EVENT_CLICKED, NULL);
        return;
    }
    assert(click_matrix_button(screen, text) && "button not found");
}

static unsigned count_objects(lv_obj_t *object)
{
    unsigned count = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        count += count_objects(lv_obj_get_child(object, i));
    return count;
}

static void report_memory(const char *phase)
{
    if (!getenv("KATMED_UI_MEM")) return;
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    fprintf(stderr, "memory %s: used=%zu peak=%zu free=%zu largest=%zu objects=%u\n",
            phase, memory.total_size - memory.free_size, memory.max_used,
            memory.free_size, memory.free_biggest_size,
            count_objects(lv_screen_active()));
}

static void cycle_views(lv_obj_t *screen)
{
    click_label(screen, "Uhrdesign");
    click_label(screen, "Abbrechen");
    click_label(screen, "Medikamente: Stunden");
    click_label(screen, "Abbrechen");
    click_label(screen, "Status");
    click_label(screen, "Zurück");
}

static void enter_keys(lv_obj_t *screen, const char *text)
{
    /* Replace the actual configured value now shown on opening the dialog. */
    for (unsigned i = 0; i < 32; ++i) click_label(screen, "<");
    for (const char *cursor = text; *cursor; ++cursor) {
        char key[] = {*cursor, '\0'};
        click_label(screen, key);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 6);
    Host host = {.width = atoi(argv[1]), .height = atoi(argv[2]),
                 .result = UI_SUBMIT_COMMITTED};
    int orientation_index = atoi(argv[3]);
    const char *dir = argv[4];
    const char *prefix = argv[5];
    assert(host.width > 0 && host.width <= 320 && host.height > 0 && host.height <= 320);
    assert(orientation_index >= ORIENTATION_0 && orientation_index <= ORIENTATION_270);
    host.frame = calloc((size_t)host.width * host.height, 2);
    uint8_t *draw_buffer = malloc((size_t)host.width * 16 * 2);
    assert(host.frame && draw_buffer);
    lv_init();
    lv_display_t *display = lv_display_create(host.width, host.height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, (uint32_t)host.width * 16 * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_user_data(display, &host);
    report_memory("display");

    UiServices services = {.context = &host, .submit = submit, .connected = connected,
                           .network_status = network_status};
    ui_set_services(&services);
    LocalSettings local = {.orientation = (Orientation)orientation_index,
                           .clock_style = DIGITAL_LARGE};
    ClockSample clock = {.monotonic_ms = 0, .domain = 1,
                         .utc_ms = 1791973800000LL, .utc_valid = true,
                         .quality = CLOCK_SYNCHRONIZED};
    AppState state = {0};
    state.alarm_active = true;
    state.image_id = 0;
    state.round.origin = (EventId){.device = 1, .epoch = 1, .sequence = 1};
    state.round.ordinal = 1;
    ui_render(&state, &local, BACKGROUND, &clock);
    report_memory("alarm-start");
    assert(ui_current_view() == ALARM);

    /* A boot contact is discarded until release. */
    ui_touch_sample(true, 100, 100, true, 10);
    ui_touch_sample(false, 0, 0, true, 110);
    assert(host.submit_calls == 0);
    ui_touch_sample(true, 100, 100, true, 200);
    ui_touch_sample(false, 0, 0, true, 300);
    assert(host.submit_calls == 1 && host.store_calls == 1);
    ui_touch_sample(true, 100, 100, true, 310);
    ui_touch_sample(false, 0, 0, true, 380);
    assert(host.submit_calls == 1);
    state.round.ordinal = 2;
    ui_render(&state, &local, ALARM, &clock);
    ui_touch_sample(true, 100, 100, true, 400);
    ui_touch_sample(false, 0, 0, false, 450);
    ui_touch_sample(false, 0, 0, true, 500);
    assert(host.submit_calls == 1);
    ui_touch_sample(true, 100, 100, true, 600);
    ui_touch_sample(false, 0, 0, true, 1300);
    assert(host.submit_calls == 1);
    swipe(60, 130, 1400);
    assert(ui_current_view() == ALARM && host.submit_calls == 1);

    /* The path of a contact matters even if it returns to its start. */
    ui_touch_sample(true, 90, 90, true, 1600);
    ui_touch_sample(true, 170, 90, true, 1660);
    ui_touch_sample(true, 90, 90, true, 1700);
    ui_touch_sample(false, 0, 0, true, 1750);
    assert(host.submit_calls == 1);

    /* A pressed contact belongs to the complete alarm round at touch-down. */
    ui_touch_sample(true, 90, 90, true, 1800);
    state.round.origin.sequence++;
    ui_render(&state, &local, ALARM, &clock);
    ui_touch_sample(false, 0, 0, true, 1880);
    assert(host.submit_calls == 1);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_update_layout(screen);
    lv_obj_t *bar = find_alarm_bar(screen);
    assert(bar);
    int expected_y = host.height == 240 ? 30 : host.height == 320 ? 40 : 22;
    int expected_end = host.height == 240 ? 210 : host.height == 320 ? 280 : 151;
    assert(lv_obj_get_y(bar) == expected_y);
    assert(lv_obj_get_height(bar) == expected_end - expected_y);
    assert(lv_obj_get_style_bg_opa(bar, 0) == 217);
    /* Use a valid core alarm round for time-based presentation. Earlier
     * synthetic rounds intentionally exercise contact boundaries only. */
    AppState display_state = {0};
    SharedSettings display_settings = app_default_settings();
    ClockSample display_start = clock;
    const uint8_t display_catalog[KATMED_CATALOG_BYTES] = {1};
    assert(app_init(&display_state, &display_settings, &display_start,
                    (EventId){1, 1, 1}, 1, display_catalog) == APP_OK);
    clock.monotonic_ms = 2000;
    ui_render(&display_state, &local, ALARM, &clock);
    draw();
    assert(lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN));
    unsigned first_strips = visible_alarm_strips(screen, bar);
    assert(first_strips > 0 && first_strips <= 6);
    char name[128];
    snprintf(name, sizeof(name), "%s_alarm_on", prefix);
    snapshot(&host, dir, name);
    clock.monotonic_ms = 2250;
    ui_render(&display_state, &local, ALARM, &clock);
    draw();
    assert(lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN));
    unsigned second_strips = visible_alarm_strips(screen, bar);
    assert(second_strips < first_strips);
    snprintf(name, sizeof(name), "%s_alarm_off", prefix);
    snapshot(&host, dir, name);

    /* At all tempos the union of the existing six strips must cover exactly
     * the pixel interval, with neither holes nor a seven-step staircase. */
    const uint64_t stages[] = {0, 30 * 60000u, 60 * 60000u, 90 * 60000u, 120 * 60000u};
    for (unsigned stage = 0; stage < 5; ++stage) {
        int previous_edge = -1;
        unsigned distinct = 0;
        for (unsigned ms = 0; ms < 3000; ms += 10) {
            clock.monotonic_ms = stages[stage] + ms;
            AlarmAnimation animation;
            assert(app_alarm_animation(&display_state, &clock, &animation) == APP_OK);
            ui_render(&display_state, &local, ALARM, &clock);
            lv_obj_update_layout(screen);
            int total_height = expected_end - expected_y;
            int edge = animation.phase_permyriad < 5000 ?
                (animation.phase_permyriad * total_height + 4999) / 5000 :
                (animation.phase_permyriad - 5000) * total_height / 5000;
            int first = animation.phase_permyriad < 5000 ? 0 : edge;
            int last = animation.phase_permyriad < 5000 ? edge : total_height;
            int covered = 0;
            for (uint32_t n = 0; n < lv_obj_get_child_count(screen); ++n) {
                lv_obj_t *part = lv_obj_get_child(screen, n);
                if (part == bar || lv_obj_get_style_bg_opa(part, 0) != 217 ||
                    lv_obj_has_flag(part, LV_OBJ_FLAG_HIDDEN)) continue;
                assert(lv_obj_get_y(part) == expected_y + first + covered);
                covered += lv_obj_get_height(part);
            }
            assert(covered == last - first);
            if (edge != previous_edge) ++distinct;
            previous_edge = edge;
        }
        assert(distinct > 80);
    }
    clock.monotonic_ms = 2250;

    /* Alarm beginning during a pressed contact discards its later release. */
    state.alarm_active = false;
    ui_render(&state, &local, ALARM, &clock);
    ui_touch_sample(false, 0, 0, true, 2300);
    ui_touch_sample(true, 90, 90, true, 2310);
    state.alarm_active = true;
    ui_render(&state, &local, BACKGROUND, &clock);
    ui_touch_sample(false, 0, 0, true, 2400);
    assert(host.submit_calls == 1);

    /* None of these service outcomes may be presented as a saved dose. */
    const UiSubmitResult failures[] = {UI_SUBMIT_UNAVAILABLE, UI_SUBMIT_REJECTED,
                                       UI_SUBMIT_PENDING, UI_SUBMIT_STALE};
    const char *messages[] = {"Speichern derzeit nicht verfügbar", "Änderung abgelehnt",
                              "Lokal gespeichert; Gruppe ausstehend", "Vorschau veraltet; erneut prüfen"};
    for (int i = 0; i < 4; ++i) {
        /* PENDING locally saves this round, so each service outcome needs a
         * fresh round before another confirmation request is meaningful. */
        state.round.ordinal++;
        ui_render(&state, &local, ALARM, &clock);
        host.result = failures[i];
        uint64_t t = 2500 + (uint64_t)i * 200;
        ui_touch_sample(true, 90, 90, true, t);
        ui_touch_sample(false, 0, 0, true, t + 80);
        assert(host.submit_calls == i + 2);
        assert(find_label(screen, messages[i]));
        assert(state.alarm_active);
    }
    host.result = UI_SUBMIT_COMMITTED;
    int before_unbound = host.submit_calls;
    ui_set_services(NULL);
    ui_touch_sample(true, 90, 90, true, 3300);
    ui_touch_sample(false, 0, 0, true, 3380);
    assert(host.submit_calls == before_unbound);
    assert(find_label(screen, "Speichern derzeit nicht verfügbar"));
    ui_set_services(&services);

    state.alarm_active = false;
    clock.monotonic_ms = 6000;
    ui_render(&state, &local, ALARM, &clock);
    swipe(60, 130, 6100);
    assert(ui_current_view() == CLOCK);
    ui_render(&state, &local, ui_current_view(), &clock);
    report_memory("clock");
    lv_obj_t *opening_clock = find_clock_view(screen);
    assert(opening_clock);
    lv_timer_handler();
    lv_tick_inc(100);
    lv_timer_handler();
    assert(lv_obj_get_x(opening_clock) == -host.width);
    lv_tick_inc(140);
    lv_timer_handler();
    assert(lv_obj_get_x(opening_clock) > -host.width && lv_obj_get_x(opening_clock) < 0);
    lv_tick_inc(100);
    lv_timer_handler();
    assert(lv_obj_get_x(opening_clock) == 0);
    draw();
    snprintf(name, sizeof(name), "%s_digital_large", prefix);
    snapshot(&host, dir, name);
    swipe(130, 60, 6300);
    assert(ui_current_view() == BACKGROUND);
    swipe(60, 130, 6500);
    assert(ui_current_view() == CLOCK);
    ui_touch_sample(true, 90, 90, true, 6700);
    state.alarm_active = true;
    ui_render(&state, &local, CLOCK, &clock);
    assert(ui_current_view() == ALARM);
    assert(find_clock_view(screen) == NULL);
    int pre_alarm_calls = host.submit_calls;
    ui_touch_sample(false, 0, 0, true, 6800);
    assert(host.submit_calls == pre_alarm_calls);
    state.alarm_active = false;
    ui_render(&state, &local, ALARM, &clock);
    swipe(60, 130, 6900);
    assert(ui_current_view() == CLOCK);

    local.clock_style = DIGITAL_DATE;
    clock.utc_ms = 1792015200000LL; /* 2026-10-15 00:00 in Berlin. */
    ui_render(&state, &local, CLOCK, &clock);
    draw();
    lv_obj_t *date = find_label(screen, "15.10.2026");
    if (!date) {
        lv_obj_t *clock_debug = find_clock_view(screen);
        lv_obj_t *plate_debug = lv_obj_get_child(clock_debug, 0);
        fprintf(stderr, "date actual: %s\n",
                lv_label_get_text(lv_obj_get_child(plate_debug, 1)));
    }
    assert(date);
    snprintf(name, sizeof(name), "%s_digital_date_midnight", prefix);
    snapshot(&host, dir, name);
    Orientation original_orientation = local.orientation;
    local.orientation = (Orientation)((orientation_index + 2) % 4);
    ui_render(&state, &local, CLOCK, &clock);
    lv_obj_t *rotated_clock = find_clock_view(screen);
    assert(rotated_clock);
    assert(!lv_obj_has_flag(rotated_clock, LV_OBJ_FLAG_HIDDEN));
    assert(find_label(screen, "15.10.2026"));
    local.orientation = original_orientation;
    ui_render(&state, &local, CLOCK, &clock);

    local.clock_style = ANALOG;
    clock.utc_ms = 1791973800000LL; /* 12:30 in Berlin. */
    ui_render(&state, &local, CLOCK, &clock);
    draw();
    lv_obj_t *clock_obj = find_clock_view(screen);
    assert(clock_obj);
    lv_obj_t *plate = lv_obj_get_child(clock_obj, 0);
    lv_obj_t *dial = lv_obj_get_child(clock_obj, 1);
    assert(lv_obj_get_style_bg_opa(plate, 0) == LV_OPA_TRANSP);
    assert(lv_obj_get_style_border_width(dial, 0) == 0);
    for (unsigned mark = 0; mark < 12; ++mark)
        assert(lv_obj_has_flag(lv_obj_get_child(dial, mark), LV_OBJ_FLAG_HIDDEN));
    lv_obj_t *hour_outline = lv_obj_get_child(dial, 12);
    lv_obj_t *hour = lv_obj_get_child(dial, 13);
    lv_obj_t *minute_outline = lv_obj_get_child(dial, 14);
    lv_obj_t *minute_hand = lv_obj_get_child(dial, 15);
    lv_obj_t *second_outline = lv_obj_get_child(dial, 16);
    lv_obj_t *second_hand = lv_obj_get_child(dial, 17);
    lv_obj_t *hub_outline = lv_obj_get_child(dial, 18);
    lv_obj_t *hub = lv_obj_get_child(dial, 19);
    assert(lv_obj_get_style_line_width(second_outline, 0) == 3);
    assert(lv_obj_get_style_line_width(second_hand, 0) == 1);
    assert(lv_obj_get_style_line_rounded(second_outline, 0));
    assert(lv_color_to_u32(lv_obj_get_style_line_color(second_outline, 0)) ==
           lv_color_to_u32(lv_color_black()));
    assert(lv_color_to_u32(lv_obj_get_style_border_color(hub_outline, 0)) ==
           lv_color_to_u32(lv_color_black()));
    assert(lv_obj_get_style_border_width(hub, 0) == 2);
    assert(lv_obj_get_style_border_width(hub_outline, 0) == 3);
    assert(lv_obj_get_width(hub_outline) == lv_obj_get_width(hub) + 2);
    assert(!lv_obj_has_flag(second_outline, LV_OBJ_FLAG_CLICKABLE));
    assert(!lv_obj_has_flag(hub_outline, LV_OBJ_FLAG_CLICKABLE));
    assert(lv_obj_get_style_bg_opa(hub_outline, 0) == LV_OPA_TRANSP);
    assert(lv_obj_get_style_bg_opa(hub, 0) == LV_OPA_TRANSP);
    lv_obj_t *outlines[] = {hour_outline, minute_outline};
    for (unsigned h = 0; h < 2; ++h) {
        assert(lv_obj_get_style_line_width(outlines[h], 0) ==
               (lv_obj_get_width(dial) < 240 ? 4 : 5));
        assert(lv_color_to_u32(lv_obj_get_style_line_color(outlines[h], 0)) ==
               lv_color_to_u32(lv_color_black()));
        assert(!lv_obj_has_flag(outlines[h], LV_OBJ_FLAG_CLICKABLE));
    }
    assert(lv_obj_get_width(dial) == (host.width < host.height ? host.width : host.height));
    assert(lv_color_to_u32(lv_obj_get_style_line_color(hour, 0)) ==
           lv_color_to_u32(lv_color_hex(0x6baede)));
    assert(lv_color_to_u32(lv_obj_get_style_line_color(minute_hand, 0)) ==
           lv_color_to_u32(lv_color_hex(0xffba19)));
    assert(lv_color_to_u32(lv_obj_get_style_line_color(second_hand, 0)) ==
           lv_color_to_u32(lv_color_hex(0xbdd731)));
    /* Check projected real-time tips, including seconds within the same minute.
     * This exercises ui_render's cache as well as the actual shared LVGL objects. */
    const unsigned offsets_to_check[] = {0, 15, 30, 45, 59, 60, 11730};
    for (unsigned sample = 0; sample < 7; ++sample) {
        unsigned offset = offsets_to_check[sample];
        unsigned sec = offset % 60;
        unsigned minutes = (30 + offset / 60) % 60;
        unsigned hours = (12 + (30 + offset / 60) / 60) % 12;
        clock.utc_ms = 1791973800000LL + offset * 1000;
        ui_render(&state, &local, CLOCK, &clock);
        draw();
        double center = lv_obj_get_width(dial) / 2;
        double scale = lv_obj_get_width(dial) / 240.0;
        const double angles[] = {(hours + (minutes + sec / 60.0) / 60.0) *
                                 2 * 3.141592653589793 / 12,
                                 (minutes + sec / 60.0) * 2 * 3.141592653589793 / 60,
                                 sec * 2 * 3.141592653589793 / 60};
        lv_obj_t *hands[] = {hour, minute_hand, second_hand};
        const double radii[] = {58, 95, 97};
        for (unsigned h = 0; h < 3; ++h) {
            const lv_point_precise_t *points = lv_line_get_points(hands[h]);
            unsigned tip = h == 2 ? 1 : 2;
            double dx = points[tip].x - center, dy = points[tip].y - center;
            assert(fabs(dx - scale * radii[h] * sin(angles[h])) <= 0.6);
            assert(fabs(dy + scale * radii[h] * cos(angles[h])) <= 0.6);
            assert(hypot(dx, dy) > scale * radii[h] - 1);
            assert(lv_line_get_point_count(hands[h]) == (h == 2 ? 2 : 6));
        }
        for (unsigned h = 0; h < 2; ++h) {
            assert(lv_line_get_points(outlines[h]) == lv_line_get_points(hands[h]));
            assert(lv_line_get_point_count(outlines[h]) == 6);
            assert(lv_obj_get_style_line_width(hands[h], 0) == 3);
        }
        assert(lv_line_get_points(second_outline) == lv_line_get_points(second_hand));
        assert(lv_line_get_point_count(second_outline) == 2);
        const lv_point_precise_t *second_points = lv_line_get_points(second_hand);
        assert(fabs(second_points[0].x - center + scale * 17 * sin(angles[2])) <= .6);
        assert(fabs(second_points[0].y - center - scale * 17 * cos(angles[2])) <= .6);
        /* All three reference hues reach actual rendered pixels. */
        const uint16_t colors[] = {0x6d7b, 0xfdc3, 0xbea6};
        for (unsigned h = 0; h < 3; ++h) {
            unsigned count = 0;
            for (int pixel = 0; pixel < host.width * host.height; ++pixel)
                count += host.frame[pixel] == colors[h];
            assert(count > 5);
        }
        if (offset == 0) {
            /* Compare the real framebuffer with/without just the underlays.
             * Newly black pixels must border both sides of each colored stroke,
             * including the side facing the transparent polygon interior. */
            size_t bytes = (size_t)host.width * host.height * sizeof(uint16_t);
            uint16_t *without_outline = malloc(bytes);
            assert(without_outline);
            for (unsigned h = 0; h < 2; ++h)
                lv_obj_add_flag(outlines[h], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(second_outline, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(hub_outline, LV_OBJ_FLAG_HIDDEN);
            draw();
            memcpy(without_outline, host.frame, bytes);
            for (unsigned h = 0; h < 2; ++h)
                lv_obj_remove_flag(outlines[h], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(second_outline, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(hub_outline, LV_OBJ_FLAG_HIDDEN);
            draw();
            lv_area_t area;
            lv_obj_get_coords(dial, &area);
            for (unsigned h = 0; h < 3; ++h) {
                const lv_point_precise_t *p = lv_line_get_points(hands[h]);
                double dx = p[1].x - p[0].x, dy = p[1].y - p[0].y;
                double length = hypot(dx, dy);
                unsigned sides[2] = {0, 0}, colored = 0;
                for (int y = 0; y < host.height; ++y)
                    for (int x = 0; x < host.width; ++x) {
                        double rx = x - area.x1 - p[0].x;
                        double ry = y - area.y1 - p[0].y;
                        double along = (rx * dx + ry * dy) / length;
                        double across = (rx * dy - ry * dx) / length;
                        if (along < length * .25 || along > length * .75 ||
                            fabs(across) > 4) continue;
                        size_t pixel = (size_t)y * host.width + x;
                        if (host.frame[pixel] == colors[h]) ++colored;
                        uint16_t a = host.frame[pixel], b = without_outline[pixel];
                        /* One-pixel antialiased black rims blend with the image. */
                        if (a != b && (a >> 11) <= (b >> 11) &&
                            ((a >> 5) & 63) <= ((b >> 5) & 63) &&
                            (a & 31) <= (b & 31)) ++sides[across > 0];
                    }

                printf("outline hand=%u black_sides=%u,%u colored=%u\n",
                       h, sides[0], sides[1], colored);
                fflush(stdout);
                assert(sides[0] > 1 && sides[1] > 1 && colored > 5);
            }
            free(without_outline);
            /* Interior of the long minute outline must reveal the background. */
            int x = host.width / 2;
            int y = host.height / 2 + (int)lround(60 * scale);
            if (lv_obj_get_width(dial) >= 240) {
                assert(host.frame[y * host.width + x] == test_image[y * host.width + x]);
            } else {
                /* On the 172px dial, rounding moves the narrow open center by
                 * a pixel. Require actual untouched interior pixels, not merely
                 * a transparent style property, in its central strip. */
                unsigned clear = 0;
                for (int radial = 30; radial < 85; ++radial)
                    for (int across = -1; across <= 1; ++across) {
                        int py = host.height / 2 + (int)lround(radial * scale);
                        size_t pixel = (size_t)py * host.width + x + across;
                        clear += host.frame[pixel] == test_image[pixel];
                    }
                assert(clear > 1);
            }
        }
        if (offset == 15) {
            /* Isolate only the new seconds underlays on the real framebuffer.
             * The forward arm, counterarm and transparent hub are all covered. */
            size_t bytes = (size_t)host.width * host.height * sizeof(uint16_t);
            uint16_t *without_seconds = malloc(bytes);
            assert(without_seconds);
            lv_obj_add_flag(second_outline, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(hub_outline, LV_OBJ_FLAG_HIDDEN);
            draw();
            memcpy(without_seconds, host.frame, bytes);
            lv_obj_remove_flag(second_outline, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(hub_outline, LV_OBJ_FLAG_HIDDEN);
            draw();
            lv_area_t area;
            lv_obj_get_coords(dial, &area);
            const lv_point_precise_t *p = lv_line_get_points(second_hand);
            for (unsigned arm = 0; arm < 2; ++arm) {
                unsigned sides[2] = {0, 0}, colored = 0;
                int first = (int)lround((arm ? -15 : 25) * scale);
                int last = (int)lround((arm ? -10 : 85) * scale);
                for (int radial = first; radial <= last; ++radial)
                    for (int across = -2; across <= 2; ++across) {
                        int x = area.x1 + (int)center + radial;
                        int y = area.y1 + (int)p[0].y + across;
                        size_t pixel = (size_t)y * host.width + x;
                        uint16_t a = host.frame[pixel], b = without_seconds[pixel];
                        colored += a == colors[2];
                        if (a != b && (a >> 11) <= (b >> 11) &&
                            ((a >> 5) & 63) <= ((b >> 5) & 63) &&
                            (a & 31) <= (b & 31)) ++sides[across > 0];
                    }
                printf("seconds arm=%u black_sides=%u,%u colored=%u\n",
                       arm, sides[0], sides[1], colored);
                assert(sides[0] > 0 && sides[1] > 0 && colored > 0);
            }
            for (unsigned end = 0; end < 2; ++end) {
                int x = area.x1 + (int)p[end].x + (end ? 1 : -1);
                int y = area.y1 + (int)p[end].y;
                size_t pixel = (size_t)y * host.width + x;
                assert(host.frame[pixel] != without_seconds[pixel]);
                assert(host.frame[pixel] == 0); /* Rounded black endpoint cap. */
            }
            lv_area_t hub_area;
            lv_obj_get_coords(hub, &hub_area);
            unsigned rim = 0, clear = 0, green = 0;
            for (int y = hub_area.y1 - 1; y <= hub_area.y2 + 1; ++y)
                for (int x = hub_area.x1 - 1; x <= hub_area.x2 + 1; ++x) {
                    size_t pixel = (size_t)y * host.width + x;
                    rim += host.frame[pixel] != without_seconds[pixel];
                    green += host.frame[pixel] == colors[2];
                    if (x > hub_area.x1 + 2 && x < hub_area.x2 - 2 &&
                        y > hub_area.y1 + 2 && y < hub_area.y2 - 2)
                        clear += host.frame[pixel] == test_image[pixel];
                }
            printf("seconds hub rim=%u green=%u clear=%u\n", rim, green, clear);
            assert(rim > 0 && green > 0);
            /* Isolate the ring opening from the unchanged hands behind it. */
            lv_obj_t *behind_hub[] = {hour_outline, hour, minute_outline,
                                     minute_hand, second_outline, second_hand};
            for (unsigned h = 0; h < 6; ++h)
                lv_obj_add_flag(behind_hub[h], LV_OBJ_FLAG_HIDDEN);
            draw();
            clear = 0;
            for (int y = hub_area.y1 + 2; y <= hub_area.y2 - 2; ++y)
                for (int x = hub_area.x1 + 2; x <= hub_area.x2 - 2; ++x) {
                    size_t pixel = (size_t)y * host.width + x;
                    clear += host.frame[pixel] == test_image[pixel];
                }
            printf("seconds isolated hub clear=%u\n", clear);
            assert(clear > 0);
            for (unsigned h = 0; h < 6; ++h)
                lv_obj_remove_flag(behind_hub[h], LV_OBJ_FLAG_HIDDEN);
            draw();
            free(without_seconds);
            snprintf(name, sizeof(name), "%s_analog_123015", prefix);
            snapshot(&host, dir, name);
        }
    }

    clock.utc_valid = false;
    clock.quality = CLOCK_UNAVAILABLE;
    ui_render(&state, &local, CLOCK, &clock);
    draw();
    assert(find_label(screen, "--:--") || find_label(screen,"--:\n--"));
    assert(find_label(screen, "Uhrzeit nicht verfügbar"));
    assert(lv_obj_get_style_bg_opa(plate, 0) == LV_OPA_TRANSP);
    assert(lv_obj_get_style_border_width(dial, 0) == 0);
    /* Re-enter with the same unavailable time and persisted analog style:
     * a new view must apply style even when every cached value is unchanged. */
    ui_render(&state, &local, SETTINGS, &clock);
    assert(find_clock_view(screen) == NULL);
    ui_render(&state, &local, CLOCK, &clock);
    clock_obj = find_clock_view(screen);
    plate = lv_obj_get_child(clock_obj, 0);
    dial = lv_obj_get_child(clock_obj, 1);
    assert(lv_obj_get_style_bg_opa(plate, 0) == LV_OPA_TRANSP);
    assert(lv_obj_get_style_border_width(dial, 0) == 0);
    assert(lv_obj_has_flag(dial, LV_OBJ_FLAG_HIDDEN));
    snprintf(name, sizeof(name), "%s_clock_unavailable", prefix);
    snapshot(&host, dir, name);
    clock.utc_valid = true;
    clock.quality = CLOCK_HOLDOVER;
    ui_render(&state, &local, CLOCK, &clock);
    draw();
    assert(find_label(screen, "Zeit nicht synchronisiert"));
    snprintf(name, sizeof(name), "%s_clock_holdover", prefix);
    snapshot(&host, dir, name);

    swipe(130, 60, 7100);
    assert(ui_current_view() == BACKGROUND);
    swipe(130, 60, 7300);
    assert(ui_current_view() == SETTINGS);
    ui_render(&state, &local, SETTINGS, &clock);
    report_memory("settings");
    draw();
    snprintf(name, sizeof(name), "%s_settings", prefix);
    snapshot(&host, dir, name);
    lv_obj_t *placeholder = find_label(screen,
                                       "Lade Bilder von URL/Github\nSpäter verfügbar");
    assert(placeholder);
    lv_obj_t *import = lv_obj_get_parent(placeholder);
    assert(lv_obj_has_state(import, LV_STATE_DISABLED));
    int previous = host.submit_calls;
    int old_network = host.network_calls;
    int old_store = host.store_calls;
    lv_obj_send_event(import, LV_EVENT_CLICKED, NULL);
    assert(host.submit_calls == previous && host.network_calls == old_network &&
           host.store_calls == old_store);
    assert(ui_current_view() == SETTINGS);
    const char *angle_names[] = {"0°", "90°", "180°", "270°"};
    for (int i = 0; i < 4; ++i) {
        click_label(screen, angle_names[i]);
        assert(host.last_request.kind == UI_REQUEST_SET_ORIENTATION);
        assert(host.last_request.value.orientation == (Orientation)i);
    }
    const char *rotation_names[] = {"Aus", "Nach Namen", "Zufall"};
    for (int i = 0; i < 3; ++i) {
        click_label(screen, rotation_names[i]);
        assert(host.last_request.kind == UI_REQUEST_SET_ROTATION_MODE);
        assert(host.last_request.value.rotation == (RotationMode)i);
    }

    int before_style = host.submit_calls;
    click_label(screen, "Uhrdesign");
    report_memory("style-dialog");
    draw();
    snprintf(name, sizeof(name), "%s_style_dialog", prefix);
    snapshot(&host, dir, name);
    click_label(screen, "Digital groß");
    assert(host.submit_calls == before_style);
    click_label(screen, "Abbrechen");
    assert(host.submit_calls == before_style && local.clock_style == ANALOG);
    click_label(screen, "Uhrdesign");
    click_label(screen, "Digital mit Datum");
    host.result = UI_SUBMIT_REJECTED;
    click_label(screen, "Speichern");
    assert(host.submit_calls == before_style + 1);
    assert(find_label(screen, "Änderung abgelehnt"));
    assert(local.clock_style == ANALOG);
    host.result = UI_SUBMIT_COMMITTED;
    click_label(screen, "Speichern");
    assert(host.submit_calls == before_style + 2);
    assert(host.last_request.kind == UI_REQUEST_SET_CLOCK_STYLE);
    assert(host.last_request.value.clock_style == DIGITAL_DATE);
    assert(local.clock_style == ANALOG);
    lv_obj_t *saved_feedback = find_label(screen, "Gespeichert");
    lv_obj_t *rebuilt_menu = lv_obj_get_parent(find_label(screen, "Einstellungen"));
    assert(saved_feedback && rebuilt_menu);
    assert(!lv_obj_has_flag(saved_feedback, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_index(saved_feedback) > lv_obj_get_index(rebuilt_menu));
    draw();
    snprintf(name, sizeof(name), "%s_style_saved_feedback", prefix);
    snapshot(&host, dir, name);

    clock.quality = CLOCK_SYNCHRONIZED;
    clock.utc_ms = 1774744200000LL; /* 2026-03-29 01:30 CET. */
    ui_render(&state, &local, SETTINGS, &clock);
    click_label(screen, "Nächste Fälligkeit HH:MM");
    report_memory("time-keypad");
    click_label(screen, "0");
    click_label(screen, "2");
    /* The HHMM keypad inserts its separator after digit entry. */
    click_label(screen, "3");
    click_label(screen, "0");
    lv_obj_t *skip = find_label_prefix(screen, "02:30 heute nicht vorhanden");
    assert(skip && strstr(lv_label_get_text(skip), "30.03.2026"));
    draw();
    snprintf(name, sizeof(name), "%s_dst_gap_preview", prefix);
    snapshot(&host, dir, name);
    click_label(screen, "Abbrechen");
    clock.utc_ms = 1791973800000LL;
    ui_render(&state, &local, SETTINGS, &clock);
    click_label(screen, "Nächste Fälligkeit HH:MM");
    click_label(screen, "1");
    click_label(screen, "2");
    /* The HHMM keypad inserts its separator after digit entry. */
    click_label(screen, "3");
    click_label(screen, "0");
    assert(find_label_prefix(screen, "Wirksam:"));
    draw();
    snprintf(name, sizeof(name), "%s_hhmm_preview", prefix);
    snapshot(&host, dir, name);
    click_label(screen, "Speichern");
    assert(host.last_request.kind == UI_REQUEST_SET_NEXT_LOCAL_TIME);
    assert(host.last_request.value.local_time.hour == 12);
    assert(host.last_request.value.local_time.minute == 30);
    assert(host.last_request.value.local_time.preview.effective_utc.valid);
    state.has_last_confirmation = true;
    state.last_confirmation.utc_valid = true;
    state.last_confirmation.utc_ms = 1791973800000LL;
    state.deadline.effective_utc = (UtcInstant){true, 1792015200000LL};
    state.confirmation_pending_sync = true;
    ui_render(&state, &local, SETTINGS, &clock);
    click_label(screen, "Status");
    report_memory("status");
    lv_obj_t *feedback = find_label(screen, "Gespeichert");
    assert(feedback && lv_obj_has_flag(feedback, LV_OBJ_FLAG_HIDDEN));
    assert(find_label_prefix(screen, "Letzte Gabe: 14.10.2026"));
    assert(find_label_prefix(screen, "Nächster Alarm: 15.10.2026"));
    assert(find_label(screen, "Bestätigung noch nicht synchronisiert"));
    draw();
    snprintf(name, sizeof(name), "%s_status", prefix);
    snapshot(&host, dir, name);
    lv_obj_t *status_panel = lv_obj_get_parent(find_label_prefix(screen, "Letzte Gabe:"));
    assert(!find_label(status_panel, "Zurück"));
    click_label(screen, "Zurück");
    click_label(screen, "Bilder: Stunden");
    assert(find_label(screen, "Positive Stundenzahl eingeben"));
    local.orientation = (Orientation)((orientation_index + 2) % 4);
    ui_render(&state, &local, SETTINGS, &clock);
    assert(find_label(screen, "Eingabe nach Drehung verworfen"));
    assert(!find_label(screen, "Positive Stundenzahl eingeben"));
    click_label(screen, "Medikamente: Stunden");
    assert(find_label(screen, "Positive Stundenzahl eingeben"));
    int calls_before_invalid = host.submit_calls;
    click_label(screen, "Speichern");
    assert(host.submit_calls == calls_before_invalid);
    click_label(screen, "Abbrechen");

    lv_mem_monitor_t before_cycles, after_cycles;
    cycle_views(screen);
    lv_mem_monitor(&before_cycles);
    for (int i = 0; i < 50; ++i) cycle_views(screen);
    lv_mem_monitor(&after_cycles);
    if (after_cycles.free_size < before_cycles.free_size)
        fprintf(stderr, "cycle memory before=%zu after=%zu delta=%zu\n",
                before_cycles.free_size, after_cycles.free_size,
                before_cycles.free_size - after_cycles.free_size);
    assert(after_cycles.free_size >= before_cycles.free_size);

    /* A direct view switch must free the old menu before creating the clock. */
    ui_render(&state, &local, CLOCK, &clock);
    assert(ui_current_view() == CLOCK && find_clock_view(screen));
    assert(!find_label(screen, "Einstellungen"));
    ui_render(&state, &local, SETTINGS, &clock);
    assert(ui_current_view() == SETTINGS && find_label(screen, "Einstellungen"));
    click_label(screen, "Uhrdesign");
    assert(find_label(screen, "Uhrdesign wählen"));
    ui_render(&state, &local, CLOCK, &clock);
    assert(ui_current_view() == CLOCK && find_clock_view(screen));
    assert(!find_label(screen, "Uhrdesign wählen"));
    ui_render(&state, &local, SETTINGS, &clock);
    assert(find_label(screen, "Einstellungen"));
    const char *hour_values[] = {"1.5", "1,5", "0", "999999999999999999999999",
                                 "1..2"};
    for (unsigned i = 0; i < 5; ++i) {
        int before = host.submit_calls;
        click_label(screen, "Bilder: Stunden");
        enter_keys(screen, hour_values[i]);
        click_label(screen, "Speichern");
        if (i < 2) {
            assert(host.submit_calls == before + 1);
            assert(host.last_request.value.seconds == 5400);
        } else {
            if (host.submit_calls != before)
                fprintf(stderr, "invalid hours '%s' reached service\n", hour_values[i]);
            assert(host.submit_calls == before);
            click_label(screen, "Abbrechen");
        }
    }
    lv_obj_t *angle_matrix = find_matrix_with_first_button(screen, "0°");
    lv_obj_t *rotation_matrix = find_matrix_with_first_button(screen, "Aus");
    assert(angle_matrix && rotation_matrix);
    for (uint32_t i = 0; i < 4; ++i)
        assert(lv_buttonmatrix_has_button_ctrl(angle_matrix, i,
               LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT));
    for (uint32_t i = 0; i < 3; ++i)
        assert(lv_buttonmatrix_has_button_ctrl(rotation_matrix, i,
               LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT));

    lv_indev_t *pointer = lv_indev_create();
    assert(pointer);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, display);
    lv_indev_set_read_cb(pointer, pointer_read);
    lv_obj_scroll_to_view(angle_matrix, LV_ANIM_OFF);
    lv_obj_update_layout(screen);
    lv_area_t matrix_area;
    lv_obj_get_coords(angle_matrix, &matrix_area);
    pointer_point.x = (matrix_area.x1 + matrix_area.x2) / 2;
    pointer_point.y = matrix_area.y1 + 16;
    assert(pointer_point.x >= 0 && pointer_point.x < host.width);
    assert(pointer_point.y >= 0 && pointer_point.y < host.height);
    int calls_before_pointer = host.submit_calls;
    pointer_down = true;
    lv_indev_read(pointer);
    assert(host.submit_calls == calls_before_pointer);
    lv_tick_inc(1000);
    lv_indev_read(pointer);
    assert(host.submit_calls == calls_before_pointer);
    pointer_down = false;
    lv_indev_read(pointer);
    assert(host.submit_calls == calls_before_pointer + 1);
    assert(host.last_request.kind == UI_REQUEST_SET_ORIENTATION);
    assert(host.last_request.value.orientation == ORIENTATION_0);
    pointer_down = true;
    lv_indev_read(pointer);
    assert(host.submit_calls == calls_before_pointer + 1);
    lv_indev_reset(pointer, NULL);
    pointer_down = false;
    lv_indev_read(pointer);
    assert(host.submit_calls == calls_before_pointer + 1);
    lv_indev_delete(pointer);
    draw();
    report_memory("final");

    /* Settings right -> home, then vertical home gesture reuses status. */
    ui_render(&state, &local, SETTINGS, &clock);
    swipe(60, 140, 50000);
    assert(ui_current_view() == BACKGROUND);
    ui_render(&state, &local, BACKGROUND, &clock);
    assert(!find_label(screen, "Einstellungen"));
    ui_touch_sample(true, 100, 180, true, 51000);
    ui_touch_sample(true, 100, 80, true, 51100);
    ui_touch_sample(false, 0, 0, true, 51200);
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Status"));
    assert(find_label(screen, "WLAN: nicht konfiguriert"));
    assert(find_label(screen, "Mesh-Funkkanal: inaktiv"));
    host.network = (UiNetworkStatus){.configured = true, .connected = true};
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "DNS-IP: unbekannt"));
    assert(find_label(screen, "Zeitserver-IP (letzter lokaler Sync): unbekannt"));
    host.network = (UiNetworkStatus){.configured = true, .connected = true,
        .dns_ip = "192.0.2.53", .sync_server_ip = "203.0.113.1",
        .mesh_active = true, .mesh_channel = 6,
        .mesh_current_count = 2, .mesh_history_count = 3, .mesh_quality = 16,
        .mesh_current_valid = true, .mesh_history_complete = true,
        .station_ip = "192.0.2.10", .gateway = "192.0.2.1", .ap_ip = "192.0.2.129"};
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh-Funkkanal: 6"));
    assert(find_label(screen, "IP: 192.0.2.10"));
    assert(find_label(screen, "Gateway: 192.0.2.1"));
    assert(find_label(screen, "AP-IP: 192.0.2.129"));
    assert(find_label(screen, "DNS-IP: 192.0.2.53"));
    assert(find_label(screen, "Zeitserver-IP (letzter lokaler Sync): 203.0.113.1"));
    assert(find_label(screen, "Mesh aktuell (inkl. Gerät): 2"));
    assert(find_label(screen, "Mesh bisher erfasst (ab Einführung): 3"));
    host.network.mesh_quality = 2;
    host.network.mesh_history_complete = false;
    clock.monotonic_ms += 1000;ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh bisher erfasst (ab Einführung): mindestens 3 (Speicherfehler)"));
    host.network.mesh_quality = 1;
    clock.monotonic_ms += 1000;ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh bisher erfasst (ab Einführung): mindestens 3 (Speicher voll)"));
    host.network.mesh_quality = 4;
    clock.monotonic_ms += 1000;ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh aktuell (inkl. Gerät): mindestens 2 (Wiederaufbau)"));
    assert(find_label(screen, "Mesh bisher erfasst (ab Einführung): mindestens 3 (Abgleich)"));
    host.network.mesh_quality = 8;
    clock.monotonic_ms += 1000;ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh aktuell (inkl. Gerät): mindestens 2 (unvollständig)"));
    host.network.mesh_current_valid = false;
    clock.monotonic_ms += 1000;ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh aktuell (inkl. Gerät): unbekannt"));
    host.network.mesh_quality = 16;host.network.mesh_current_valid = true;
    host.network.mesh_history_complete = true;
    clock.monotonic_ms += 1000;ui_render(&state, &local, ui_current_view(), &clock);
    lv_obj_t *server_label = find_label(screen, "Zeitserver-IP (letzter lokaler Sync): 203.0.113.1");
    lv_obj_update_layout(screen);
    assert(lv_obj_get_width(server_label) <= host.width - (host.width == 240 ? 100 : 20));
    lv_obj_scroll_to_view(server_label, LV_ANIM_OFF);
    lv_refr_now(display);
    snprintf(name, sizeof(name), "%s-status-servers", prefix);
    snapshot(&host, dir, name);
    /* An unchanged one-second refresh preserves rows, text storage and scroll. */
    status_panel = lv_obj_get_parent(server_label);
    status_row_creations = status_row_deletions = 0;
    lv_obj_add_event_cb(status_panel, status_row_event, LV_EVENT_CHILD_CREATED, NULL);
    lv_obj_add_event_cb(server_label, status_row_event, LV_EVENT_DELETE, NULL);
    int32_t scroll_before = lv_obj_get_scroll_y(status_panel);
    const char *server_text = lv_label_get_text(server_label);
    uint32_t rows_before = lv_obj_get_child_count(status_panel);
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, ui_current_view(), &clock);
    lv_obj_update_layout(screen);
    assert(find_label(screen, "Zeitserver-IP (letzter lokaler Sync): 203.0.113.1") == server_label);
    assert(lv_label_get_text(server_label) == server_text);
    assert(lv_obj_get_child_count(status_panel) == rows_before);
    assert(lv_obj_get_scroll_y(status_panel) == scroll_before);
    assert(status_row_creations == 0 && status_row_deletions == 0);
    printf("status_unchanged_refresh rows=%u created=%u deleted=%u scroll=%d\n",
           rows_before, status_row_creations, status_row_deletions, (int)scroll_before);
    host.network.connected = false; /* Even a faulty provider's stale STA strings stay hidden. */
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "WLAN: getrennt"));
    assert(!find_label(screen, "IP: 192.0.2.10"));
    assert(!find_label(screen, "Gateway: 192.0.2.1"));
    assert(find_label(screen, "Gateway: nicht verfügbar"));
    assert(!find_label(screen, "DNS-IP: 192.0.2.53"));
    assert(!find_label(screen, "Zeitserver-IP (letzter lokaler Sync): 203.0.113.1"));
    assert(find_label(screen, "DNS-IP: unbekannt"));
    assert(find_label(screen, "Zeitserver-IP (letzter lokaler Sync): unbekannt"));
    host.network.mesh_channel = 0;
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh-Funkkanal: unbekannt"));
    host.network.mesh_active = false;
    host.network.mesh_channel = 6; /* Inactive must suppress a stale channel. */
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh-Funkkanal: inaktiv"));
    click_label(screen, "Zurück");
    assert(ui_current_view() == BACKGROUND && !find_label(screen, "Status"));
    assert(!find_label(screen, "Einstellungen"));

    lv_display_delete(display);
    lv_deinit();
    free(draw_buffer);
    free(host.frame);
    printf("layout=%dx%d orientation=%d touch/store=%d network=%d artifacts=14\n",
           host.width, host.height, orientation_index * 90,
           host.store_calls, host.network_calls);
    return 0;
}
