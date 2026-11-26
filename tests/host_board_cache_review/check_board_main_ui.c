#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "katmed_images.h"
#include "katmed_ui.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"

/* Product paths stay intact. Only physical I2C, the panel and service sinks
 * are replaced; LVGL reads the product board callback and emits real clicks. */
static bool boot_input_ready; /* Host-only stand-in for board_deinit's ESP task state. */
#include "../../firmware/components/board/board.c"
#undef pdMS_TO_TICKS
#define KATMED_FULL_STACK 1
#define KATMED_WITH_CORE 1
#include "../../firmware/main/main.c"

static uint64_t fake_ms;
static uint8_t register_selected;
static bool io_failure;
static bool lock_failure;
static bool raw_down;
static int raw_x, raw_y;
static unsigned confirms, shared_writes, local_writes;
static unsigned render_trace_begin[KATMED_RENDER_INNER_COUNT];
static unsigned render_trace_end[KATMED_RENDER_INNER_COUNT];
static bool render_trace_open[KATMED_RENDER_INNER_COUNT];
static uint8_t render_trace_lock_success;
static unsigned brightness_submits, brightness_previews;
static uint16_t submitted_brightness;
static uint8_t preview_brightness_value;
static uint8_t stored_brightness = 37;
static unsigned slider_releases, slider_press_losses, slider_resets;
static uint16_t draw_buffer[320 * 16];
static unsigned internal_free_reads, dma_free_reads;
size_t heap_caps_get_free_size(uint32_t caps)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    assert(caps == internal || caps == (internal | MALLOC_CAP_DMA));
    /* Synthetic total free sizes, each at least the unchanged largest block.
     * They model separate capability views, not device memory measurements. */
    if (caps & MALLOC_CAP_DMA) { ++dma_free_reads; return 1u << 20; }
    ++internal_free_reads;
    return 2u << 20;
}
size_t heap_caps_get_largest_free_block(uint32_t caps)
{ (void)caps; return 1u << 20; }
static AppState state;
static LocalSettings local;

int64_t esp_timer_get_time(void) { return (int64_t)fake_ms * 1000; }
static esp_err_t read_register(uint8_t reg, void *out, size_t length)
{
    if (io_failure) return -2;
    uint8_t *bytes = out;
    memset(bytes, 0, length);
#if CONFIG_IDF_TARGET_ESP32C3
    if (reg == 2 && length == 1) bytes[0] = raw_down ? 1 : 0;
    else if (reg == 3 && length == 4) {
        bytes[0] = (uint8_t)(raw_x >> 8);
        bytes[1] = (uint8_t)raw_x;
        bytes[2] = (uint8_t)(raw_y >> 8);
        bytes[3] = (uint8_t)raw_y;
    } else assert(0);
#else
    assert(reg == 1 && length == 14);
    bytes[1] = raw_down ? 1 : 0;
    bytes[2] = (uint8_t)(raw_x >> 8);
    bytes[3] = (uint8_t)raw_x;
    bytes[4] = (uint8_t)(raw_y >> 8);
    bytes[5] = (uint8_t)raw_y;
#endif
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
    const void *send, size_t send_length, void *receive, size_t receive_length, int timeout)
{
    (void)device; (void)timeout;
    assert(send_length == 1);
    return read_register(*(const uint8_t *)send, receive, receive_length);
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
    const void *send, size_t send_length, int timeout)
{
    (void)device; (void)timeout;
    assert(send_length == 1);
    register_selected = *(const uint8_t *)send;
    return io_failure ? -2 : ESP_OK;
}
esp_err_t i2c_master_receive(i2c_master_dev_handle_t device,
    void *receive, size_t receive_length, int timeout)
{
    (void)device; (void)timeout;
    return read_register(register_selected, receive, receive_length);
}
void katmed_config_render_inner(KatmedRenderInnerPhase phase, bool begin,
                                uint8_t outcome)
{
    assert(phase == KATMED_RENDER_INNER_LVGL_LOCK ||
           phase == KATMED_RENDER_INNER_UI_RENDER);
    if (begin) {
        assert(outcome == 0 && !render_trace_open[phase]);
        if (phase == KATMED_RENDER_INNER_UI_RENDER)
            assert(render_trace_lock_success == 1);
        render_trace_open[phase] = true;
        ++render_trace_begin[phase];
    } else {
        assert(render_trace_open[phase]);
        assert(outcome <= (phase == KATMED_RENDER_INNER_LVGL_LOCK ? 1 : 0));
        render_trace_open[phase] = false;
        ++render_trace_end[phase];
        if (phase == KATMED_RENDER_INNER_LVGL_LOCK)
            render_trace_lock_success = outcome;
    }
}
bool lvgl_port_lock(unsigned timeout) { assert(timeout == 100); return !lock_failure; }
void lvgl_port_unlock(void) {}
int esp_lcd_panel_set_gap(void) { return 0; }
int esp_lcd_panel_swap_xy(void) { return 0; }
int esp_lcd_panel_mirror(void) { return 0; }
KatmedImageStatus katmed_images_present(lv_obj_t *image, lv_obj_t *fallback,
    uint32_t id, Orientation orientation)
{
    (void)id; (void)orientation;
    lv_obj_set_hidden(image, true);
    lv_obj_set_hidden(fallback, false);
    return KATMED_IMAGE_OK;
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels;
    lv_display_flush_ready(display);
}
static UiSubmitResult submit(void *context, const UiRequest *request)
{
    (void)context;
    switch (request->kind) {
    case UI_REQUEST_CONFIRM: confirms++; return UI_SUBMIT_PENDING;
    case UI_REQUEST_SET_BRIGHTNESS:
        brightness_submits++;
        submitted_brightness = request->value.display_value;
        return UI_SUBMIT_QUEUED;
    case UI_REQUEST_SET_ORIENTATION:
    case UI_REQUEST_SET_CLOCK_STYLE:
        local_writes++;
        return UI_SUBMIT_COMMITTED;
    default:
        shared_writes++;
        return UI_SUBMIT_UNAVAILABLE;
    }
}
static bool preview_brightness(void *context, uint8_t value)
{
    (void)context;
    brightness_previews++;
    preview_brightness_value = value;
    return true;
}
static void read_display(void *context, KatmedDisplaySettings *settings)
{
    (void)context;
    *settings = katmed_display_defaults();
    settings->brightness = stored_brightness;
}

static void advance_time(void)
{
    fake_ms += 50;
    lv_tick_inc(50);
}
static BoardTouch cache_touch(bool down, int screen_x, int screen_y, bool fail_io)
{
    advance_time();
    raw_down = down;
#if CONFIG_IDF_TARGET_ESP32C6
    raw_x = LCD_WIDTH - 1 - screen_x;
#else
    raw_x = screen_x;
#endif
    raw_y = screen_y;
    io_failure = fail_io;
    BoardTouch sample;
    BoardResult result = board_read_touch(&sample);
    assert(result == (fail_io ? BOARD_IO_ERROR : BOARD_OK));
    return sample;
}

static void render_touch(const BoardTouch *sample, bool valid, bool fail_main_lock)
{
    lock_failure = fail_main_lock;
    const unsigned lock_begin = render_trace_begin[KATMED_RENDER_INNER_LVGL_LOCK];
    const unsigned lock_end = render_trace_end[KATMED_RENDER_INNER_LVGL_LOCK];
    const unsigned ui_begin = render_trace_begin[KATMED_RENDER_INNER_UI_RENDER];
    ClockSample clock = {.monotonic_ms = fake_ms, .domain = 1,
        .utc_ms = 1791973800000LL, .utc_valid = true,
        .quality = CLOCK_SYNCHRONIZED};
    assert(render_ui(&state, &local, &clock, sample, valid, false) == !fail_main_lock);
    assert(render_trace_begin[KATMED_RENDER_INNER_LVGL_LOCK] == lock_begin + 1);
    assert(render_trace_end[KATMED_RENDER_INNER_LVGL_LOCK] == lock_end + 1);
    assert(!render_trace_open[KATMED_RENDER_INNER_LVGL_LOCK] &&
           !render_trace_open[KATMED_RENDER_INNER_UI_RENDER]);
    assert(render_trace_lock_success == (uint8_t)!fail_main_lock);
    assert(render_trace_begin[KATMED_RENDER_INNER_UI_RENDER] ==
           render_trace_end[KATMED_RENDER_INNER_UI_RENDER]);
    if (fail_main_lock)
        assert(render_trace_begin[KATMED_RENDER_INNER_UI_RENDER] == ui_begin);
    lock_failure = false;
}

static void step(bool down, int screen_x, int screen_y, bool fail_io,
                 bool fail_main_lock, bool poll_indev)
{
    BoardTouch sample = cache_touch(down, screen_x, screen_y, fail_io);
    render_touch(&sample, !fail_io, fail_main_lock);
    if (poll_indev) lv_indev_read(lv_touch);
}
static lv_obj_t *find_matrix(lv_obj_t *parent, const char *text, uint32_t *button)
{
    if (lv_obj_check_type(parent, &lv_buttonmatrix_class)) {
        const lv_buttonmatrix_t *matrix = (const lv_buttonmatrix_t *)parent;
        for (uint32_t i = 0; i < matrix->btn_cnt; ++i) {
            if (strcmp(lv_buttonmatrix_get_button_text(parent, i), text) == 0) {
                *button = i;
                return parent;
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *found = find_matrix(lv_obj_get_child(parent, i), text, button);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *find_slider(lv_obj_t *parent)
{
    if (lv_obj_check_type(parent, &lv_slider_class)) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *found = find_slider(lv_obj_get_child(parent, i));
        if (found) return found;
    }
    return NULL;
}
static void observe_slider(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
    case LV_EVENT_RELEASED: ++slider_releases; break;
    case LV_EVENT_PRESS_LOST: ++slider_press_losses; break;
    case LV_EVENT_INDEV_RESET: ++slider_resets; break;
    default: break;
    }
}

/* Choose the intended fraction of the visible track before any input. */
static int brightness_x(lv_obj_t *slider, unsigned percent)
{
    lv_area_t area;
    lv_obj_get_coords(slider, &area);
    const int left = lv_obj_get_style_pad_left(slider, LV_PART_MAIN);
    const int right = lv_obj_get_style_pad_right(slider, LV_PART_MAIN);
    const int width = lv_obj_get_width(slider) - left - right;
    assert(width >= 100 && percent >= 1 && percent <= 100);
    return area.x1 + left + (width * (int)(percent - 1) + 49) / 99;
}

static int slider_center_y(lv_obj_t *slider)
{
    lv_obj_update_layout(lv_screen_active());
    lv_area_t area;
    lv_obj_get_coords(slider, &area);
    lv_obj_scroll_by(lv_obj_get_parent(slider),0,
        LCD_HEIGHT/2-(area.y1+area.y2)/2,LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_get_coords(slider,&area);
    const int y = (area.y1 + area.y2) / 2;
    assert(y > 0 && y < LCD_HEIGHT);
    return y;
}

static void drag_brightness(lv_obj_t *slider, int y, unsigned target)
{
    const int start = brightness_x(slider, 50);
    step(false, 0, 0, false, false, true);
    step(true, start, y, false, false, true);
    step(true, brightness_x(slider, 70), y, false, false, true);
    step(true, brightness_x(slider, target), y, false, false, true);
    assert(lv_slider_get_value(slider) == (int32_t)target);
    assert(brightness_submits == 0 && brightness_previews > 0 &&
           preview_brightness_value == target && shared_writes == 0 && local_writes == 0 &&
           ui_current_view() == SETTINGS && find_slider(lv_screen_active()) == slider);
}

static void check_brightness_release(lv_obj_t *slider, int y, unsigned target)
{
    const int x = brightness_x(slider, target);
    const bool changed = target != stored_brightness;
    drag_brightness(slider, y, target);
    /* C3 reports count zero. C6 reports Up with unrelated payload bytes. */
    step(false, 0, 0, false, false, true);
    assert(slider_releases == 1 && slider_press_losses == 0 && slider_resets == 0);
    assert(brightness_submits == (unsigned)changed &&
           preview_brightness_value == 0 && shared_writes == 0 && local_writes == 0 &&
           ui_current_view() == SETTINGS && find_slider(lv_screen_active()) == slider);
    if (changed) assert(submitted_brightness == target);
    assert(lv_slider_get_value(slider) == (int32_t)target);
    assert(latest_base_x == x && latest_base_y == y);
    step(false, 0, 0, false, false, true);
    assert(latest_base_x == x && latest_base_y == y &&
           brightness_submits == (unsigned)changed && slider_releases == 1);
}

static void check_brightness_abort(lv_obj_t *slider, int y, const char *mode)
{
    drag_brightness(slider, y, 40);
    if (strcmp(mode, "press-lost") == 0) {
        /* Ask LVGL to abandon this contact when it next sees physical Up. */
        lv_indev_wait_release(lv_touch);
        step(false, 0, 0, false, false, true);
        assert(slider_press_losses == 1 && slider_resets == 0);
    } else if (strcmp(mode, "stale-reset") == 0) {
        /* The active sample ages out while the sampling task supplies no new item. */
        for (unsigned i = 0; i < 4; ++i) advance_time();
        lv_indev_read(lv_touch);
        assert(slider_resets == 1 && indev_gate.quarantined);
        render_touch(NULL, false, false);
    } else {
        BoardTouch invalid = cache_touch(false, 0, 0, true);
        const bool main_first = strcmp(mode, "io-reset-main-first") == 0;
        assert(main_first || strcmp(mode, "io-reset-lvgl-first") == 0);
        if (main_first) render_touch(&invalid, false, false);
        lv_indev_read(lv_touch);
        assert(slider_resets == 1 && indev_gate.quarantined &&
               latest_base_x == 0 && latest_base_y == 0);
        assert(preview_brightness_value == 0 && lv_slider_get_value(slider) == stored_brightness);
        if (!main_first) render_touch(&invalid, false, false);
    }
    step(false, 0, 0, false, false, true);
    assert(slider_releases == 0 && brightness_submits == 0);
    assert(preview_brightness_value == 0);
    assert(lv_slider_get_value(slider) == stored_brightness);
    drag_brightness(slider, slider_center_y(slider), 50);
    step(false, 0, 0, false, false, true);
    assert(slider_releases == 1 && brightness_submits == 1 &&
           submitted_brightness == 50 && preview_brightness_value == 0);
}
static void button_center(const char *text, int *x, int *y)
{
    uint32_t button = 0;
    lv_obj_t *matrix = find_matrix(lv_screen_active(), text, &button);
    assert(matrix);
    lv_obj_scroll_to_view(matrix, LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());
    lv_area_t bounds;
    lv_obj_get_coords(matrix, &bounds);
    const lv_area_t *item = &((lv_buttonmatrix_t *)matrix)->button_areas[button];
    *x = bounds.x1 + (item->x1 + item->x2) / 2;
    *y = bounds.y1 + (item->y1 + item->y2) / 2;
    assert(*x >= 0 && *x < LCD_WIDTH && *y >= 0 && *y < LCD_HEIGHT);
}
static void check_counters(unsigned c, unsigned s, unsigned l)
{
    assert(confirms == c && shared_writes == s && local_writes == l);
}
int main(int argc, char **argv)
{
    /* Exercise Main's sampler through this harness's first-priority SDK
     * header and linked doubles, before unchanged board/UI scenarios. */
    startup_heap_sample("host_board_main");
    assert(internal_free_reads == 1 && dma_free_reads == 1);
    assert(heap_caps_get_largest_free_block(MALLOC_CAP_DMA) == (1u << 20));
    assert(argc == 3);
    const bool alarm = strcmp(argv[1], "alarm") == 0;
    const bool shared = strcmp(argv[1], "shared") == 0;
    const bool local_setting = strcmp(argv[1], "local") == 0;
    const bool brightness = strcmp(argv[1], "brightness") == 0;
    if (brightness && strcmp(argv[2], "factory") == 0)
        stored_brightness = katmed_display_defaults().brightness;
    if (brightness && strcmp(argv[2], "stored-minimum") == 0)
        stored_brightness = 1;
    const bool delayed_poll = strcmp(argv[2], "delayed_poll") == 0;
    const bool failed_lock = strcmp(argv[2], "failed_lock") == 0;
    assert(alarm || shared || local_setting || brightness);
    assert(brightness || delayed_poll || failed_lock || strcmp(argv[2], "normal") == 0);
    lv_init();
    lv_display = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    assert(lv_display);
    lv_display_set_color_format(lv_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(lv_display, draw_buffer, NULL, sizeof(draw_buffer),
        LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(lv_display, flush);
    i2c_dev = (void *)1;
    lv_touch = lv_indev_create();
    assert(lv_touch);
    lv_indev_set_type(lv_touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(lv_touch, lv_display);
    lv_indev_set_read_cb(lv_touch, lv_touch_read);
    lv_indev_set_mode(lv_touch, LV_INDEV_MODE_EVENT);
    UiServices services = {.submit = submit, .brightness_preview = preview_brightness,
                           .display_settings = read_display};
    ui_set_services(&services);
    if (brightness) board_set_touch_filter(ui_lvgl_touch_allowed);
    katmed_i18n_select(KATMED_LANG_DE);
    state.alarm_active = alarm;
    state.start_reason = START_SETTINGS;
    state.round.origin = (EventId){1, 1, 1};
    state.round.ordinal = 1;
    local.clock_style = DIGITAL_LARGE;
    ClockSample clock = {.monotonic_ms = fake_ms, .domain = 1};
    ui_render(&state, &local, alarm ? ALARM : SETTINGS, &clock);
    lv_obj_update_layout(lv_screen_active());
    if (brightness) {
        lv_obj_t *slider = find_slider(lv_screen_active());
        assert(slider && lv_slider_get_value(slider) == stored_brightness);
        lv_obj_add_event_cb(slider, observe_slider, LV_EVENT_ALL, NULL);
        const int y = slider_center_y(slider);
        if (strcmp(argv[2], "factory") == 0) {
            assert(stored_brightness == 100);
            check_brightness_release(slider, y, 40);
        } else if (strcmp(argv[2], "stored-minimum") == 0) {
            check_brightness_release(slider, y, 50);
        } else if (strcmp(argv[2], "press-lost") == 0 || strcmp(argv[2], "stale-reset") == 0 ||
                   strcmp(argv[2], "io-reset-main-first") == 0 ||
                   strcmp(argv[2], "io-reset-lvgl-first") == 0) {
            check_brightness_abort(slider, y, argv[2]);
        } else {
            const unsigned target = (unsigned)strtoul(argv[2], NULL, 10);
            assert(target == 1 || target == 37 || target == 40 || target == 50 || target == 100);
            check_brightness_release(slider, y, target);
        }
        lv_indev_delete(lv_touch);
        lv_display_delete(lv_display);
        lv_deinit();
        printf("C%d brightness: %s PASS\n",
#if CONFIG_IDF_TARGET_ESP32C3
               3,
#else
               6,
#endif
               argv[2]);
        return 0;
    }
    int x = 90, y = 100;
    if (shared) button_center("Zufall", &x, &y);
    if (local_setting) button_center("90°", &x, &y);

    step(false, x, y, false, false, true); /* Arm a fresh contact. */
    step(true, x, y, false, false, true);
    check_counters(0, 0, 0); /* Down alone may never apply a setting. */
    step(true, x, y, true, failed_lock, !delayed_poll);
    if (delayed_poll) {
        /* I/O error is overwritten in the cache before LVGL polls again. */
        step(false, x, y, false, false, true);
    } else {
        step(false, x, y, false, false, true);
    }
    check_counters(0, 0, 0);
    step(true, x, y, false, false, true);
    step(false, x, y, false, false, true);
    check_counters(alarm ? 1 : 0, shared ? 1 : 0, local_setting ? 1 : 0);
    printf("%s %s C%d: after_error=0 fresh_contact=1\n", argv[1], argv[2],
#if CONFIG_IDF_TARGET_ESP32C3
        3
#else
        6
#endif
    );
    lv_indev_delete(lv_touch);
    lv_display_delete(lv_display);
    lv_deinit();
    return 0;
}
