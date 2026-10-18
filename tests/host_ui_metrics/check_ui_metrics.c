#define main original_surface_main
#include "../check_ui_surface.c"
#undef main

static void peer_display_event(lv_event_t *event)
{
    unsigned *ready_count = lv_event_get_user_data(event);
    (*ready_count)++;
}

int main(void)
{
    Host host = {.width = 240, .height = 240, .result = UI_SUBMIT_COMMITTED};
    host.frame = calloc((size_t)host.width * host.height, sizeof(*host.frame));
    uint16_t *buffer = calloc((size_t)host.width * 16u, sizeof(*buffer));
    assert(host.frame && buffer);
    lv_init();
    lv_display_t *display = lv_display_create(host.width, host.height);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL,
                           (uint32_t)host.width * 16u * 2u,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_user_data(display, &host);

    UiServices services = {.context = &host, .submit = submit,
                           .connected = connected};
    ui_set_services(&services);
    AppState state = {0};
    state.round.origin = (EventId){1, 1, 1};
    state.round.ordinal = 1;
    LocalSettings local = {.clock_style = DIGITAL_LARGE,
                           .orientation = ORIENTATION_0};
    ClockSample clock = {.monotonic_ms = 1000, .domain = 1,
                         .utc_ms = 1791973800000LL, .utc_valid = true,
                         .quality = CLOCK_SYNCHRONIZED};

    ui_render(&state, &local, BACKGROUND, &clock);
    draw();
    assert(!ui_blink_render_metrics().valid);
    unsigned peer_ready_count = 0;
    lv_display_add_event_cb(display, peer_display_event, LV_EVENT_REFR_READY,
                            &peer_ready_count);
    swipe(60, 130, 1100);
    assert(ui_current_view() == CLOCK);
    ui_render(&state, &local, ui_current_view(), &clock);
    draw();
    assert(!ui_blink_render_metrics().valid);
    ui_render(&state, &local, SETTINGS, &clock);
    draw();
    assert(ui_current_view() == SETTINGS);
    assert(!ui_blink_render_metrics().valid);
    assert(host.submit_calls == 0 && host.store_calls == 0);

    SharedSettings settings = app_default_settings();
    const uint8_t catalog[KATMED_CATALOG_BYTES] = {1};
    assert(app_init(&state, &settings, &clock, (EventId){1, 1, 1},
                    1, catalog) == APP_OK);
    AlarmAnimation first_phase;
    assert(app_alarm_animation(&state, &clock, &first_phase) == APP_OK);
    assert(first_phase.color == ALARM_COLOR_BLUE && first_phase.phase_permyriad == 0);
    ui_render(&state, &local, SETTINGS, &clock);
    assert(!ui_blink_render_metrics().valid);
    draw();
    UiBlinkRenderMetrics first = ui_blink_render_metrics();
    assert(first.valid && first.completed_cycles == 1);
    assert(first.last_blink_changes == 1);
    assert(first.last_render_us > 0);
    assert(first.last_flush_areas > 0 && first.last_flushed_pixels > 0);
    assert(first.last_flushed_pixels <= (uint32_t)host.width * host.height);
    assert(ui_current_view() == ALARM);
    assert(host.submit_calls == 0 && host.store_calls == 0);

    ui_render(&state, &local, ALARM, &clock);
    draw();
    assert(ui_blink_render_metrics().completed_cycles == first.completed_cycles);
    clock.utc_ms += 1000;
    ui_render(&state, &local, ALARM, &clock);
    draw();
    assert(ui_blink_render_metrics().completed_cycles == first.completed_cycles);
    clock.monotonic_ms = 1350;
    AlarmAnimation second_phase;
    assert(app_alarm_animation(&state, &clock, &second_phase) == APP_OK);
    assert(second_phase.color == ALARM_COLOR_BLUE &&
           second_phase.phase_permyriad > first_phase.phase_permyriad);
    ui_render(&state, &local, ALARM, &clock);
    draw();
    UiBlinkRenderMetrics second = ui_blink_render_metrics();
    assert(second.valid && second.completed_cycles == first.completed_cycles + 1);
    assert(second.last_blink_changes == 1);
    assert(second.last_render_us > 0);
    assert(second.last_flush_areas > 0 && second.last_flushed_pixels > 0);
    assert(second.last_flushed_pixels < (uint32_t)host.width * host.height);
    assert(peer_ready_count > 0);
    assert(host.submit_calls == 0 && host.store_calls == 0);

    printf("ui_metrics=PASS cycles=%u first_pixels=%u second_pixels=%u peer_ready=%u\n",
           second.completed_cycles, first.last_flushed_pixels,
           second.last_flushed_pixels, peer_ready_count);
    lv_display_delete(display);
    assert(!ui_blink_render_metrics().valid);
    lv_deinit();
    free(buffer);
    free(host.frame);
    return 0;
}
