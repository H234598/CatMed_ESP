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
static bool flush_starts_at_row[320];

static void panel_flush(lv_display_t *display, const lv_area_t *area,
                        uint8_t *pixels)
{
    ++submitted_transfers;
    assert(area->y1 >= 0 && area->y1 < 320);
    flush_starts_at_row[area->y1] = true;
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

typedef struct {
    uint64_t age;
    int c3_top, c3_end;
    int c6_top, c6_end;
    uint16_t color;
} BarSample;

static void check_phase_pixels(Host *host, AppState *state, LocalSettings *local,
                               ClockSample *clock, bool coalesce)
{
    /* Boot-relative fixtures, with the first and last covered rows set from
     * the alarm timing and panel geometry. End rows are exclusive. No phase
     * query, bar object geometry or whole-frame redraw supplies the oracle. */
    static const BarSample samples[] = {
        {1801441, 30, 81, 40, 107, 0xfe00},
        {1801562, 30, 106, 40, 141, 0xfe00},
        {1805774, 79, 210, 106, 280, 0xfe00},
        {1805895, 105, 210, 140, 280, 0xfe00},
        {7200186, 30, 81, 40, 108, 0xd000},
        {7200220, 30, 106, 40, 141, 0xd000},
        {7200435, 80, 210, 106, 280, 0xd000},
        {7200470, 105, 210, 140, 280, 0xd000},
    };
    const bool c3 = host->width == 240;
    const int xs[] = {c3 ? 18 : 6, c3 ? 221 : 165};
    const int first_row = c3 ? 65 : 86;
    const int end_row = c3 ? 110 : 149;
    unsigned checked = 0;
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        const BarSample *sample = &samples[i];
        clock->monotonic_ms = 1000 + sample->age;
        ui_render(state, local, ALARM, clock);
        if (coalesce && i % 2 == 0) continue;
        memset(flush_starts_at_row, 0, sizeof(flush_starts_at_row));
        draw();
        const int top = c3 ? sample->c3_top : sample->c6_top;
        const int end = c3 ? sample->c3_end : sample->c6_end;
        if (i % 2 != 0) {
            const BarSample *previous = sample - 1;
            const int from = i % 4 == 1 ?
                (c3 ? previous->c3_end : previous->c6_end) :
                (c3 ? previous->c3_top : previous->c6_top);
            const int to = i % 4 == 1 ? end : top;
            bool crossed = false;
            for (int y = from + 1; y < to; ++y) crossed |= flush_starts_at_row[y];
            assert(crossed);
        }
        for (int y = first_row; y < end_row; ++y) {
            for (unsigned col = 0; col < sizeof(xs) / sizeof(xs[0]); ++col) {
                const int index = y * host->width + xs[col];
                const uint16_t background = test_image[index];
                const uint16_t actual = host->frame[index];
                if (y < top || y >= end) {
                    if (actual != background) {
                        fprintf(stderr, "alarm_background_mismatch sample=%u x=%d y=%d\n",
                                i, xs[col], y);
                        abort();
                    }
                } else {
                    /* Independent channel blend at opacity 217 with one
                     * quantization level of tolerance for RGB565 rounding. */
                    const unsigned shifts[] = {0, 5, 11};
                    const unsigned masks[] = {31, 63, 31};
                    for (unsigned n = 0; n < 3; ++n) {
                        const unsigned fg = (sample->color >> shifts[n]) & masks[n];
                        const unsigned bg = (background >> shifts[n]) & masks[n];
                        const int expected = (217u * fg + 38u * bg) / 255u;
                        const int channel = (actual >> shifts[n]) & masks[n];
                        if (abs(channel - expected) > 1) {
                            fprintf(stderr, "alarm_fill_mismatch sample=%u x=%d y=%d channel=%u\n",
                                    i, xs[col], y, n);
                            abort();
                        }
                    }
                }
                ++checked;
            }
        }
    }
    printf("alarm_pixels panel=%s coalesced=%u checked=%u\n",
           c3 ? "C3" : "C6", coalesce, checked);
}

static int run_alarm_panel_test(int argc, char **argv)
{
    const bool c6 = argc == 2 && strcmp(argv[1], "c6") == 0;
    const bool coalesce = argc == 2 && strcmp(argv[1], "c3-coalesced") == 0;
    const bool pixel_test = argc == 2 && strcmp(argv[1], "c3") == 0;
    assert(argc == 1 || c6 || coalesce || pixel_test);
    Host host = {.width = c6 ? 172 : 240, .height = c6 ? 320 : 240,
                 .result = UI_SUBMIT_COMMITTED};
    const size_t pixels_per_buffer = (size_t)host.height * 16u;
    host.frame = calloc((size_t)host.width * host.height, sizeof(*host.frame));
    uint16_t *first = calloc(pixels_per_buffer, sizeof(*first));
    uint16_t *second = calloc(pixels_per_buffer, sizeof(*second));
    assert(host.frame && first && second);
    lv_init();
    lv_display_t *display = lv_display_create(host.width, host.height);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, first, second, pixels_per_buffer * sizeof(*first),
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
    if (c6 || coalesce || pixel_test) {
        check_phase_pixels(&host, &state, &local, &clock, coalesce);
        lv_display_delete(display);
        lv_deinit();
        free(second);
        free(first);
        free(host.frame);
        return 0;
    }
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

#ifndef KATMED_PANEL_TRANSPORT_TEST
int main(int argc, char **argv)
{
    return run_alarm_panel_test(argc, argv);
}
#endif
