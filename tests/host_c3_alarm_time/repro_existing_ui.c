#define main original_ui_surface_main
#include "../check_ui_surface.c"
#undef main
#include "katmed_core.h"

/* The real ui_render path must derive its colour from the active round's
 * elapsed time. This intentionally fails against d941faee's fixed red bar. */
int main(void)
{
    Host host = {.width = 240, .height = 240, .result = UI_SUBMIT_COMMITTED};
    host.frame = calloc((size_t)host.width * host.height, sizeof(*host.frame));
    uint16_t *buffer = calloc((size_t)host.width * 16, sizeof(*buffer));
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

    ClockSample clock = {.monotonic_ms = 1000, .domain = 17,
                         .utc_ms = 0, .utc_valid = false,
                         .quality = CLOCK_UNAVAILABLE};
    SharedSettings settings = app_default_settings();
    const uint8_t catalog[KATMED_CATALOG_BYTES] = {1};
    AppState state = {0};
    assert(app_init(&state, &settings, &clock, (EventId){1, 17, 1},
                    1, catalog) == APP_OK);
    LocalSettings local = {.clock_style = DIGITAL_LARGE,
                           .orientation = ORIENTATION_0};

    ui_render(&state, &local, ALARM, &clock);
    lv_obj_t *bar = find_alarm_bar(lv_screen_active());
    assert(bar);
    lv_color_t blue = lv_obj_get_style_bg_color(bar, 0);
    clock.monotonic_ms += 1800000;
    ui_render(&state, &local, ALARM, &clock);
    lv_color_t yellow = lv_obj_get_style_bg_color(bar, 0);
    clock.monotonic_ms += 1800000;
    ui_render(&state, &local, ALARM, &clock);
    lv_color_t red = lv_obj_get_style_bg_color(bar, 0);
    printf("existing_ui_color_samples=%08lx,%08lx,%08lx\n",
           (unsigned long)lv_color_to_u32(blue),
           (unsigned long)lv_color_to_u32(yellow),
           (unsigned long)lv_color_to_u32(red));
    int result = (!lv_color_eq(blue, yellow) &&
                  !lv_color_eq(yellow, red)) ? 0 : 1;

    /* The main loop requests a redraw every 50 ms. At red's terminal 2 Hz,
     * sampling can repeatedly straddle the tiny full-bar plateau. Exercise
     * the real ui_render path at that cadence and a concrete phase offset. */
    clock.monotonic_ms = 1000 + 2u * 60u * 60u * 1000u + 358u;
    unsigned maximum_strips = 0;
    for (unsigned sample = 0; sample < 10; ++sample) {
        AlarmAnimation animation;
        assert(app_alarm_animation(&state, &clock, &animation) == APP_OK);
        assert(animation.color == ALARM_COLOR_RED &&
               animation.frequency_millihz == 2000);
        ui_render(&state, &local, ALARM, &clock);
        lv_obj_update_layout(lv_screen_active());
        const unsigned visible = visible_alarm_strips(lv_screen_active(), bar);
        if (visible > maximum_strips) maximum_strips = visible;
        printf("red_2hz_sample_%u_phase=%u_strips=%u\n", sample,
               (unsigned)animation.phase_permyriad, visible);
        clock.monotonic_ms += 50;
    }
    printf("red_2hz_maximum_visible_strips=%u\n", maximum_strips);
    if (maximum_strips != 6) result = 1;
    lv_display_delete(display);
    lv_deinit();
    free(buffer);
    free(host.frame);
    return result;
}
