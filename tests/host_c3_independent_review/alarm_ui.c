#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "katmed_ui.h"
#include "katmed_core.h"
#include "katmed_images.h"
#include "lvgl.h"

static uint16_t pixels[240 * 16];
static uint32_t flushed_pixels;

KatmedImageStatus katmed_images_present(lv_obj_t *image, lv_obj_t *fallback,
                                         uint32_t image_id, Orientation orientation)
{
    (void)image_id; (void)orientation;
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(fallback, LV_OBJ_FLAG_HIDDEN);
    return KATMED_IMAGE_OK;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *map)
{
    (void)map;
    flushed_pixels += (uint32_t)(area->x2 - area->x1 + 1) *
                      (uint32_t)(area->y2 - area->y1 + 1);
    lv_display_flush_ready(display);
}

static void draw(void)
{
    for (unsigned i = 0; i < 8; ++i) {
        lv_tick_inc(40);
        lv_timer_handler();
    }
}

typedef struct { unsigned visible; int min_y, max_y; lv_obj_t *bar; } Bars;
static Bars inspect_bars(void)
{
    Bars found = {.min_y = INT32_MAX, .max_y = INT32_MIN};
    lv_obj_t *screen = lv_screen_active();
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (lv_obj_get_style_bg_opa(child, 0) != 217) continue;
        if (!found.bar) { found.bar = child; continue; }
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        ++found.visible;
        int y = lv_obj_get_y(child);
        if (y < found.min_y) found.min_y = y;
        if (y > found.max_y) found.max_y = y;
    }
    return found;
}

static uint32_t bar_color(Bars bars)
{ assert(bars.bar); return lv_color_to_u32(lv_obj_get_style_bg_color(bars.bar, 0)); }

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(240, 240);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    ClockSample clock = {.monotonic_ms = 1000, .domain = 7};
    const uint8_t catalog[KATMED_CATALOG_BYTES] = {1};
    SharedSettings settings = app_default_settings();
    AppState state = {0};
    assert(app_init(&state, &settings, &clock, (EventId){1, 7, 1},
                    1, catalog) == APP_OK);
    LocalSettings local = {.clock_style = DIGITAL_LARGE};
    ui_render(&state, &local, ALARM, &clock);
    lv_obj_update_layout(lv_screen_active());
    Bars begin = inspect_bars();
    assert(begin.bar);
    assert(lv_obj_get_y(begin.bar) == 30 && lv_obj_get_height(begin.bar) == 180);
    assert(lv_obj_get_style_bg_opa(begin.bar, 0) == 217);
    fprintf(stderr, "independent alarm at zero: color=%08x strips=%u\n",
            bar_color(begin), begin.visible);
    assert(bar_color(begin) == lv_color_to_u32(lv_color_hex(0x1565c0)));
    assert(begin.visible == 1 && begin.min_y == 30);
    draw();
    flushed_pixels = 0;
    clock.monotonic_ms = 1350;
    ui_render(&state, &local, ALARM, &clock);
    draw();
    assert(flushed_pixels > 0 && flushed_pixels < 10000);
    fprintf(stderr, "independent changed-phase flush pixels=%u\n", flushed_pixels);
    clock.monotonic_ms = 2300;
    ui_render(&state, &local, ALARM, &clock);
    Bars half = inspect_bars();
    assert(half.visible == 6 && half.max_y == 180);
    clock.monotonic_ms = 3599;
    ui_render(&state, &local, ALARM, &clock);
    Bars end = inspect_bars();
    assert(end.visible == 1 && end.min_y == 180);
    clock.monotonic_ms = 1000 + 1800000;
    ui_render(&state, &local, ALARM, &clock);
    assert(bar_color(inspect_bars()) == lv_color_to_u32(lv_color_hex(0xffc107)));
    clock.monotonic_ms = 1000 + 3600000;
    ui_render(&state, &local, ALARM, &clock);
    assert(bar_color(inspect_bars()) == lv_color_to_u32(lv_color_hex(0xd50000)));
    AppState foreign = state;
    foreign.countdown_started.domain = 99;
    ui_render(&foreign, &local, ALARM, &clock);
    Bars fallback = inspect_bars();
    assert(!lv_obj_has_flag(fallback.bar, LV_OBJ_FLAG_HIDDEN));
    assert(fallback.visible == 0);
    assert(bar_color(fallback) == lv_color_to_u32(lv_color_hex(0x1565c0)));
    ui_render(&state, &local, ALARM, &clock);
    assert(bar_color(inspect_bars()) == lv_color_to_u32(lv_color_hex(0xd50000)));
    puts("independent alarm UI color, geometry, fill/empty, fallback: OK");
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
