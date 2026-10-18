#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board_touch_buffer.h"
#define KATMED_FULL_STACK 1
#define KATMED_WITH_CORE 1
#define KATMED_HOST_TOUCH_SAMPLER 1
#include "../../firmware/main/main.c"

static bool locked;
static unsigned confirms;
static AppState state;
static LocalSettings local;
static uint16_t pixels[240 * 16];
static BoardTouchBuffer touch_buffer;
bool board_touch_sampler_running(void) { return true; }
bool board_touch_sampler_take(BoardTouch *sample, BoardResult *result)
{
    return board_touch_buffer_take(&touch_buffer, sample, result);
}
bool lvgl_port_lock(uint32_t timeout) { (void)timeout; return !locked; }
void lvgl_port_unlock(void) {}
static UiSubmitResult submit(void *ctx, const UiRequest *req)
{
    (void)ctx;
    assert(req->kind == UI_REQUEST_CONFIRM);
    ++confirms;
    return UI_SUBMIT_PENDING;
}
KatmedImageStatus katmed_images_present(lv_obj_t *image, lv_obj_t *fallback,
                                        uint32_t id, Orientation orientation)
{
    (void)id; (void)orientation;
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(fallback, LV_OBJ_FLAG_HIDDEN);
    return KATMED_IMAGE_OK;
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *map)
{
    (void)area; (void)map;
    lv_display_flush_ready(display);
}
static void step_at(uint64_t ms, bool down, int x, bool lock_busy)
{
    ClockSample clock = {.monotonic_ms = ms, .domain = 1};
    BoardTouch touch = {.monotonic_ms = ms, .down = down, .x = x, .y = 100};
    board_touch_buffer_push(&touch_buffer, touch, BOARD_OK);
    locked = lock_busy;
    (void)render_ui(&state, &local, &clock, NULL, false, false);
    locked = false;
}
#define step(ms, down, busy) step_at(ms, down, 100, busy)
int main(int argc, char **argv)
{
    assert(argc == 2);
    lv_init();
    lv_display_t *display = lv_display_create(240, 240);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    UiServices services = {.submit = submit};
    ui_set_services(&services);
    local.clock_style = DIGITAL_LARGE;
    ClockSample clock = {.domain = 1};
    SharedSettings settings = app_default_settings();
    const uint8_t catalog[KATMED_CATALOG_BYTES] = {1};
    assert(app_init(&state, &settings, &clock, (EventId){1, 1, 1},
                    1, catalog) == APP_OK);
    ui_render(&state, &local, ALARM, &clock);
    lv_timer_handler();
    if(strcmp(argv[1], "lock_tap") == 0) {
        step(25, false, false);
        step(50, true, true);    /* LVGL holds its render lock. */
        step(125, false, true);  /* Valid 75 ms tap has ended. */
        step(1400, false, false);
        fprintf(stderr, "lock_tap confirms=%u\n", confirms);
        assert(confirms == 1);
    }
    else if(strcmp(argv[1], "lock_swipe") == 0) {
        step(25, false, false);
        step(50, true, true);
        step_at(90, true, 170, true);
        step_at(125, false, 170, true);
        step(1400, false, false);
        fprintf(stderr, "lock_swipe confirms=%u\n", confirms);
        assert(confirms == 0);
    }
    else if(strcmp(argv[1], "old_round_tap") == 0) {
        step(25, false, false);
        step(50, true, true);
        step(125, false, true);
        ClockSample new_start = {.monotonic_ms = 100, .domain = 1};
        SharedSettings new_settings = app_default_settings();
        const uint8_t new_catalog[KATMED_CATALOG_BYTES] = {1};
        assert(app_init(&state, &new_settings, &new_start,
                        (EventId){2, 1, 1}, 1, new_catalog) == APP_OK);
        step(1400, false, false);
        fprintf(stderr, "old_round_tap confirms=%u\n", confirms);
        assert(confirms == 0);
    }
    else if(strcmp(argv[1], "overflow") == 0) {
        step(25, false, false);
        for (unsigned i = 0; i < BOARD_TOUCH_BUFFER_CAPACITY + 2u; ++i)
            step(50 + i * 20u, true, true);
        step(2800, false, true);
        step(2820, false, false);
        assert(confirms == 0);
        step(2850, true, false);
        step(2925, false, false);
        fprintf(stderr, "overflow fresh_confirm=%u\n", confirms);
        assert(confirms == 1);
    }
    else if(strcmp(argv[1], "bar_pixels") == 0) {
        clock.monotonic_ms = 450;
        ui_render(&state, &local, ALARM, &clock);
        lv_tick_inc(450);
        lv_timer_handler();
        UiBlinkRenderMetrics m = ui_blink_render_metrics();
        fprintf(stderr, "bar_pixels areas=%u pixels=%u\n", m.last_flush_areas, m.last_flushed_pixels);
        assert(m.valid);
        assert(m.last_flushed_pixels < 10000);
    }
    else if(strcmp(argv[1], "fallback") == 0) {
        lv_obj_t *bar = NULL;
        lv_obj_t *screen = lv_screen_active();
        for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
            lv_obj_t *child = lv_obj_get_child(screen, i);
            if (lv_obj_get_style_bg_opa(child, 0) == 217 &&
                lv_obj_get_height(child) == 180) { bar = child; break; }
        }
        assert(bar);
        ClockSample foreign = {.monotonic_ms = 100000, .domain = 2};
        ui_render(&state, &local, ALARM, &foreign);
        assert(!lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN));
        assert(lv_color_eq(lv_obj_get_style_bg_color(bar, 0), lv_color_hex(0x1565c0)));
        clock.monotonic_ms = 100000;
        ui_render(&state, &local, ALARM, &clock);
        assert(lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN));
        fprintf(stderr, "fallback static_blue_then_original_round=PASS\n");
    }
    else assert(0);
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
