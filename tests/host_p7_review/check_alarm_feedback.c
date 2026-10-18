#define main original_surface_main
#include "../check_ui_surface.c"
#undef main

static bool visible_label(lv_obj_t *screen, const char *text)
{
    lv_obj_t *label = find_label(screen, text);
    return label && !lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN);
}

int main(void)
{
    Host host = {.width = 240, .height = 240, .result = UI_SUBMIT_PENDING_SHARED};
    host.frame = calloc((size_t)host.width * host.height, sizeof(*host.frame));
    uint16_t *buffer = calloc((size_t)host.width * 16, sizeof(*buffer));
    assert(host.frame && buffer);
    lv_init();
    lv_display_t *display = lv_display_create(host.width, host.height);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, host.width * 16u * 2u,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_user_data(display, &host);

    UiServices services = {.context = &host, .submit = submit, .connected = connected};
    ui_set_services(&services);
    AppState state = {0};
    state.round.origin = (EventId){1, 1, 1};
    state.round.ordinal = 1;
    LocalSettings local = {.orientation = ORIENTATION_0,
                           .clock_style = DIGITAL_LARGE};
    ClockSample clock = {.monotonic_ms = 1000, .domain = 1,
                         .utc_ms = 1791973800000LL, .utc_valid = true,
                         .quality = CLOCK_SYNCHRONIZED};

    ui_render(&state, &local, SETTINGS, &clock);
    lv_obj_t *screen = lv_screen_active();
    click_label(screen, "Medikamente: Stunden");
    enter_keys(screen, "2");
    click_label(screen, "Speichern");
    assert(host.submit_calls == 1);
    assert(host.last_request.kind == UI_REQUEST_SET_MEDICATION_SECONDS);
    assert(visible_label(screen, "Koordinator prüft Änderung"));

    // The same alarm round may begin while a shared write is still pending.
    // A late ACK must not resurrect the pending or success message over it.
    state.alarm_active = true;
    ui_render(&state, &local, ALARM, &clock);
    ui_shared_result(true);
    ui_render(&state, &local, ALARM, &clock);
    assert(!visible_label(screen, "Koordinator prüft Änderung"));
    assert(!visible_label(screen, "Gemeinsame Änderung gespeichert"));
    assert(host.submit_calls == 1 && host.store_calls == 0);

    state.alarm_active = false;
    ui_render(&state, &local, SETTINGS, &clock);
    assert(!visible_label(screen, "Gemeinsame Änderung gespeichert"));
    lv_display_delete(display);
    lv_deinit();
    free(buffer);
    free(host.frame);
    puts("P7 same-round alarm suppresses pending and late shared ACK feedback");
    return 0;
}
