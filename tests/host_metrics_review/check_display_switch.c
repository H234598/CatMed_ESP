#define main original_surface_main
#include "../check_ui_surface.c"
#undef main

static lv_display_t *make_display(Host *host, uint16_t *buffer)
{
    lv_display_t *display = lv_display_create(host->width, host->height);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL,
                           (uint32_t)host->width * 16u * 2u,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_user_data(display, host);
    return display;
}

int main(void)
{
    Host hosts[2] = {
        {.width = 240, .height = 240, .result = UI_SUBMIT_COMMITTED},
        {.width = 240, .height = 240, .result = UI_SUBMIT_COMMITTED},
    };
    uint16_t *buffers[2] = {0};
    lv_init();
    for (unsigned i = 0; i < 2; ++i) {
        hosts[i].frame = calloc(240u * 240u, sizeof(uint16_t));
        buffers[i] = calloc(240u * 16u, sizeof(uint16_t));
        assert(hosts[i].frame && buffers[i]);
    }
    lv_display_t *first = make_display(&hosts[0], buffers[0]);
    UiServices services = {.context = &hosts[0], .submit = submit,
                           .connected = connected};
    ui_set_services(&services);
    AppState state = {.alarm_active = true};
    state.round.origin = (EventId){1, 1, 1};
    state.round.ordinal = 1;
    LocalSettings local = {.clock_style = DIGITAL_LARGE,
                           .orientation = ORIENTATION_0};
    ClockSample clock = {.monotonic_ms = 1000, .domain = 1,
                         .utc_ms = 1791973800000LL, .utc_valid = true,
                         .quality = CLOCK_SYNCHRONIZED};
    ui_render(&state, &local, ALARM, &clock);
    draw();
    assert(ui_blink_render_metrics().valid);

    lv_display_t *second = make_display(&hosts[1], buffers[1]);
    lv_display_set_default(second);
    services.context = &hosts[1];
    ui_set_services(&services);
    clock.monotonic_ms = 1500;
    ui_render(&state, &local, ALARM, &clock);
    assert(!ui_blink_render_metrics().valid);
    draw();
    assert(ui_blink_render_metrics().valid);
    const uint32_t cycles = ui_blink_render_metrics().completed_cycles;

    lv_display_delete(first);
    assert(ui_blink_render_metrics().valid);
    clock.monotonic_ms = 1750;
    ui_render(&state, &local, ALARM, &clock);
    draw();
    assert(ui_blink_render_metrics().completed_cycles == cycles + 1);
    assert(hosts[0].submit_calls == 0 && hosts[0].store_calls == 0);
    assert(hosts[1].submit_calls == 0 && hosts[1].store_calls == 0);

    lv_display_delete(second);
    assert(!ui_blink_render_metrics().valid);
    lv_deinit();
    for (unsigned i = 0; i < 2; ++i) {
        free(buffers[i]);
        free(hosts[i].frame);
    }
    puts("independent display-switch metrics lifecycle passed");
    return 0;
}
