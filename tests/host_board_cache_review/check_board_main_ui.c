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
#include "../../firmware/components/board/board.c"
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
static uint16_t draw_buffer[320 * 16];
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
bool lvgl_port_lock(unsigned timeout) { (void)timeout; return !lock_failure; }
void lvgl_port_unlock(void) {}
int esp_lcd_panel_set_gap(void) { return 0; }
int esp_lcd_panel_swap_xy(void) { return 0; }
int esp_lcd_panel_mirror(void) { return 0; }
KatmedImageStatus katmed_images_present(lv_obj_t *image, lv_obj_t *fallback,
    uint32_t id, Orientation orientation)
{
    (void)id; (void)orientation;
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(fallback, LV_OBJ_FLAG_HIDDEN);
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
    case UI_REQUEST_SET_ORIENTATION:
    case UI_REQUEST_SET_CLOCK_STYLE:
        local_writes++;
        return UI_SUBMIT_COMMITTED;
    default:
        shared_writes++;
        return UI_SUBMIT_UNAVAILABLE;
    }
}

static void advance_time(void)
{
    fake_ms += 50;
    lv_tick_inc(50);
}
static void step(bool down, int screen_x, int screen_y, bool fail_io,
                 bool fail_main_lock, bool poll_indev)
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
    lock_failure = fail_main_lock;
    ClockSample clock = {.monotonic_ms = fake_ms, .domain = 1,
        .utc_ms = 1791973800000LL, .utc_valid = true,
        .quality = CLOCK_SYNCHRONIZED};
    assert(render_ui(&state, &local, &clock, &sample,
        result == BOARD_OK, false) == !fail_main_lock);
    lock_failure = false;
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
    assert(argc == 3);
    const bool alarm = strcmp(argv[1], "alarm") == 0;
    const bool shared = strcmp(argv[1], "shared") == 0;
    const bool local_setting = strcmp(argv[1], "local") == 0;
    const bool delayed_poll = strcmp(argv[2], "delayed_poll") == 0;
    const bool failed_lock = strcmp(argv[2], "failed_lock") == 0;
    assert(alarm || shared || local_setting);
    assert(delayed_poll || failed_lock || strcmp(argv[2], "normal") == 0);
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
    UiServices services = {.submit = submit};
    ui_set_services(&services);
    state.alarm_active = alarm;
    state.round.origin = (EventId){1, 1, 1};
    state.round.ordinal = 1;
    local.clock_style = DIGITAL_LARGE;
    ClockSample clock = {.monotonic_ms = fake_ms, .domain = 1};
    ui_render(&state, &local, alarm ? ALARM : SETTINGS, &clock);
    lv_obj_update_layout(lv_screen_active());
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
