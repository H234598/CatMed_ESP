#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "board_touch_buffer.h"
#define KATMED_FULL_STACK 1
#define KATMED_WITH_CORE 1
#define KATMED_HOST_TOUCH_SAMPLER 1
#include KATMED_MAIN_SOURCE

static bool busy;
static unsigned confirmation_calls;
static BoardTouchBuffer samples;
static AppState state;
static LocalSettings local;
static uint16_t pixel_buffer[240 * 16];

bool board_touch_sampler_running(void) { return true; }
bool board_touch_sampler_take(BoardTouch *sample, BoardResult *result)
{ return board_touch_buffer_take(&samples, sample, result); }
bool lvgl_port_lock(uint32_t timeout) { (void)timeout; return !busy; }
void lvgl_port_unlock(void) {}

static UiSubmitResult independent_submit(void *context, const UiRequest *request)
{
    (void)context;
    assert(request->kind == UI_REQUEST_CONFIRM);
    ++confirmation_calls;
    return UI_SUBMIT_PENDING;
}

KatmedImageStatus katmed_images_present(lv_obj_t *image, lv_obj_t *fallback,
                                         uint32_t image_id, Orientation orientation)
{
    (void)image_id;
    (void)orientation;
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(fallback, LV_OBJ_FLAG_HIDDEN);
    return KATMED_IMAGE_OK;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *map)
{ (void)area; (void)map; lv_display_flush_ready(display); }

static void sample(uint64_t at, bool down, int16_t x, bool lock_busy)
{
    BoardTouch touch = {.monotonic_ms = at, .down = down, .x = x, .y = 100};
    board_touch_buffer_push(&samples, touch, BOARD_OK);
    busy = lock_busy;
    ClockSample now = {.monotonic_ms = at, .domain = 1};
    (void)render_ui(&state, &local, &now, NULL, false, false);
    busy = false;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    lv_init();
    lv_display_t *display = lv_display_create(240, 240);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixel_buffer, NULL, sizeof(pixel_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    UiServices services = {.submit = independent_submit};
    ui_set_services(&services);
    ClockSample start = {.monotonic_ms = 1000, .domain = 1};
    SharedSettings settings = app_default_settings();
    const uint8_t catalog[KATMED_CATALOG_BYTES] = {1};
    assert(app_init(&state, &settings, &start, (EventId){1, 1, 1},
                    1, catalog) == APP_OK);
    local.clock_style = DIGITAL_LARGE;
    ui_render(&state, &local, ALARM, &start);
    lv_timer_handler();

    sample(1010, false, 100, false);
    if (strcmp(argv[1], "short_tap") == 0) {
        sample(1040, true, 100, true);
        sample(1115, false, 100, true);
        sample(2400, false, 100, false);
        assert(confirmation_calls == 1);
    } else if (strcmp(argv[1], "swipe") == 0) {
        sample(1040, true, 100, true);
        sample(1080, true, 180, true);
        sample(1120, false, 180, true);
        sample(2400, false, 100, false);
        assert(confirmation_calls == 0);
    } else if (strcmp(argv[1], "overflow") == 0) {
        for (unsigned i = 0; i < BOARD_TOUCH_BUFFER_CAPACITY + 2; ++i)
            sample(1040 + i * 20, true, 100, true);
        sample(3800, false, 100, true);
        sample(3820, false, 100, false);
        assert(confirmation_calls == 0);
        sample(3900, true, 100, false);
        sample(3975, false, 100, false);
        assert(confirmation_calls == 1);
    } else {
        assert(!"unknown contact scenario");
    }
    printf("independent %s confirmations=%u\n", argv[1], confirmation_calls);
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
