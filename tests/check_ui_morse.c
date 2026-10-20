#define main original_ui_surface_main
#include "check_ui_surface.c"
#undef main
#include "katmed_morse.h"

static unsigned flush_calls;

static void counted_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    ++flush_calls;
    flush(display, area, pixels);
}

static void assert_solid(const Host *host, uint16_t expected)
{
    for (int y = 0; y < host->height; ++y) {
        for (int x = 0; x < host->width; ++x) {
            if (host->frame[y * host->width + x] != expected) {
                fprintf(stderr, "expected 0x%04x at (%d,%d), got 0x%04x\n",
                        expected, x, y, host->frame[y * host->width + x]);
                abort();
            }
        }
    }
}

static void render_at(AppState *state, LocalSettings *local, ClockSample *clock,
                      Host *host, uint64_t when_ms, uint16_t expected)
{
    clock->monotonic_ms = when_ms;
    ui_render(state, local, ui_current_view(), clock);
    draw();
    assert_solid(host, expected);
}

static void render_current(AppState *state, LocalSettings *local,
                           ClockSample *clock, uint64_t when_ms)
{
    clock->monotonic_ms = when_ms;
    ui_render(state, local, ui_current_view(), clock);
    draw();
}

static uint64_t morse_first_transition(uint64_t start_ms)
{
    KatmedMorse expected;
    katmed_morse_start(&expected, start_ms);
    assert(expected.active && expected.white && expected.due_ms > start_ms);
    return expected.due_ms;
}

static uint64_t morse_pause_bounds(uint64_t start_ms, uint64_t *pause_end)
{
    KatmedMorse expected;
    katmed_morse_start(&expected, start_ms);
    size_t steps = 0;
    while (expected.active && expected.phase != 2u) {
        assert(++steps <= strlen(katmed_morse_message()) * 16u + 16u);
        assert(expected.due_ms > start_ms);
        start_ms = expected.due_ms;
        (void)katmed_morse_update(&expected, start_ms);
    }
    assert(expected.active && expected.phase == 2u && !expected.white);
    *pause_end = expected.due_ms;
    return start_ms;
}

static void confirm_alarm(AppState *state, LocalSettings *local,
                          ClockSample *clock, Host *host, uint64_t when_ms)
{
    assert(ui_current_view() == ALARM);
    ui_touch_sample(false, 0, 0, true, when_ms - 10);
    ui_touch_sample(true, 110, 110, true, when_ms);
    assert(ui_current_view() == ALARM);
    ui_touch_sample(false, 0, 0, true, when_ms + 60);
    assert(host->last_request.kind == UI_REQUEST_CONFIRM);
    if (host->result == UI_SUBMIT_COMMITTED || host->result == UI_SUBMIT_PENDING)
        state->alarm_active = false;
    render_current(state, local, clock, when_ms + 60);
}

/* This is a real LVGL pointer event, followed by the independent board touch
 * sample. The click callback can run before that contact's release sample. */
static void press_morse(lv_obj_t *screen, uint64_t when_ms,
                        bool board_release_first, uint64_t board_release_ms)
{
    lv_obj_t *caption = find_label(screen, "Seltsam");
    assert(caption);
    lv_obj_t *control = lv_obj_get_parent(caption);
    lv_obj_scroll_to_view(control, LV_ANIM_OFF);
    lv_obj_update_layout(screen);
    lv_area_t area;
    lv_obj_get_coords(control, &area);
    pointer_point.x = (area.x1 + area.x2) / 2;
    pointer_point.y = (area.y1 + area.y2) / 2;
    assert(pointer_point.x >= 0 && pointer_point.x < 240);
    assert(pointer_point.y >= 0 && pointer_point.y < 240);
    ui_touch_sample(false, 0, 0, true, when_ms - 10);
    pointer_down = true;
    lv_tick_inc(40);
    lv_timer_handler();
    ui_touch_sample(true, pointer_point.x, pointer_point.y, true, when_ms);
    if (board_release_first)
        ui_touch_sample(false, 0, 0, true, board_release_ms);
    pointer_down = false;
    lv_tick_inc(40);
    lv_timer_handler();
    assert(ui_current_view() == MORSE);
    if (!board_release_first)
        ui_touch_sample(false, 0, 0, true, board_release_ms);
    assert(ui_current_view() == MORSE);
}

int main(void)
{
    Host host = {.width = 240, .height = 240, .result = UI_SUBMIT_COMMITTED};
    host.frame = calloc((size_t)host.width * host.height, sizeof(*host.frame));
    uint16_t *buffer = calloc((size_t)host.width * 16, sizeof(*buffer));
    assert(host.frame && buffer);
    lv_init();
    lv_display_t *display = lv_display_create(host.width, host.height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, (uint32_t)host.width * 16u * 2u,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, counted_flush);
    lv_display_set_user_data(display, &host);
    UiServices services = {.context = &host, .submit = submit};
    ui_set_services(&services);
    ClockSample clock = {.monotonic_ms = 1000, .domain = 1};
    LocalSettings local = {.orientation = ORIENTATION_0,
                           .clock_style = DIGITAL_LARGE};
    AppState state = {.image_id = 0, .start_reason = START_SETTINGS};
    ui_render(&state, &local, BACKGROUND, &clock);
    swipe(150, 60, 1100);
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(ui_current_view() == SETTINGS);
    assert(find_label(lv_screen_active(), "Seltsam"));
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, pointer_read);

    /* Queued board release can predate the LVGL click callback even when
     * it is delivered afterward. It must still finish the start contact. */
    press_morse(lv_screen_active(), 1120, false, 1000);
    assert(host.submit_calls == 0 && host.store_calls == 0);
    render_at(&state, &local, &clock, &host, 1000, 0xffff);
    unsigned stable_flushes = flush_calls;
    render_at(&state, &local, &clock, &host, 1100, 0xffff);
    assert(flush_calls == stable_flushes);
    const uint64_t first_end = morse_first_transition(1000);
    render_at(&state, &local, &clock, &host, first_end - 1u, 0xffff);
    render_at(&state, &local, &clock, &host, first_end, 0x0000);
    state.alarm_active = true;
    state.round.origin = (EventId){.device = 1, .epoch = 1, .sequence = 1};
    state.round.ordinal = 1;
    const uint64_t first_alarm_at = first_end + 220u;
    render_current(&state, &local, &clock, first_alarm_at);
    assert(ui_current_view() == ALARM);
    lv_obj_t *alarm_text = find_label(lv_screen_active(),
                                      "Please customize me! This is Alarm!");
    assert(alarm_text && !lv_obj_is_hidden(alarm_text));
    assert(host.submit_calls == 0 && host.store_calls == 0);
    render_current(&state, &local, &clock, 5000);
    assert(ui_current_view() == ALARM);
    host.result = UI_SUBMIT_REJECTED;
    confirm_alarm(&state, &local, &clock, &host, 5000);
    assert(ui_current_view() == ALARM);
    assert(host.submit_calls == 1 && host.store_calls == 1);
    host.result = UI_SUBMIT_COMMITTED;
    confirm_alarm(&state, &local, &clock, &host, 5100);
    assert(host.submit_calls == 2 && host.store_calls == 2);
    assert(ui_current_view() == MORSE);
    /* With no active alarm, ordinary Morse cancellation has no ACK side effect. */
    assert(!state.alarm_active);
    ui_touch_sample(true, 110, 110, true, 5170);
    assert(ui_current_view() == MORSE);
    ui_touch_sample(false, 0, 0, true, 5230);
    assert(ui_current_view() == BACKGROUND);
    render_current(&state, &local, &clock, 5240);
    assert(host.submit_calls == 2 && host.store_calls == 2);

    state.alarm_active = false;
    ui_render(&state, &local, ui_current_view(), &clock);
    swipe(150, 60, 1700);
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(ui_current_view() == SETTINGS);
    const uint64_t cycle_start = 2000;
    uint64_t pause_end;
    const uint64_t pause_start = morse_pause_bounds(cycle_start, &pause_end);
    clock.monotonic_ms = cycle_start;
    ui_render(&state, &local, ui_current_view(), &clock);
    press_morse(lv_screen_active(), 2120, true, 2200);
    ui_touch_sample(true, 100, 100, true, cycle_start); /* Queued start contact. */
    ui_touch_sample(false, 0, 0, true, cycle_start);
    assert(ui_current_view() == MORSE);
    render_at(&state, &local, &clock, &host, pause_start - 1u, 0xffff);
    render_at(&state, &local, &clock, &host, pause_start, 0x0000);
    /* The alarm pauses the long black pause and resumes its remaining time. */
    const uint64_t alarm_at = pause_start + 1000u;
    clock.monotonic_ms = alarm_at;
    state.alarm_active = true;
    state.round.ordinal = 2;
    render_current(&state, &local, &clock, alarm_at);
    assert(ui_current_view() == ALARM);
    render_current(&state, &local, &clock, alarm_at + 1000u);
    assert(ui_current_view() == ALARM);
    assert(host.submit_calls == 2 && host.store_calls == 2);
    confirm_alarm(&state, &local, &clock, &host, alarm_at + 1000u);
    assert(host.submit_calls == 3 && host.store_calls == 3);
    assert(ui_current_view() == MORSE);
    const uint64_t resumed_pause_end = pause_end + 1060u;
    render_at(&state, &local, &clock, &host, resumed_pause_end - 1u, 0x0000);
    render_at(&state, &local, &clock, &host, resumed_pause_end, 0xffff);
    ui_touch_sample(true, 110, 110, true, resumed_pause_end + 10u);
    assert(ui_current_view() == MORSE);
    ui_touch_sample(false, 0, 0, true, resumed_pause_end + 70u);
    assert(ui_current_view() == BACKGROUND);
    render_current(&state, &local, &clock, resumed_pause_end + 80u);
    assert(host.submit_calls == 3 && host.store_calls == 3);
    lv_indev_delete(indev);
    lv_display_delete(display);
    lv_deinit();
    free(buffer);
    free(host.frame);
    return 0;
}
