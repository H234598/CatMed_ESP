/* Exercise ui_render and LVGL's partial RGB565 flush after one lost panel
 * transfer. The source image is the real host UI fixture, not an expected
 * rectangle copied into the framebuffer. */
#define main existing_surface_main
#include "../check_ui_surface.c"
#undef main

static bool drop_one_transfer;
static unsigned dropped_transfers;
static unsigned submitted_transfers;
static lv_area_t lost_area;

static void panel_flush(lv_display_t *display, const lv_area_t *area,
                        uint8_t *pixels)
{
    ++submitted_transfers;
    if (drop_one_transfer && area->y1 <= 90 && area->y2 >= 90) {
        drop_one_transfer = false;
        ++dropped_transfers;
        lost_area = *area;
        lv_display_flush_ready(display);
        return;
    }
    flush(display, area, pixels);
}

static uint64_t time_with_interior_fill(const AppState *state,
                                        ClockSample *clock, uint64_t stage_age,
                                        AlarmColor color)
{
    for (uint64_t age = stage_age; age < stage_age + 3000; ++age) {
        clock->monotonic_ms = 1000 + age;
        AlarmAnimation animation;
        assert(app_alarm_animation(state, clock, &animation) == APP_OK);
        if (animation.color == color &&
            animation.phase_permyriad >= 3800 &&
            animation.phase_permyriad <= 4200) return clock->monotonic_ms;
    }
    abort();
}

int main(void)
{
    Host host = {.width = 240, .height = 240, .result = UI_SUBMIT_COMMITTED};
    host.frame = calloc(240u * 240u, sizeof(*host.frame));
    uint16_t *first = calloc(240u * 16u, sizeof(*first));
    uint16_t *second = calloc(240u * 16u, sizeof(*second));
    assert(host.frame && first && second);
    lv_init();
    lv_display_t *display = lv_display_create(240, 240);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, first, second, 240u * 16u * 2u,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, panel_flush);
    lv_display_set_user_data(display, &host);
    UiServices services = {.context = &host, .submit = submit,
                           .connected = connected};
    ui_set_services(&services);
    ClockSample clock = {.monotonic_ms = 1000, .domain = 17,
                         .quality = CLOCK_UNAVAILABLE};
    SharedSettings settings = app_default_settings();
    const uint8_t catalog[KATMED_CATALOG_BYTES] = {1};
    AppState state = {0};
    assert(app_init(&state, &settings, &clock, (EventId){1, 17, 1},
                    1, catalog) == APP_OK);
    LocalSettings local = {.clock_style = DIGITAL_LARGE,
                           .orientation = ORIENTATION_0};
    AppState quiet = state;
    quiet.alarm_active = false;
    ui_render(&quiet, &local, BACKGROUND, &clock);
    draw();
    const uint16_t baseline = host.frame[90u * 240u + 5u];

    bool repaired_all = true;
    const uint64_t stages[] = {1800000u, 7200000u};
    const AlarmColor colors[] = {ALARM_COLOR_YELLOW, ALARM_COLOR_RED};
    for (unsigned stage = 0; stage < 2; ++stage) {
        const uint16_t previous = host.frame[90u * 240u + 5u];
        const uint64_t selected_ms = time_with_interior_fill(
            &state, &clock, stages[stage], colors[stage]);
        drop_one_transfer = true;
        ui_render(&state, &local, ALARM, &clock);
        draw();
        assert(dropped_transfers == stage + 1 && !drop_one_transfer);
        assert(host.frame[90u * 240u + 5u] == previous);

        /* At the next 50 ms tick, row 90 is still inside the same filled
         * interval. The fresh whole-screen raster is only the oracle. */
        clock.monotonic_ms = selected_ms + 50;
        AlarmAnimation next;
        assert(app_alarm_animation(&state, &clock, &next) == APP_OK);
        assert(next.color == colors[stage] && next.phase_permyriad > 3000);
        ui_render(&state, &local, ALARM, &clock);
        draw();
        const uint16_t incremental = host.frame[90u * 240u + 5u];
        uint16_t recovered_band[240u * 2u];
        unsigned band_rows = 0;
        for (int y = lost_area.y1; y <= lost_area.y2; ++y) {
            if (y < 30 || y >= 130) continue;
            recovered_band[band_rows++] = host.frame[(unsigned)y * 240u + 5u];
            recovered_band[band_rows++] = host.frame[(unsigned)y * 240u + 234u];
        }
        lv_obj_invalidate(lv_screen_active());
        draw();
        const uint16_t full_reference = host.frame[90u * 240u + 5u];
        bool repaired = incremental == full_reference &&
                        full_reference != previous && band_rows >= 2;
        unsigned checked = 0;
        for (int y = lost_area.y1; y <= lost_area.y2; ++y) {
            if (y < 30 || y >= 130) continue;
            repaired &= recovered_band[checked++] ==
                        host.frame[(unsigned)y * 240u + 5u];
            repaired &= recovered_band[checked++] ==
                        host.frame[(unsigned)y * 240u + 234u];
        }
        repaired_all &= repaired;
        printf("c3_panel_stage=%u lost_transfer=1 next_tick_repaired=%u "
               "phase=%u checked_side_pixels=%u submitted=%u\n", stage,
               repaired, (unsigned)next.phase_permyriad, checked,
               submitted_transfers);
    }
    assert(baseline != host.frame[90u * 240u + 5u]);
    lv_display_delete(display);
    lv_deinit();
    free(second);
    free(first);
    free(host.frame);
    return repaired_all ? 0 : 1;
}
