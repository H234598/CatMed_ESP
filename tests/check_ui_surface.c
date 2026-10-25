#include "katmed_ui.h"
#include "katmed_i18n.h"
#include "katmed_font.h"
#include "clock_view.h"
#include "katmed_images.h"
#include "katmed_core.h"
#include "katmed_reset_journal.h"
#include "c3_pins.h"
#include "lvgl.h"
#include "src/misc/lv_text_ap.h"
#include "src/misc/lv_text_private.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(KATMED_C3_LCD_WIDTH == 240 && KATMED_C3_LCD_HEIGHT == 240,
               "C3 display profile must match the GUITION panel");
_Static_assert(KATMED_C3_LCD_SCK == 6 && KATMED_C3_LCD_MOSI == 7 &&
               KATMED_C3_LCD_DC == 2 && KATMED_C3_LCD_CS == 10 &&
               KATMED_C3_LCD_BACKLIGHT == 3,
               "C3 LCD pins must match the GUITION evidence");
_Static_assert(KATMED_C3_TOUCH_SDA == 4 && KATMED_C3_TOUCH_SCL == 5 &&
               KATMED_C3_TOUCH_RESET == 1 && KATMED_C3_TOUCH_INTERRUPT == 0 &&
               KATMED_C3_TOUCH_I2C_ADDRESS_7BIT == 0x15,
               "C3 touch pins must match the GUITION evidence");

#ifdef KATMED_SURFACE_ALLOC_ACCOUNTING
/* Host-only request accounting. TLSF block capacity can gain alignment slack
 * without retaining another allocation or another requested byte. */
static struct { void *pointer; size_t bytes; } surface_allocations[4096];
void *__real_lv_malloc_core(size_t bytes);
void *__real_lv_realloc_core(void *pointer, size_t bytes);
void __real_lv_free_core(void *pointer);
static void surface_remember(void *pointer, size_t bytes)
{
    if (!pointer) return;
    for (unsigned i = 0; i < 4096; ++i) {
        if (!surface_allocations[i].pointer) {
            surface_allocations[i].pointer = pointer;
            surface_allocations[i].bytes = bytes;
            return;
        }
    }
    abort();
}
static void surface_forget(void *pointer)
{
    if (!pointer) return;
    for (unsigned i = 0; i < 4096; ++i) {
        if (surface_allocations[i].pointer == pointer) {
            surface_allocations[i].pointer = NULL;
            surface_allocations[i].bytes = 0;
            return;
        }
    }
    abort();
}
void *__wrap_lv_malloc_core(size_t bytes)
{
    void *pointer = __real_lv_malloc_core(bytes);
    surface_remember(pointer, bytes);
    return pointer;
}
void *__wrap_lv_realloc_core(void *pointer, size_t bytes)
{
    void *replacement = __real_lv_realloc_core(pointer, bytes);
    if (replacement) {
        surface_forget(pointer);
        surface_remember(replacement, bytes);
    }
    return replacement;
}
void __wrap_lv_free_core(void *pointer)
{
    surface_forget(pointer);
    __real_lv_free_core(pointer);
}
static void surface_live_requests(size_t *count, size_t *bytes)
{
    *count = *bytes = 0;
    for (unsigned i = 0; i < 4096; ++i) {
        if (surface_allocations[i].pointer) {
            ++*count;
            *bytes += surface_allocations[i].bytes;
        }
    }
}
#endif

typedef struct {
    int width, height;
    uint16_t *frame;
    int submit_calls, store_calls, network_calls;
    UiSubmitResult result;
    UiRequest last_request;
    uint8_t display_percent, preview_percent;
    unsigned preview_calls;
    UiNetworkStatus network;
    bool reset_prepared, reset_warning;
    bool reset_marker;
    KatmedResetConfirmation reset_confirmation;
    uint64_t reset_now_ms;
    uint8_t reset_random_sequence;
    unsigned reset_arms, reset_cancels, reset_confirms;
} Host;

static bool reset_arm(void *context, bool from_web)
{
    Host *host = context;
    (void)from_web;
    if (host->reset_prepared || host->reset_warning) return false;
    host->reset_prepared = true;
    host->reset_marker = true; /* Durable one-boot marker in this test model. */
    ++host->reset_arms;
    return true;
}

static bool reset_cancel(void *context)
{
    Host *host = context;
    host->reset_prepared = host->reset_warning = false;
    host->reset_marker = false;
    memset(&host->reset_confirmation, 0, sizeof(host->reset_confirmation));
    ++host->reset_cancels;
    return true;
}

static bool reset_random_bytes(void *context, uint8_t *out, size_t length)
{
    Host *host = context;
    for (size_t i = 0; i < length; ++i) out[i] = ++host->reset_random_sequence;
    return true;
}

static void reset_simulate_hardware_boot(Host *host, bool reset_reason_may_be_physical)
{
    const bool preparation_consumed = host->reset_marker;
    host->reset_marker = false;
    host->reset_prepared = false;
    memset(&host->reset_confirmation, 0, sizeof(host->reset_confirmation));
    host->reset_warning = katmed_reset_warning_after_boot(
        preparation_consumed, reset_reason_may_be_physical);
}

static bool reset_prepare_warning(void *context, char *out, size_t capacity, size_t *length)
{
    Host *host = context;
    return host->reset_warning && katmed_reset_confirmation_prepare(
        &host->reset_confirmation, reset_random_bytes, host, host->reset_now_ms,
        90000u, out, capacity, length);
}

static int reset_confirm(void *context, const char *token, size_t length)
{
    Host *host = context;
    if (!host->reset_warning || !katmed_reset_confirmation_consume(
            &host->reset_confirmation, token, length, host->reset_now_ms)) return 1;
    host->reset_warning = false;
    ++host->reset_confirms;
    return 0;
}

static uint16_t test_image[320 * 320];
static lv_image_dsc_t image_dsc;

static unsigned image_orientations_seen;
KatmedImageStatus katmed_images_present(lv_obj_t *image, lv_obj_t *fallback,
                                        uint32_t image_id, Orientation orientation)
{
    (void)image_id;
    assert(orientation >= ORIENTATION_0 && orientation <= ORIENTATION_270);
    image_orientations_seen|=1u<<orientation;
    int width = lv_display_get_horizontal_resolution(lv_display_get_default());
    int height = lv_display_get_vertical_resolution(lv_display_get_default());
    int source_width = orientation == ORIENTATION_90 || orientation == ORIENTATION_270 ?
                       height : width;
    int source_height = orientation == ORIENTATION_90 || orientation == ORIENTATION_270 ?
                        width : height;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int sx = x, sy = y;
            if (orientation == ORIENTATION_90) {
                sx = y;
                sy = source_height - 1 - x;
            } else if (orientation == ORIENTATION_180) {
                sx = source_width - 1 - x;
                sy = source_height - 1 - y;
            } else if (orientation == ORIENTATION_270) {
                sx = source_width - 1 - y;
                sy = x;
            }
            unsigned r = (unsigned)(sx * 15 / source_width);
            unsigned g = (unsigned)(sy * 45 / source_height) + 12;
            unsigned b = ((sx / 24 + sy / 24) & 1) ? 22 : 14;
            test_image[y * width + x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    image_dsc.header.w = (uint16_t)width;
    image_dsc.header.h = (uint16_t)height;
    image_dsc.header.stride = (uint16_t)(width * 2);
    image_dsc.data_size = (uint32_t)(width * height * 2);
    image_dsc.data = (const uint8_t *)test_image;
    lv_image_set_src(image, &image_dsc);
    lv_obj_set_hidden(fallback, true);
    return KATMED_IMAGE_OK;
}

static UiSubmitResult submit(void *context, const UiRequest *request)
{
    Host *host = context;
    assert(request);
    host->last_request = *request;
    host->submit_calls++;
    if(request->kind==UI_REQUEST_SET_BRIGHTNESS && host->result==UI_SUBMIT_COMMITTED)
        host->display_percent=(uint8_t)request->value.display_value;
    if (request->kind == UI_REQUEST_CONFIRM) host->store_calls++;
    else host->network_calls++;
    return host->result;
}

static void read_display(void *context,KatmedDisplaySettings *out)
{
    Host *host=context;*out=katmed_display_defaults();
    if(host->display_percent)out->brightness=host->display_percent;
}
static bool preview_brightness(void *context,uint8_t percent)
{
    Host *host=context;assert(percent<=100);host->preview_percent=percent;
    ++host->preview_calls;return true;
}

static bool connected(void *context)
{
    (void)context;
    return false;
}

static void network_status(void *context, UiNetworkStatus *status)
{
    *status = ((Host *)context)->network;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    Host *host = lv_display_get_user_data(display);
    int length = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; ++y) {
        memcpy(host->frame + y * host->width + area->x1,
               pixels + (y - area->y1) * length * 2, (size_t)length * 2);
    }
    lv_display_flush_ready(display);
}

static unsigned status_row_creations, status_row_deletions;
static void status_row_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CHILD_CREATED) ++status_row_creations;
    if (lv_event_get_code(event) == LV_EVENT_DELETE) ++status_row_deletions;
}

static void draw(void)
{
    for (int i = 0; i < 8; ++i) {
        lv_tick_inc(40);
        lv_timer_handler();
    }
}

static void snapshot(const Host *host, const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n%d %d\n255\n", host->width, host->height);
    for (int i = 0; i < host->width * host->height; ++i) {
        uint16_t p = host->frame[i];
        unsigned char rgb[3] = {
            (unsigned char)(((p >> 11) & 31) * 255 / 31),
            (unsigned char)(((p >> 5) & 63) * 255 / 63),
            (unsigned char)((p & 31) * 255 / 31)};
        assert(fwrite(rgb, 1, 3, file) == 3);
    }
    assert(fclose(file) == 0);
}

static void swipe(int x0, int x1, uint64_t t)
{
    ui_touch_sample(false, 0, 0, true, t);
    ui_touch_sample(true, (int16_t)x0, 100, true, t + 10);
    ui_touch_sample(true, (int16_t)x1, 100, true, t + 100);
    ui_touch_sample(false, 0, 0, true, t + 130);
}

static void settings_parent(void)
{
    static uint64_t tick=UINT64_C(40000000);tick+=1000;
    swipe(35,185,tick);
}

static lv_obj_t *find_label(lv_obj_t *node, const char *text)
{
    if (lv_obj_check_type(node, &lv_label_class) &&
        strcmp(lv_label_get_text(node), text) == 0) return node;
    for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i) {
        lv_obj_t *found = find_label(lv_obj_get_child(node, i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_label_prefix(lv_obj_t *node, const char *text)
{
    if (lv_obj_check_type(node, &lv_label_class) &&
        strncmp(lv_label_get_text(node), text, strlen(text)) == 0) return node;
    for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i) {
        lv_obj_t *found = find_label_prefix(lv_obj_get_child(node, i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_alarm_bar(lv_obj_t *screen)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (lv_obj_get_style_bg_opa(child, 0) == 217) return child;
    }
    return NULL;
}

static unsigned visible_alarm_strips(lv_obj_t *screen, lv_obj_t *fallback_bar)
{
    unsigned count = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (child != fallback_bar && lv_obj_get_style_bg_opa(child, 0) == 217 &&
            !lv_obj_is_hidden(child)) ++count;
    }
    return count;
}

static lv_obj_t *find_clock_view(lv_obj_t *screen)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (find_label(child, "12:30") || find_label(child, "00:00") ||
            find_label(child, "--:--") || find_label(child,"12:\n30") ||
            find_label(child,"00:\n00") || find_label(child,"--:\n--")) return child;
    }
    return NULL;
}

static bool click_matrix_button(lv_obj_t *node, const char *text)
{
    if (lv_obj_check_type(node, &lv_buttonmatrix_class)) {
        for (uint32_t i = 0; i < 16; ++i) {
            const char *key = lv_buttonmatrix_get_button_text(node, i);
            if (key && strcmp(key, text) == 0) {
                lv_buttonmatrix_set_selected_button(node, i);
                lv_obj_send_event(node, LV_EVENT_VALUE_CHANGED, NULL);
                return true;
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i) {
        if (click_matrix_button(lv_obj_get_child(node, i), text)) return true;
    }
    return false;
}

static lv_obj_t *find_matrix_with_first_button(lv_obj_t *node, const char *text)
{
    if (lv_obj_check_type(node, &lv_buttonmatrix_class)) {
        const char *first = lv_buttonmatrix_get_button_text(node, 0);
        if (first && strcmp(first, text) == 0) return node;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i) {
        lv_obj_t *found = find_matrix_with_first_button(lv_obj_get_child(node, i), text);
        if (found) return found;
    }
    return NULL;
}

static bool pointer_down;
static lv_point_t pointer_point;

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = pointer_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point = pointer_point;
}

static void pointer_click(lv_display_t *display, lv_obj_t *target)
{
    lv_obj_update_layout(lv_screen_active());
    lv_area_t area;
    lv_obj_get_coords(target, &area);
    pointer_point.x = (area.x1 + area.x2) / 2;
    pointer_point.y = (area.y1 + area.y2) / 2;
    lv_indev_t *pointer = lv_indev_create();
    assert(pointer);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, display);
    lv_indev_set_read_cb(pointer, pointer_read);
    pointer_down = true;
    lv_indev_read(pointer);
    lv_tick_inc(60);
    pointer_down = false;
    lv_indev_read(pointer);
    lv_indev_reset(pointer, NULL);
    lv_indev_delete(pointer);
}

static lv_obj_t *find_control_caption(lv_obj_t *node, const char *text)
{
    if (lv_obj_check_type(node, &lv_label_class) &&
        lv_obj_check_type(lv_obj_get_parent(node), &lv_button_class) &&
        strcmp(lv_label_get_text(node), text) == 0) return node;
    for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i) {
        lv_obj_t *found = find_control_caption(lv_obj_get_child(node, i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_slider(lv_obj_t *node)
{
    if(lv_obj_check_type(node,&lv_slider_class))return node;
    for(uint32_t i=0;i<lv_obj_get_child_count(node);++i){
        lv_obj_t *found=find_slider(lv_obj_get_child(node,i));if(found)return found;
    }
    return NULL;
}

static void click_label(lv_obj_t *screen, const char *text)
{
    if ((strcmp(text, "Status") == 0 && ui_current_view() == SETTINGS && !find_label(screen, text)) ||
        (strcmp(text, "Zurück") == 0 && ui_current_view() == UI_VIEW_STATUS)) {
        /* These navigation buttons were removed: exercise the actual edge swipe. */
        static uint64_t nav_time = UINT64_C(10000000);
        nav_time += 1000;
        int height=lv_display_get_vertical_resolution(lv_display_get_default());
        bool down=ui_current_view()==UI_VIEW_STATUS;
        ui_touch_sample(false,0,0,true,nav_time);
        ui_touch_sample(true,100,down?1:height-2,true,nav_time+30);
        ui_touch_sample(true,100,down?100:height-100,true,nav_time+130);
        ui_touch_sample(false,0,0,true,nav_time+230);
        return;
    }
    if (strlen(text) == 1 && strchr("0123456789.,:<", text[0]) &&
        click_matrix_button(screen, text)) return;
    lv_obj_t *caption = find_control_caption(screen, text);
    if (caption) {
        lv_obj_send_event(lv_obj_get_parent(caption), LV_EVENT_CLICKED, NULL);
        return;
    }
    const bool clicked = click_matrix_button(screen, text);
    if (!clicked)
        fprintf(stderr, "button not found: %s\n", text);
    assert(clicked && "button not found");
}

static unsigned count_objects(lv_obj_t *object)
{
    unsigned count = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        count += count_objects(lv_obj_get_child(object, i));
    return count;
}

static void report_memory(const char *phase)
{
    if (!getenv("KATMED_UI_MEM")) return;
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    fprintf(stderr, "memory %s: used=%zu peak=%zu free=%zu largest=%zu objects=%u\n",
            phase, memory.total_size - memory.free_size, memory.max_used,
            memory.free_size, memory.free_biggest_size,
            count_objects(lv_screen_active()));
}

static void cycle_views(lv_obj_t *screen)
{
    click_label(screen, "Uhrdesign");
    click_label(screen, "Abbrechen");
    click_label(screen, "Zeiten");
    click_label(screen, "Medikamente: Stunden");
    click_label(screen, "Abbrechen");
    static uint64_t nav_time = UINT64_C(30000000);
    nav_time += 1000;
    swipe(35, 185, nav_time);
    click_label(screen, "Status");
    click_label(screen, "Zurück");
}

static void enter_keys(lv_obj_t *screen, const char *text)
{
    /* Replace the actual configured value now shown on opening the dialog. */
    for (unsigned i = 0; i < 32; ++i) click_label(screen, "<");
    for (const char *cursor = text; *cursor; ++cursor) {
        char key[] = {*cursor, '\0'};
        click_label(screen, key);
    }
}

/* Accepted-model projection must update an already open submenu without writes. */
static AppState option_model;
static LocalSettings option_local;
static ClockSample option_clock;
static bool option_auto;
static bool read_option_auto(void *context) {(void)context;return option_auto;}
static bool read_option_model(void *context, AppState *state,
                              LocalSettings *local, ClockSample *clock)
{
    (void)context;
    *state = option_model; *local = option_local; *clock = option_clock;
    return true;
}

static void assert_option(lv_obj_t *matrix, unsigned selected, unsigned count)
{
    assert(lv_buttonmatrix_get_selected_button(matrix) == selected);
    for (unsigned i = 0; i < count; ++i)
        assert(lv_buttonmatrix_has_button_ctrl(matrix, i,
                   LV_BUTTONMATRIX_CTRL_CHECKED) == (i == selected));
}

static void check_rotation_pointer(lv_display_t *display, Host *host)
{
    option_local.orientation = ORIENTATION_180;
    option_auto = false;
    host->result = UI_SUBMIT_QUEUED;
    ui_render(&option_model, &option_local, SETTINGS, &option_clock);
    lv_obj_t *caption=find_control_caption(lv_screen_active(),"Rotation"); assert(caption);
    lv_obj_scroll_to_view(lv_obj_get_parent(caption),LV_ANIM_OFF);
    pointer_click(display,lv_obj_get_parent(caption));
    lv_obj_t *matrix = find_matrix_with_first_button(lv_screen_active(), "0°");
    assert(matrix);
    lv_obj_scroll_to_y(lv_obj_get_parent(matrix), 0, LV_ANIM_OFF);
    lv_obj_update_layout(lv_screen_active());
    lv_area_t bounds;
    lv_obj_get_coords(matrix, &bounds);
    /* Read the real widget's layout, not a second implementation of hit testing. */
    const lv_area_t row = ((lv_buttonmatrix_t *)matrix)->button_areas[0];
    const int x0 = bounds.x1 + row.x1 + 2, x1 = bounds.x1 + row.x2 - 2;
    const int y0 = bounds.y1 + row.y1 + 2, y1 = bounds.y1 + row.y2 - 2;
    const lv_point_t points[] = {
        {(x0+x1)/2,(y0+y1)/2}, {x0,(y0+y1)/2}, {x1,(y0+y1)/2},
        {x0,y0}, {x1,y0}, {x0,y1}, {x1,y1}, {(x0+x1)/2,y0}, {(x0+x1)/2,y1},
        {(x0+x1)/2,(y0+y1)/2} /* center held across publication too */
    };
    lv_indev_t *pointer = lv_indev_create(); assert(pointer);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, display);
    lv_indev_set_read_cb(pointer, pointer_read);
    for(unsigned i=0;i<sizeof(points)/sizeof(points[0]);++i) {
        option_local.orientation=ORIENTATION_180;
        ui_render(&option_model,&option_local,SETTINGS,&option_clock);
        matrix=find_matrix_with_first_button(lv_screen_active(),"0°"); assert(matrix);
        const int calls=host->submit_calls;
        pointer_point=points[i]; pointer_down=true;
        lv_indev_read(pointer);
        assert(host->submit_calls==calls);
        /* Real app-loop publication can occur while the finger is held.
         * The immediate center tap is the reference; padding holds span renders. */
        if(i) for(unsigned tick=0;tick<3;++tick) {
            lv_tick_inc(25);
            ui_render(&option_model,&option_local,SETTINGS,&option_clock);
        }
        pointer_down=false; lv_indev_read(pointer);
        assert(host->submit_calls==calls+1);
        assert(host->last_request.kind==UI_REQUEST_SET_ORIENTATION);
        if(host->last_request.value.orientation!=ORIENTATION_0)
            fprintf(stderr,"rotation pointer point %u delivered wrong intent %u\n",i,
                    (unsigned)host->last_request.value.orientation);
        assert(host->last_request.value.orientation==ORIENTATION_0);
        assert(option_local.orientation==ORIENTATION_180); /* queued is not committed */
        const UiRequest queued=host->last_request;
        option_local.orientation=ORIENTATION_0;
        ui_deferred_result(&queued,UI_SUBMIT_COMMITTED);
        ui_render(&option_model,&option_local,SETTINGS,&option_clock);
        matrix=find_matrix_with_first_button(lv_screen_active(),"0°"); assert(matrix);
        assert_option(matrix,ORIENTATION_0,host->width==240 ? 4u : 5u);
    }
    const int before_cancel=host->submit_calls;
    pointer_point=points[0];pointer_down=true;lv_indev_read(pointer);
    pointer_point=(lv_point_t){0,0};lv_indev_read(pointer);
    lv_tick_inc(25);ui_render(&option_model,&option_local,SETTINGS,&option_clock);
    pointer_down=false;lv_indev_read(pointer);
    assert(host->submit_calls==before_cancel); /* leave/drag must not invent a selection */
    lv_indev_reset(pointer,NULL);lv_indev_delete(pointer);
    puts("rotation hitarea: native pointer center/padding/corners, held-render, exact single queued intent PASS");
}

/* Real LVGL pointer events plus the production gesture gates, never callback injection. */
static uint16_t drag_brightness(lv_display_t *display,Host *host,bool right)
{
    lv_obj_t *slider=find_slider(lv_screen_active());assert(slider);
    lv_obj_update_layout(lv_screen_active());lv_area_t area;lv_obj_get_coords(slider,&area);
    const int calls=host->submit_calls;const unsigned previews=host->preview_calls;
    const uint8_t stored=host->display_percent;
    const int y=(area.y1+area.y2)/2,span=area.x2-area.x1;
    const int from=area.x1+span*(right?1:4)/5,to=area.x1+span*(right?4:1)/5;
    lv_indev_t *pointer=lv_indev_create();assert(pointer);lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer,display);lv_indev_set_read_cb(pointer,pointer_read);
    static uint64_t ticks=20000000;ticks+=1000;
    ui_touch_sample(false,from,y,true,ticks);
    pointer_down=true;pointer_point=(lv_point_t){from,y};
    assert(ui_lvgl_touch_allowed(true,from,y,true,ticks+30));
    ui_touch_sample(true,from,y,true,ticks+30);lv_indev_read(pointer);
    for(int step=1;step<=4;++step){
        const int x=from+(to-from)*step/4;pointer_point=(lv_point_t){x,y};
        assert(ui_lvgl_touch_allowed(true,x,y,true,ticks+30+40*step));
        ui_touch_sample(true,x,y,true,ticks+30+40*step);lv_indev_read(pointer);
        const int32_t held=lv_slider_get_value(slider);
        option_clock.monotonic_ms=ticks+30+40*step;
        ui_render(&option_model,&option_local,SETTINGS,&option_clock);lv_tick_inc(25);lv_timer_handler();
        assert(lv_slider_get_value(slider)==held && ui_current_view()==SETTINGS);
        char percentage[12];snprintf(percentage,sizeof(percentage),"%u%%",(unsigned)held);
        assert(find_label(lv_screen_active(),percentage));
        assert(host->submit_calls==calls && host->display_percent==stored);
    }
    const uint16_t wanted=(uint16_t)lv_slider_get_value(slider);
    assert(wanted>=1 && wanted<=100 && wanted!=stored && host->preview_calls>previews);
    assert(host->preview_percent==wanted);
    pointer_down=false;
    assert(ui_lvgl_touch_allowed(false,to,y,true,ticks+250));
    ui_touch_sample(false,to,y,true,ticks+250);lv_indev_read(pointer);lv_indev_read(pointer);
    assert(ui_current_view()==SETTINGS && find_slider(lv_screen_active())==slider);
    assert(host->submit_calls==calls+1 && host->last_request.kind==UI_REQUEST_SET_BRIGHTNESS &&
           host->last_request.value.display_value==wanted && host->preview_percent==0);
    lv_indev_reset(pointer,NULL);lv_indev_delete(pointer);return wanted;
}

static int check_option_sync(int width, int height, bool hitarea_only)
{
    Host host = {.width = width, .height = height, .result = UI_SUBMIT_PENDING_SHARED};
    lv_init();
    assert(lv_display_create(width, height));
    option_clock = (ClockSample){.monotonic_ms=1000, .domain=17,
                                .quality=CLOCK_UNAVAILABLE};
    SharedSettings settings = app_default_settings();
    const uint8_t catalog[KATMED_CATALOG_BYTES] = {1};
    assert(app_init(&option_model, &settings, &option_clock,
                    (EventId){1,17,1}, 1, catalog) == APP_OK);
    option_model.alarm_active = false;
    option_local = (LocalSettings){.clock_style=DIGITAL_LARGE,
                                   .orientation=ORIENTATION_0};
    katmed_i18n_select(KATMED_LANG_EN);
    UiServices services = {.context=&host, .submit=submit, .read_model=read_option_model,
        .display_settings=read_display,.brightness_preview=preview_brightness};
    ui_set_services(&services);
    ui_render(&option_model, &option_local, SETTINGS, &option_clock);
    lv_obj_t *screen = lv_screen_active();
    if(hitarea_only) {
        services.orientation_auto_available=width!=240;
        services.orientation_automatic=read_option_auto;
        ui_set_services(&services);
        check_rotation_pointer(lv_display_get_default(),&host);
        return 0;
    }
    click_label(screen, "Slideshow / playback order");
    lv_obj_t *matrix = find_matrix_with_first_button(screen, "Off"); assert(matrix);
    const unsigned before = option_model.settings.rotation;
    const unsigned target = before == ROTATION_RANDOM ? ROTATION_NAME_ORDER : ROTATION_RANDOM;
    click_label(screen, lv_buttonmatrix_get_button_text(matrix, target));
    assert(host.last_request.kind == UI_REQUEST_SET_ROTATION_MODE);
    assert_option(matrix, before, 3); /* Pending does not predict coordinator acceptance. */
    ui_shared_result(false);
    ui_render(&option_model, &option_local, SETTINGS, &option_clock);
    assert_option(matrix, before, 3);
    click_label(screen, lv_buttonmatrix_get_button_text(matrix, target));
    option_model.settings.rotation = (RotationMode)target;
    ui_shared_result(true);
    ui_render(&option_model, &option_local, SETTINGS, &option_clock);
    assert_option(matrix, target, 3); /* Same live matrix, no submenu rebuild. */
    int calls = host.submit_calls;
    option_model.settings.rotation = ROTATION_OFF;
    ui_render(&option_model, &option_local, SETTINGS, &option_clock);
    assert_option(matrix, ROTATION_OFF, 3); /* Externally accepted state. */
    ui_render(&option_model, &option_local, SETTINGS, &option_clock);
    assert_option(matrix, ROTATION_OFF, 3);
    assert(host.submit_calls == calls);
    settings_parent(); click_label(screen, "Rotation");
    matrix = find_matrix_with_first_button(screen, "0°"); assert(matrix);
    assert_option(matrix, ORIENTATION_0, 4);
    host.result = UI_SUBMIT_REJECTED;
    click_label(screen, "90°");
    assert_option(matrix, ORIENTATION_0, 4);
    host.result = UI_SUBMIT_PENDING_SHARED;
    click_label(screen, "180°");
    assert_option(matrix, ORIENTATION_0, 4);
    ui_shared_result(false);
    assert_option(matrix, ORIENTATION_0, 4);
    host.result = UI_SUBMIT_COMMITTED;
    option_local.orientation = ORIENTATION_90;
    click_label(screen, "90°");
    assert_option(matrix, ORIENTATION_90, 4);
    ui_render(&option_model, &option_local, SETTINGS, &option_clock);
    matrix = find_matrix_with_first_button(screen, "0°"); assert(matrix);
    assert_option(matrix, ORIENTATION_90, 4);
    option_local.orientation = ORIENTATION_270;
    calls = host.submit_calls;
    ui_render(&option_model, &option_local, SETTINGS, &option_clock);
    matrix = find_matrix_with_first_button(screen, "0°"); assert(matrix);
    assert_option(matrix, ORIENTATION_270, 4);
    settings_parent(); click_label(screen, "Slideshow / playback order");
    matrix = find_matrix_with_first_button(screen, "Off"); assert(matrix);
    assert_option(matrix, ROTATION_OFF, 3);
    assert(host.submit_calls == calls && host.store_calls == 0);
    settings_parent();
    services.orientation_auto_available=true;
    services.orientation_automatic=read_option_auto;
    option_auto=true; ui_set_services(&services);
    ui_render(&option_model,&option_local,SETTINGS,&option_clock);
    click_label(screen,"Rotation");
    matrix=find_matrix_with_first_button(screen,"0°"); assert(matrix);
    assert_option(matrix,4,5);
    host.result=UI_SUBMIT_REJECTED;
    click_label(screen,"270°"); // same displayed angle, still a real manual request
    assert(host.last_request.kind==UI_REQUEST_SET_ORIENTATION);
    assert_option(matrix,4,5);
    host.result=UI_SUBMIT_COMMITTED; option_auto=false;
    click_label(screen,"270°"); assert_option(matrix,3,5);
    option_auto=true;
    click_label(screen,"Automatic");
    assert(host.last_request.kind==UI_REQUEST_SET_ORIENTATION_AUTOMATIC);
    assert_option(matrix,4,5);
    calls=host.submit_calls;
    settings_parent(); click_label(screen,"Rotation");
    matrix=find_matrix_with_first_button(screen,"0°"); assert_option(matrix,4,5);
    assert(host.submit_calls==calls);
    for(unsigned angle=0;angle<4;++angle){
        option_local.orientation=(Orientation)angle;
        ui_render(&option_model,&option_local,SETTINGS,&option_clock);
    }
    assert(image_orientations_seen==15u);
    settings_parent();click_label(screen,"Languages");
    char arabic_name[80];lv_text_ap_proc("العربية",arabic_name);
    assert(find_label(screen,arabic_name)); // native shaping is required even under English UI
    settings_parent();
    calls=host.submit_calls;host.display_percent=37;host.result=UI_SUBMIT_QUEUED;
    click_label(screen,"Change brightness");
    assert(find_slider(screen) && host.submit_calls==calls);
    assert(lv_slider_get_min_value(find_slider(screen))==1 && lv_slider_get_max_value(find_slider(screen))==100);
    assert(lv_slider_get_value(find_slider(screen))==37 && find_label(screen,"37%"));
    uint16_t wanted=drag_brightness(lv_display_get_default(),&host,true);
    assert(host.display_percent==37 && find_label(screen,"Change queued"));
    assert(lv_obj_has_state(find_slider(screen),LV_STATE_DISABLED));
    UiRequest queued=host.last_request;
    host.display_percent=(uint8_t)wanted;ui_deferred_result(&queued,UI_SUBMIT_COMMITTED);
    assert(!lv_obj_has_state(find_slider(screen),LV_STATE_DISABLED));
    assert(lv_slider_get_value(find_slider(screen))==wanted && find_label(screen,"Saved"));
    click_label(screen,"Cancel");click_label(screen,"Change brightness");
    assert(lv_slider_get_value(find_slider(screen))==wanted); /* reopen accepted value */
    host.result=UI_SUBMIT_UNAVAILABLE;
    (void)drag_brightness(lv_display_get_default(),&host,false);
    assert(lv_slider_get_value(find_slider(screen))==wanted); /* rollback, not saved */
    assert(find_label(screen,"Save currently not available"));
    host.result=UI_SUBMIT_QUEUED;
    (void)drag_brightness(lv_display_get_default(),&host,false);queued=host.last_request;
    ui_deferred_result(&queued,UI_SUBMIT_UNAVAILABLE);
    assert(lv_slider_get_value(find_slider(screen))==wanted &&
           !lv_obj_has_state(find_slider(screen),LV_STATE_DISABLED));
    (void)drag_brightness(lv_display_get_default(),&host,false);queued=host.last_request;
    click_label(screen,"Cancel");
    click_label(screen,"Configure night mode");
    host.result=UI_SUBMIT_COMMITTED;
    calls=host.submit_calls;click_label(screen,"Enable night mode");
    assert(host.submit_calls==calls+1 && host.last_request.kind==UI_REQUEST_SET_NIGHT_ENABLED);
    click_label(screen,"Set night mode globally on all devices in the mesh");
    assert(host.last_request.kind==UI_REQUEST_COPY_NIGHT && find_label(screen,"No global night copy yet"));
    click_label(screen,"Night starts HH:MM");calls=host.submit_calls;
    host.display_percent=(uint8_t)queued.value.display_value;ui_deferred_result(&queued,UI_SUBMIT_COMMITTED);
    assert(find_label(screen,"Night time HH:MM") && host.submit_calls==calls);
    click_label(screen,"Cancel");
    puts("brightness slider: native drag both ways, held render, percent, one release intent, queued/deferred/busy rollback/reopen/gesture capture PASS");
    ui_set_display_awake(false);
    assert(!ui_lvgl_touch_allowed(true,50,50,true,1000));
    ui_set_display_awake(true);
    assert(!ui_lvgl_touch_allowed(true,50,50,true,1000));
    assert(!ui_lvgl_touch_allowed(false,50,50,true,1001));
    assert(ui_lvgl_touch_allowed(true,50,50,true,1000));
    (void)ui_lvgl_touch_allowed(false,50,50,true,1001);
    puts("night UI: genuine controls, percent edit, explicit copy and complete wake-contact quarantine PASS");
    puts("option-sync: pending/rejected/accepted/external/orientation/navigation PASS");
    settings_parent();
    services.orientation_auto_available=width!=240;
    services.orientation_automatic=read_option_auto;
    ui_set_services(&services);
    check_rotation_pointer(lv_display_get_default(),&host);
    return 0;
}

/* Exercise the real shaping API with insufficient temporary heap. The output
 * buffer is caller-owned and sized for both original and shaped public text. */
static void text_shaping_allocation_failure(void)
{
    /* Legacy shaping lookup truncates supplementary codepoints to16bits.
     * Its nominal byte count can therefore be smaller than the raw UTF-8. */
    const char supplementary[] =
        "\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7"
        "\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7"
        "\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7"
        "\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7"
        "\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7"
        "\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7"
        "\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7"
        "\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7\xF0\x90\x98\xA7";
    const char *samples[] = {
        "A long public warning must remain readable when shaping has no heap.",
        "السلام عليكم السلام عليكم السلام عليكم", supplementary
    };
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        lv_mem_monitor_t before, limited, after;
        lv_mem_monitor(&before);
        void *pressure[2048];
        unsigned allocated = 0;
        while (allocated < sizeof(pressure) / sizeof(pressure[0])) {
            void *block = lv_malloc(32);
            if (!block) break;
            pressure[allocated++] = block;
        }
        assert(allocated && allocated < sizeof(pressure) / sizeof(pressure[0]));
        lv_mem_monitor(&limited);
        assert(limited.free_biggest_size <
               sizeof(uint32_t) * (lv_text_get_encoded_length(samples[i]) + 1));
        char output[256];
        assert(strlen(samples[i]) < sizeof(output));
        assert(lv_text_ap_strlen(samples[i]) + 1 < sizeof(output));
        const size_t capacity = lv_text_ap_strlen(samples[i]) + 1;
        output[capacity] = (char)0x5a;
        lv_text_ap_proc(samples[i], output);
        assert((unsigned char)output[capacity] == 0x5a);
        if (strlen(samples[i]) < capacity) {
            assert(strcmp(output, samples[i]) == 0);
            char inplace[256];
            strcpy(inplace, samples[i]);
            lv_text_ap_proc(inplace, inplace);
            assert(strcmp(inplace, samples[i]) == 0);
        } else {
            assert(output[0] == '\0');
        }
        while (allocated) lv_free(pressure[--allocated]);
        lv_mem_monitor(&after);
        assert(after.used_cnt == before.used_cnt);
        assert(after.free_size + after.free_cnt * sizeof(size_t) ==
               before.free_size + before.free_cnt * sizeof(size_t));
    }
    char shaped[256];
    lv_text_ap_proc(samples[1], shaped);
    assert(strcmp(shaped, samples[1]) != 0 &&
           "ordinary Arabic shaping must resume after temporary pressure");
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "option-sync") == 0)
        return check_option_sync(atoi(argv[2]), atoi(argv[3]), false);
    if (argc == 4 && strcmp(argv[1], "rotation-hitarea") == 0)
        return check_option_sync(atoi(argv[2]), atoi(argv[3]), true);
    assert(argc == 6);
    Host host = {.width = atoi(argv[1]), .height = atoi(argv[2]),
                 .result = UI_SUBMIT_COMMITTED, .reset_now_ms = 1000};
    int orientation_index = atoi(argv[3]);
    const char *dir = argv[4];
    const char *prefix = argv[5];
    assert(host.width > 0 && host.width <= 320 && host.height > 0 && host.height <= 320);
    assert(orientation_index >= ORIENTATION_0 && orientation_index <= ORIENTATION_270);
    host.frame = calloc((size_t)host.width * host.height, 2);
    uint8_t *draw_buffer = malloc((size_t)host.width * 16 * 2);
    assert(host.frame && draw_buffer);
    lv_init();
    text_shaping_allocation_failure();
    lv_display_t *display = lv_display_create(host.width, host.height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, (uint32_t)host.width * 16 * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_user_data(display, &host);
    report_memory("display");

    UiServices services = {.context = &host, .submit = submit, .connected = connected,
                           .network_status = network_status};
    ui_set_services(&services);
    LocalSettings local = {.orientation = (Orientation)orientation_index,
                           .clock_style = DIGITAL_LARGE};
    ClockSample clock = {.monotonic_ms = 0, .domain = 1,
                         .utc_ms = 1791973800000LL, .utc_valid = true,
                         .quality = CLOCK_SYNCHRONIZED};
    AppState state = {0};
    katmed_i18n_select(KATMED_LANG_DE); /* Existing surface assertions use German source copy. */
    state.alarm_active = true;
    state.start_reason = START_SETTINGS; /* Synthetic fixture is a due reminder. */
    state.image_id = 0;
    state.round.origin = (EventId){.device = 1, .epoch = 1, .sequence = 1};
    state.round.ordinal = 1;
    ui_render(&state, &local, BACKGROUND, &clock);
    assert(find_label(lv_screen_active(), "Bild nicht verfügbar") &&
           "UI must supply the German image fallback label");
    report_memory("alarm-start");
    assert(ui_current_view() == ALARM);

    /* A boot contact is discarded until release. */
    ui_touch_sample(true, 100, 100, true, 10);
    ui_touch_sample(false, 0, 0, true, 110);
    assert(host.submit_calls == 0);
    ui_touch_sample(true, 100, 100, true, 200);
    ui_touch_sample(false, 0, 0, true, 300);
    assert(host.submit_calls == 1 && host.store_calls == 1);
    ui_touch_sample(true, 100, 100, true, 310);
    ui_touch_sample(false, 0, 0, true, 380);
    assert(host.submit_calls == 1);
    state.round.ordinal = 2;
    ui_render(&state, &local, ALARM, &clock);
    ui_touch_sample(true, 100, 100, true, 400);
    ui_touch_sample(false, 0, 0, false, 450);
    ui_touch_sample(false, 0, 0, true, 500);
    assert(host.submit_calls == 1);
    ui_touch_sample(true, 100, 100, true, 600);
    ui_touch_sample(false, 0, 0, true, 1300);
    assert(host.submit_calls == 1);
    swipe(60, 130, 1400);
    assert(ui_current_view() == ALARM && host.submit_calls == 1);

    /* The path of a contact matters even if it returns to its start. */
    ui_touch_sample(true, 90, 90, true, 1600);
    ui_touch_sample(true, 170, 90, true, 1660);
    ui_touch_sample(true, 90, 90, true, 1700);
    ui_touch_sample(false, 0, 0, true, 1750);
    assert(host.submit_calls == 1);

    /* A pressed contact belongs to the complete alarm round at touch-down. */
    ui_touch_sample(true, 90, 90, true, 1800);
    state.round.origin.sequence++;
    ui_render(&state, &local, ALARM, &clock);
    ui_touch_sample(false, 0, 0, true, 1880);
    assert(host.submit_calls == 1);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_update_layout(screen);
    lv_obj_t *bar = find_alarm_bar(screen);
    assert(bar);
    int expected_y = host.height == 240 ? 30 : host.height == 320 ? 40 : 22;
    int expected_end = host.height == 240 ? 210 : host.height == 320 ? 280 : 151;
    assert(lv_obj_get_y(bar) == expected_y);
    assert(lv_obj_get_height(bar) == expected_end - expected_y);
    assert(lv_obj_get_style_bg_opa(bar, 0) == 217);
    /* Use a valid core alarm round for time-based presentation. Earlier
     * synthetic rounds intentionally exercise contact boundaries only. */
    AppState display_state = {0};
    SharedSettings display_settings = app_default_settings();
    ClockSample display_start = clock;
    const uint8_t display_catalog[KATMED_CATALOG_BYTES] = {1};
    assert(app_init(&display_state, &display_settings, &display_start,
                    (EventId){1, 1, 1}, 1, display_catalog) == APP_OK);
    clock.monotonic_ms = 2000;
    ui_render(&display_state, &local, ALARM, &clock);
    draw();
    assert(lv_obj_is_hidden(bar));
    unsigned first_strips = visible_alarm_strips(screen, bar);
    assert(first_strips > 0 && first_strips <= 6);
    char name[128];
    snprintf(name, sizeof(name), "%s_alarm_on", prefix);
    snapshot(&host, dir, name);
    clock.monotonic_ms = 2250;
    ui_render(&display_state, &local, ALARM, &clock);
    draw();
    assert(lv_obj_is_hidden(bar));
    unsigned second_strips = visible_alarm_strips(screen, bar);
    assert(second_strips < first_strips);
    snprintf(name, sizeof(name), "%s_alarm_off", prefix);
    snapshot(&host, dir, name);

    /* At all tempos the union of the existing six strips must cover exactly
     * the pixel interval, with neither holes nor a seven-step staircase. */
    const uint64_t stages[] = {0, 30 * 60000u, 60 * 60000u, 90 * 60000u, 120 * 60000u};
    for (unsigned stage = 0; stage < 5; ++stage) {
        int previous_edge = -1;
        unsigned distinct = 0;
        for (unsigned ms = 0; ms < 3000; ms += 10) {
            clock.monotonic_ms = stages[stage] + ms;
            AlarmAnimation animation;
            assert(app_alarm_animation(&display_state, &clock, &animation) == APP_OK);
            ui_render(&display_state, &local, ALARM, &clock);
            lv_obj_update_layout(screen);
            int total_height = expected_end - expected_y;
            int edge = animation.phase_permyriad < 5000 ?
                (animation.phase_permyriad * total_height + 4999) / 5000 :
                (animation.phase_permyriad - 5000) * total_height / 5000;
            int first = animation.phase_permyriad < 5000 ? 0 : edge;
            int last = animation.phase_permyriad < 5000 ? edge : total_height;
            int covered = 0;
            for (uint32_t n = 0; n < lv_obj_get_child_count(screen); ++n) {
                lv_obj_t *part = lv_obj_get_child(screen, n);
                if (part == bar || lv_obj_get_style_bg_opa(part, 0) != 217 ||
                    lv_obj_is_hidden(part)) continue;
                assert(lv_obj_get_y(part) == expected_y + first + covered);
                covered += lv_obj_get_height(part);
            }
            assert(covered == last - first);
            if (edge != previous_edge) ++distinct;
            previous_edge = edge;
        }
        assert(distinct > 80);
    }
    clock.monotonic_ms = 2250;

    /* Alarm beginning during a pressed contact discards its later release. */
    state.alarm_active = false;
    ui_render(&state, &local, ALARM, &clock);
    ui_touch_sample(false, 0, 0, true, 2300);
    ui_touch_sample(true, 90, 90, true, 2310);
    state.alarm_active = true;
    ui_render(&state, &local, BACKGROUND, &clock);
    ui_touch_sample(false, 0, 0, true, 2400);
    assert(host.submit_calls == 1);

    /* None of these service outcomes may be presented as a saved dose. */
    const UiSubmitResult failures[] = {UI_SUBMIT_UNAVAILABLE, UI_SUBMIT_REJECTED,
                                       UI_SUBMIT_PENDING, UI_SUBMIT_STALE};
    const char *messages[] = {"Speichern derzeit nicht verfügbar", "Änderung abgelehnt",
                              "Lokal gespeichert; Gruppe ausstehend", "Vorschau veraltet; erneut prüfen"};
    for (int i = 0; i < 4; ++i) {
        /* PENDING locally saves this round, so each service outcome needs a
         * fresh round before another confirmation request is meaningful. */
        state.round.ordinal++;
        ui_render(&state, &local, ALARM, &clock);
        host.result = failures[i];
        uint64_t t = 2500 + (uint64_t)i * 200;
        ui_touch_sample(true, 90, 90, true, t);
        ui_touch_sample(false, 0, 0, true, t + 80);
        assert(host.submit_calls == i + 2);
        assert(find_label(screen, messages[i]));
        assert(state.alarm_active);
    }
    host.result = UI_SUBMIT_COMMITTED;
    int before_unbound = host.submit_calls;
    ui_set_services(NULL);
    ui_touch_sample(true, 90, 90, true, 3300);
    ui_touch_sample(false, 0, 0, true, 3380);
    assert(host.submit_calls == before_unbound);
    assert(find_label(screen, "Speichern derzeit nicht verfügbar"));
    ui_set_services(&services);

    state.alarm_active = false;
    clock.monotonic_ms = 6000;
    ui_render(&state, &local, ALARM, &clock);
    swipe(60, 130, 6100);
    assert(ui_current_view() == CLOCK);
    ui_render(&state, &local, ui_current_view(), &clock);
    report_memory("clock");
    lv_obj_t *opening_clock = find_clock_view(screen);
    assert(opening_clock);
    lv_timer_handler();
    lv_tick_inc(100);
    lv_timer_handler();
    assert(lv_obj_get_x(opening_clock) == -host.width);
    lv_tick_inc(140);
    lv_timer_handler();
    assert(lv_obj_get_x(opening_clock) > -host.width && lv_obj_get_x(opening_clock) < 0);
    lv_tick_inc(100);
    lv_timer_handler();
    assert(lv_obj_get_x(opening_clock) == 0);
    draw();
    snprintf(name, sizeof(name), "%s_digital_large", prefix);
    snapshot(&host, dir, name);
    swipe(130, 60, 6300);
    assert(ui_current_view() == BACKGROUND);
    swipe(60, 130, 6500);
    assert(ui_current_view() == CLOCK);
    ui_touch_sample(true, 90, 90, true, 6700);
    state.alarm_active = true;
    ui_render(&state, &local, CLOCK, &clock);
    assert(ui_current_view() == ALARM);
    assert(find_clock_view(screen) == NULL);
    int pre_alarm_calls = host.submit_calls;
    ui_touch_sample(false, 0, 0, true, 6800);
    assert(host.submit_calls == pre_alarm_calls);
    state.alarm_active = false;
    ui_render(&state, &local, ALARM, &clock);
    swipe(60, 130, 6900);
    assert(ui_current_view() == CLOCK);

    local.clock_style = DIGITAL_DATE;
    clock.utc_ms = 1792015200000LL; /* 2026-10-15 00:00 in Berlin. */
    ui_render(&state, &local, CLOCK, &clock);
    draw();
    const char *clock_date = host.width == 172 ? "15.10.\n2026" : "15.10.2026";
    lv_obj_t *date = find_label(screen, clock_date);
    if (!date) {
        lv_obj_t *clock_debug = find_clock_view(screen);
        lv_obj_t *plate_debug = lv_obj_get_child(clock_debug, 0);
        fprintf(stderr, "date actual: %s\n",
                lv_label_get_text(lv_obj_get_child(plate_debug, 1)));
    }
    assert(date);
    snprintf(name, sizeof(name), "%s_digital_date_midnight", prefix);
    snapshot(&host, dir, name);
    Orientation original_orientation = local.orientation;
    local.orientation = (Orientation)((orientation_index + 2) % 4);
    ui_render(&state, &local, CLOCK, &clock);
    lv_obj_t *rotated_clock = find_clock_view(screen);
    assert(rotated_clock);
    assert(!lv_obj_is_hidden(rotated_clock));
    assert(find_label(screen, clock_date));
    local.orientation = original_orientation;
    ui_render(&state, &local, CLOCK, &clock);

    local.clock_style = ANALOG;
    clock.utc_ms = 1791973800000LL; /* 12:30 in Berlin. */
    ui_render(&state, &local, CLOCK, &clock);
    draw();
    lv_obj_t *clock_obj = find_clock_view(screen);
    assert(clock_obj);
    lv_obj_t *plate = lv_obj_get_child(clock_obj, 0);
    lv_obj_t *dial = lv_obj_get_child(clock_obj, 1);
    assert(lv_obj_get_style_bg_opa(plate, 0) == LV_OPA_TRANSP);
    assert(lv_obj_get_style_border_width(dial, 0) == 0);
    for (unsigned mark = 0; mark < 12; ++mark)
        assert(lv_obj_is_hidden(lv_obj_get_child(dial, mark)));
    lv_obj_t *hour_outline = lv_obj_get_child(dial, 12);
    lv_obj_t *hour = lv_obj_get_child(dial, 13);
    lv_obj_t *minute_outline = lv_obj_get_child(dial, 14);
    lv_obj_t *minute_hand = lv_obj_get_child(dial, 15);
    lv_obj_t *second_outline = lv_obj_get_child(dial, 16);
    lv_obj_t *second_hand = lv_obj_get_child(dial, 17);
    lv_obj_t *hub_outline = lv_obj_get_child(dial, 18);
    lv_obj_t *hub = lv_obj_get_child(dial, 19);
    assert(lv_obj_get_style_line_width(second_outline, 0) == 3);
    assert(lv_obj_get_style_line_width(second_hand, 0) == 1);
    assert(lv_obj_get_style_line_rounded(second_outline, 0));
    assert(lv_color_to_u32(lv_obj_get_style_line_color(second_outline, 0)) ==
           lv_color_to_u32(lv_color_black()));
    assert(lv_color_to_u32(lv_obj_get_style_border_color(hub_outline, 0)) ==
           lv_color_to_u32(lv_color_black()));
    assert(lv_obj_get_style_border_width(hub, 0) == 2);
    assert(lv_obj_get_style_border_width(hub_outline, 0) == 3);
    assert(lv_obj_get_width(hub_outline) == lv_obj_get_width(hub) + 2);
    assert(!lv_obj_is_clickable(second_outline));
    assert(!lv_obj_is_clickable(hub_outline));
    assert(lv_obj_get_style_bg_opa(hub_outline, 0) == LV_OPA_TRANSP);
    assert(lv_obj_get_style_bg_opa(hub, 0) == LV_OPA_TRANSP);
    lv_obj_t *outlines[] = {hour_outline, minute_outline};
    for (unsigned h = 0; h < 2; ++h) {
        assert(lv_obj_get_style_line_width(outlines[h], 0) ==
               (lv_obj_get_width(dial) < 240 ? 4 : 5));
        assert(lv_color_to_u32(lv_obj_get_style_line_color(outlines[h], 0)) ==
               lv_color_to_u32(lv_color_black()));
        assert(!lv_obj_is_clickable(outlines[h]));
    }
    assert(lv_obj_get_width(dial) == (host.width < host.height ? host.width : host.height));
    assert(lv_color_to_u32(lv_obj_get_style_line_color(hour, 0)) ==
           lv_color_to_u32(lv_color_hex(0x6baede)));
    assert(lv_color_to_u32(lv_obj_get_style_line_color(minute_hand, 0)) ==
           lv_color_to_u32(lv_color_hex(0xffba19)));
    assert(lv_color_to_u32(lv_obj_get_style_line_color(second_hand, 0)) ==
           lv_color_to_u32(lv_color_hex(0xbdd731)));
    /* Check projected real-time tips, including seconds within the same minute.
     * This exercises ui_render's cache as well as the actual shared LVGL objects. */
    const unsigned offsets_to_check[] = {0, 15, 30, 45, 59, 60, 11730};
    for (unsigned sample = 0; sample < 7; ++sample) {
        unsigned offset = offsets_to_check[sample];
        unsigned sec = offset % 60;
        unsigned minutes = (30 + offset / 60) % 60;
        unsigned hours = (12 + (30 + offset / 60) / 60) % 12;
        clock.utc_ms = 1791973800000LL + offset * 1000;
        ui_render(&state, &local, CLOCK, &clock);
        draw();
        double center = lv_obj_get_width(dial) / 2;
        double scale = lv_obj_get_width(dial) / 240.0;
        const double angles[] = {(hours + (minutes + sec / 60.0) / 60.0) *
                                 2 * 3.141592653589793 / 12,
                                 (minutes + sec / 60.0) * 2 * 3.141592653589793 / 60,
                                 sec * 2 * 3.141592653589793 / 60};
        lv_obj_t *hands[] = {hour, minute_hand, second_hand};
        const double radii[] = {58, 95, 97};
        for (unsigned h = 0; h < 3; ++h) {
            const lv_point_precise_t *points = lv_line_get_points(hands[h]);
            unsigned tip = h == 2 ? 1 : 2;
            double dx = points[tip].x - center, dy = points[tip].y - center;
            assert(fabs(dx - scale * radii[h] * sin(angles[h])) <= 0.6);
            assert(fabs(dy + scale * radii[h] * cos(angles[h])) <= 0.6);
            assert(hypot(dx, dy) > scale * radii[h] - 1);
            assert(lv_line_get_point_count(hands[h]) == (h == 2 ? 2 : 6));
        }
        for (unsigned h = 0; h < 2; ++h) {
            assert(lv_line_get_points(outlines[h]) == lv_line_get_points(hands[h]));
            assert(lv_line_get_point_count(outlines[h]) == 6);
            assert(lv_obj_get_style_line_width(hands[h], 0) == 3);
        }
        assert(lv_line_get_points(second_outline) == lv_line_get_points(second_hand));
        assert(lv_line_get_point_count(second_outline) == 2);
        const lv_point_precise_t *second_points = lv_line_get_points(second_hand);
        assert(fabs(second_points[0].x - center + scale * 17 * sin(angles[2])) <= .6);
        assert(fabs(second_points[0].y - center - scale * 17 * cos(angles[2])) <= .6);
        /* All three reference hues reach actual rendered pixels. */
        const uint16_t colors[] = {0x6d7b, 0xfdc3, 0xbea6};
        for (unsigned h = 0; h < 3; ++h) {
            unsigned count = 0;
            for (int pixel = 0; pixel < host.width * host.height; ++pixel)
                count += host.frame[pixel] == colors[h];
            assert(count > 5);
        }
        if (offset == 0) {
            /* Compare the real framebuffer with/without just the underlays.
             * Newly black pixels must border both sides of each colored stroke,
             * including the side facing the transparent polygon interior. */
            size_t bytes = (size_t)host.width * host.height * sizeof(uint16_t);
            uint16_t *without_outline = malloc(bytes);
            assert(without_outline);
            for (unsigned h = 0; h < 2; ++h)
                lv_obj_set_hidden(outlines[h], true);
            lv_obj_set_hidden(second_outline, true);
            lv_obj_set_hidden(hub_outline, true);
            draw();
            memcpy(without_outline, host.frame, bytes);
            for (unsigned h = 0; h < 2; ++h)
                lv_obj_set_hidden(outlines[h], false);
            lv_obj_set_hidden(second_outline, false);
            lv_obj_set_hidden(hub_outline, false);
            draw();
            lv_area_t area;
            lv_obj_get_coords(dial, &area);
            for (unsigned h = 0; h < 3; ++h) {
                const lv_point_precise_t *p = lv_line_get_points(hands[h]);
                double dx = p[1].x - p[0].x, dy = p[1].y - p[0].y;
                double length = hypot(dx, dy);
                unsigned sides[2] = {0, 0}, colored = 0;
                for (int y = 0; y < host.height; ++y)
                    for (int x = 0; x < host.width; ++x) {
                        double rx = x - area.x1 - p[0].x;
                        double ry = y - area.y1 - p[0].y;
                        double along = (rx * dx + ry * dy) / length;
                        double across = (rx * dy - ry * dx) / length;
                        if (along < length * .25 || along > length * .75 ||
                            fabs(across) > 4) continue;
                        size_t pixel = (size_t)y * host.width + x;
                        if (host.frame[pixel] == colors[h]) ++colored;
                        uint16_t a = host.frame[pixel], b = without_outline[pixel];
                        /* One-pixel antialiased black rims blend with the image. */
                        if (a != b && (a >> 11) <= (b >> 11) &&
                            ((a >> 5) & 63) <= ((b >> 5) & 63) &&
                            (a & 31) <= (b & 31)) ++sides[across > 0];
                    }

                printf("outline hand=%u black_sides=%u,%u colored=%u\n",
                       h, sides[0], sides[1], colored);
                fflush(stdout);
                assert(sides[0] > 1 && sides[1] > 1 && colored > 5);
            }
            free(without_outline);
            /* Interior of the long minute outline must reveal the background. */
            int x = host.width / 2;
            int y = host.height / 2 + (int)lround(60 * scale);
            if (lv_obj_get_width(dial) >= 240) {
                assert(host.frame[y * host.width + x] == test_image[y * host.width + x]);
            } else {
                /* On the 172px dial, rounding moves the narrow open center by
                 * a pixel. Require actual untouched interior pixels, not merely
                 * a transparent style property, in its central strip. */
                unsigned clear = 0;
                for (int radial = 30; radial < 85; ++radial)
                    for (int across = -1; across <= 1; ++across) {
                        int py = host.height / 2 + (int)lround(radial * scale);
                        size_t pixel = (size_t)py * host.width + x + across;
                        clear += host.frame[pixel] == test_image[pixel];
                    }
                assert(clear > 1);
            }
        }
        if (offset == 15) {
            /* Isolate only the new seconds underlays on the real framebuffer.
             * The forward arm, counterarm and transparent hub are all covered. */
            size_t bytes = (size_t)host.width * host.height * sizeof(uint16_t);
            uint16_t *without_seconds = malloc(bytes);
            assert(without_seconds);
            lv_obj_set_hidden(second_outline, true);
            lv_obj_set_hidden(hub_outline, true);
            draw();
            memcpy(without_seconds, host.frame, bytes);
            lv_obj_set_hidden(second_outline, false);
            lv_obj_set_hidden(hub_outline, false);
            draw();
            lv_area_t area;
            lv_obj_get_coords(dial, &area);
            const lv_point_precise_t *p = lv_line_get_points(second_hand);
            for (unsigned arm = 0; arm < 2; ++arm) {
                unsigned sides[2] = {0, 0}, colored = 0;
                int first = (int)lround((arm ? -15 : 25) * scale);
                int last = (int)lround((arm ? -10 : 85) * scale);
                for (int radial = first; radial <= last; ++radial)
                    for (int across = -2; across <= 2; ++across) {
                        int x = area.x1 + (int)center + radial;
                        int y = area.y1 + (int)p[0].y + across;
                        size_t pixel = (size_t)y * host.width + x;
                        uint16_t a = host.frame[pixel], b = without_seconds[pixel];
                        colored += a == colors[2];
                        if (a != b && (a >> 11) <= (b >> 11) &&
                            ((a >> 5) & 63) <= ((b >> 5) & 63) &&
                            (a & 31) <= (b & 31)) ++sides[across > 0];
                    }
                printf("seconds arm=%u black_sides=%u,%u colored=%u\n",
                       arm, sides[0], sides[1], colored);
                assert(sides[0] > 0 && sides[1] > 0 && colored > 0);
            }
            for (unsigned end = 0; end < 2; ++end) {
                int x = area.x1 + (int)p[end].x + (end ? 1 : -1);
                int y = area.y1 + (int)p[end].y;
                size_t pixel = (size_t)y * host.width + x;
                assert(host.frame[pixel] != without_seconds[pixel]);
                assert(host.frame[pixel] == 0); /* Rounded black endpoint cap. */
            }
            lv_area_t hub_area;
            lv_obj_get_coords(hub, &hub_area);
            unsigned rim = 0, clear = 0, green = 0;
            for (int y = hub_area.y1 - 1; y <= hub_area.y2 + 1; ++y)
                for (int x = hub_area.x1 - 1; x <= hub_area.x2 + 1; ++x) {
                    size_t pixel = (size_t)y * host.width + x;
                    rim += host.frame[pixel] != without_seconds[pixel];
                    green += host.frame[pixel] == colors[2];
                    if (x > hub_area.x1 + 2 && x < hub_area.x2 - 2 &&
                        y > hub_area.y1 + 2 && y < hub_area.y2 - 2)
                        clear += host.frame[pixel] == test_image[pixel];
                }
            printf("seconds hub rim=%u green=%u clear=%u\n", rim, green, clear);
            assert(rim > 0 && green > 0);
            /* Isolate the ring opening from the unchanged hands behind it. */
            lv_obj_t *behind_hub[] = {hour_outline, hour, minute_outline,
                                     minute_hand, second_outline, second_hand};
            for (unsigned h = 0; h < 6; ++h)
                lv_obj_set_hidden(behind_hub[h], true);
            draw();
            clear = 0;
            for (int y = hub_area.y1 + 2; y <= hub_area.y2 - 2; ++y)
                for (int x = hub_area.x1 + 2; x <= hub_area.x2 - 2; ++x) {
                    size_t pixel = (size_t)y * host.width + x;
                    clear += host.frame[pixel] == test_image[pixel];
                }
            printf("seconds isolated hub clear=%u\n", clear);
            assert(clear > 0);
            for (unsigned h = 0; h < 6; ++h)
                lv_obj_set_hidden(behind_hub[h], false);
            draw();
            free(without_seconds);
            snprintf(name, sizeof(name), "%s_analog_123015", prefix);
            snapshot(&host, dir, name);
        }
    }

    clock.utc_valid = false;
    clock.quality = CLOCK_UNAVAILABLE;
    ui_render(&state, &local, CLOCK, &clock);
    draw();
    assert(find_label(screen, "--:--") || find_label(screen,"--:\n--"));
    assert(find_label(screen, "Uhrzeit nicht verfügbar"));
    assert(lv_obj_get_style_bg_opa(plate, 0) == LV_OPA_TRANSP);
    assert(lv_obj_get_style_border_width(dial, 0) == 0);
    /* Re-enter with the same unavailable time and persisted analog style:
     * a new view must apply style even when every cached value is unchanged. */
    ui_render(&state, &local, SETTINGS, &clock);
    assert(find_clock_view(screen) == NULL);
    ui_render(&state, &local, CLOCK, &clock);
    clock_obj = find_clock_view(screen);
    plate = lv_obj_get_child(clock_obj, 0);
    dial = lv_obj_get_child(clock_obj, 1);
    assert(lv_obj_get_style_bg_opa(plate, 0) == LV_OPA_TRANSP);
    assert(lv_obj_get_style_border_width(dial, 0) == 0);
    assert(lv_obj_is_hidden(dial));
    snprintf(name, sizeof(name), "%s_clock_unavailable", prefix);
    snapshot(&host, dir, name);
    clock.utc_valid = true;
    clock.quality = CLOCK_HOLDOVER;
    ui_render(&state, &local, CLOCK, &clock);
    draw();
    assert(find_label(screen, "Zeit nicht synchronisiert"));
    snprintf(name, sizeof(name), "%s_clock_holdover", prefix);
    snapshot(&host, dir, name);

    swipe(130, 60, 7100);
    assert(ui_current_view() == BACKGROUND);
    swipe(130, 60, 7300);
    assert(ui_current_view() == SETTINGS);
    ui_render(&state, &local, SETTINGS, &clock);
    report_memory("settings");
    draw();
    snprintf(name, sizeof(name), "%s_settings", prefix);
    snapshot(&host, dir, name);
    lv_obj_t *placeholder = find_label(screen,
                                       "Lade Bilder von URL/Github\nSpäter verfügbar");
    assert(placeholder);
    lv_obj_t *import = lv_obj_get_parent(placeholder);
    assert(lv_obj_has_state(import, LV_STATE_DISABLED));
    int previous = host.submit_calls;
    int old_network = host.network_calls;
    int old_store = host.store_calls;
    lv_obj_send_event(import, LV_EVENT_CLICKED, NULL);
    assert(host.submit_calls == previous && host.network_calls == old_network &&
           host.store_calls == old_store);
    assert(ui_current_view() == SETTINGS);
    click_label(screen, "Rotation");
    const char *angle_names[] = {"0°", "90°", "180°", "270°"};
    for (int i = 0; i < 4; ++i) {
        click_label(screen, angle_names[i]);
        assert(host.last_request.kind == UI_REQUEST_SET_ORIENTATION);
        assert(host.last_request.value.orientation == (Orientation)i);
    }
    settings_parent();click_label(screen,"Diashow / Abspielreienfolge");
    const char *rotation_names[] = {"Aus", "Nach Namen", "Zufall"};
    for (int i = 0; i < 3; ++i) {
        click_label(screen, rotation_names[i]);
        assert(host.last_request.kind == UI_REQUEST_SET_ROTATION_MODE);
        assert(host.last_request.value.rotation == (RotationMode)i);
    }

    settings_parent();
    int before_style = host.submit_calls;
    click_label(screen, "Uhrdesign");
    report_memory("style-dialog");
    draw();
    snprintf(name, sizeof(name), "%s_style_dialog", prefix);
    snapshot(&host, dir, name);
    click_label(screen, "Digital groß");
    assert(host.submit_calls == before_style);
    click_label(screen, "Abbrechen");
    assert(host.submit_calls == before_style && local.clock_style == ANALOG);
    click_label(screen, "Uhrdesign");
    click_label(screen, "Digital mit Datum");
    host.result = UI_SUBMIT_REJECTED;
    click_label(screen, "Speichern");
    assert(host.submit_calls == before_style + 1);
    assert(find_label(screen, "Änderung abgelehnt"));
    assert(local.clock_style == ANALOG);
    host.result = UI_SUBMIT_COMMITTED;
    click_label(screen, "Speichern");
    assert(host.submit_calls == before_style + 2);
    assert(host.last_request.kind == UI_REQUEST_SET_CLOCK_STYLE);
    assert(host.last_request.value.clock_style == DIGITAL_DATE);
    assert(local.clock_style == ANALOG);
    lv_obj_t *saved_feedback = find_label(screen, "Gespeichert");
    lv_obj_t *rebuilt_menu = lv_obj_get_parent(find_label(screen, "Einstellungen"));
    assert(saved_feedback && rebuilt_menu);
    assert(!lv_obj_is_hidden(saved_feedback));
    assert(lv_obj_get_index(saved_feedback) > lv_obj_get_index(rebuilt_menu));
    draw();
    snprintf(name, sizeof(name), "%s_style_saved_feedback", prefix);
    snapshot(&host, dir, name);

    clock.quality = CLOCK_SYNCHRONIZED;
    clock.utc_ms = 1774744200000LL; /* 2026-03-29 01:30 CET. */
    ui_render(&state, &local, SETTINGS, &clock);
    click_label(screen, "Sprachen");
    assert(find_label(screen, "Ostfränkisch"));
    assert(find_label(screen, "tlhIngan Hol"));
    int before_language = host.submit_calls;
    click_label(screen, "Ostfränkisch");
    assert(host.submit_calls == before_language + 1 &&
           host.last_request.kind == UI_REQUEST_SET_LANGUAGE &&
           host.last_request.value.language == KATMED_LANG_BAR);
    click_label(screen, "tlhIngan Hol");
    assert(host.submit_calls == before_language + 2 &&
           host.last_request.kind == UI_REQUEST_SET_LANGUAGE &&
           host.last_request.value.language == KATMED_LANG_TLH_LATN);
    settings_parent();click_label(screen,"Zeiten");
    click_label(screen, "Nächste Fälligkeit HH:MM");
    report_memory("time-keypad");
    click_label(screen, "0");
    click_label(screen, "2");
    /* The HHMM keypad inserts its separator after digit entry. */
    click_label(screen, "3");
    click_label(screen, "0");
    lv_obj_t *skip = find_label_prefix(screen, "02:30 heute nicht vorhanden");
    assert(skip && strstr(lv_label_get_text(skip), "30.03.2026"));
    draw();
    snprintf(name, sizeof(name), "%s_dst_gap_preview", prefix);
    snapshot(&host, dir, name);
    click_label(screen, "Abbrechen");
    clock.utc_ms = 1791973800000LL;
    ui_render(&state, &local, SETTINGS, &clock);
    click_label(screen, "Nächste Fälligkeit HH:MM");
    click_label(screen, "1");
    click_label(screen, "2");
    /* The HHMM keypad inserts its separator after digit entry. */
    click_label(screen, "3");
    click_label(screen, "0");
    assert(find_label_prefix(screen, "Wirksam:"));
    draw();
    snprintf(name, sizeof(name), "%s_hhmm_preview", prefix);
    snapshot(&host, dir, name);
    click_label(screen, "Speichern");
    assert(host.last_request.kind == UI_REQUEST_SET_NEXT_LOCAL_TIME);
    assert(host.last_request.value.local_time.hour == 12);
    assert(host.last_request.value.local_time.minute == 30);
    assert(host.last_request.value.local_time.preview.effective_utc.valid);
    state.has_last_confirmation = true;
    state.last_confirmation.utc_valid = true;
    state.last_confirmation.utc_ms = 1791973800000LL;
    state.deadline.effective_utc = (UtcInstant){true, 1792015200000LL};
    state.confirmation_pending_sync = true;
    ui_render(&state, &local, SETTINGS, &clock);
    snprintf(host.network.device_name, sizeof(host.network.device_name), "KatmedESP-002");
    host.network.device_name_assigned = true;
    click_label(screen, "Status");
    report_memory("status");
    lv_obj_t *feedback = find_label(screen, "Gespeichert");
    assert(feedback && lv_obj_is_hidden(feedback));
    assert(find_label_prefix(screen, "Letzte Gabe: 14.10.2026"));
    assert(find_label_prefix(screen, "Nächster Alarm: 15.10.2026"));
    assert(find_label(screen, "Gerätename: KatmedESP-002"));
    assert(find_label(screen, "Bestätigung noch nicht synchronisiert"));
    draw();
    snprintf(name, sizeof(name), "%s_status", prefix);
    snapshot(&host, dir, name);
    lv_obj_t *status_panel = lv_obj_get_parent(find_label_prefix(screen, "Letzte Gabe:"));
    assert(!find_label(status_panel, "Zurück"));
    click_label(screen, "Zurück");
    click_label(screen, "Bilder: Stunden");
    assert(find_label(screen, "Positive Stundenzahl eingeben"));
    local.orientation = (Orientation)((orientation_index + 2) % 4);
    ui_render(&state, &local, SETTINGS, &clock);
    assert(find_label(screen, "Eingabe nach Drehung verworfen"));
    assert(!find_label(screen, "Positive Stundenzahl eingeben"));
    click_label(screen, "Medikamente: Stunden");
    assert(find_label(screen, "Positive Stundenzahl eingeben"));
    int calls_before_invalid = host.submit_calls;
    click_label(screen, "Speichern");
    assert(host.submit_calls == calls_before_invalid);
    click_label(screen, "Abbrechen");

    lv_mem_monitor_t before_cycles, after_cycles;
    swipe(35, 185, 2500);
    cycle_views(screen);
    lv_mem_monitor(&before_cycles);
#ifdef KATMED_SURFACE_ALLOC_ACCOUNTING
    size_t requested_count_before, requested_bytes_before;
    surface_live_requests(&requested_count_before, &requested_bytes_before);
    assert(requested_count_before == before_cycles.used_cnt);
#endif
    for (int i = 0; i < 50; ++i) cycle_views(screen);
    lv_mem_monitor(&after_cycles);
    if (after_cycles.free_size < before_cycles.free_size)
        fprintf(stderr, "cycle memory before=%zu after=%zu delta=%zu\n",
                before_cycles.free_size, after_cycles.free_size,
                before_cycles.free_size - after_cycles.free_size);
#ifdef KATMED_SURFACE_ALLOC_ACCOUNTING
    size_t requested_count_after, requested_bytes_after;
    surface_live_requests(&requested_count_after, &requested_bytes_after);
    assert(requested_count_after == after_cycles.used_cnt);
    assert(requested_count_after <= requested_count_before);
    assert(requested_bytes_after <= requested_bytes_before);
#else
    assert(after_cycles.free_size >= before_cycles.free_size);
#endif

    /* A direct view switch must free the old menu before creating the clock. */
    ui_render(&state, &local, CLOCK, &clock);
    assert(ui_current_view() == CLOCK && find_clock_view(screen));
    assert(!find_label(screen, "Einstellungen"));
    ui_render(&state, &local, SETTINGS, &clock);
    assert(ui_current_view() == SETTINGS && find_label(screen, "Einstellungen"));
    ui_set_pairing_code("314159");
    assert(find_label(screen, "Web-Code: 314159"));
    ui_render(&state, &local, UI_VIEW_STATUS, &clock);
    assert(find_label(screen, "Web-Code: 314159"));
    ui_clear_pairing_code();
    assert(!find_label(screen, "Web-Code: 314159"));
    ui_set_pairing_code("12345x");
    assert(!find_label_prefix(screen, "Web-Code:"));
    ui_set_pairing_code("271828");
    ui_render(&state, &local, BACKGROUND, &clock);
    ui_render(&state, &local, SETTINGS, &clock);
    assert(find_label(screen, "Web-Code: 271828"));
    ui_clear_pairing_code();
    click_label(screen, "Uhrdesign");
    assert(find_label(screen, "Uhrdesign wählen"));
    ui_render(&state, &local, CLOCK, &clock);
    assert(ui_current_view() == CLOCK && find_clock_view(screen));
    assert(!find_label(screen, "Uhrdesign wählen"));
    ui_render(&state, &local, SETTINGS, &clock);
    assert(find_label(screen, "Einstellungen"));
    const char *hour_values[] = {"1.5", "1,5", "0", "999999999999999999999999",
                                 "1..2"};
    click_label(screen, "Zeiten");
    for (unsigned i = 0; i < 5; ++i) {
        int before = host.submit_calls;
        click_label(screen, "Bilder: Stunden");
        enter_keys(screen, hour_values[i]);
        click_label(screen, "Speichern");
        if (i < 2) {
            assert(host.submit_calls == before + 1);
            assert(host.last_request.value.seconds == 5400);
        } else {
            if (host.submit_calls != before)
                fprintf(stderr, "invalid hours '%s' reached service\n", hour_values[i]);
            assert(host.submit_calls == before);
            click_label(screen, "Abbrechen");
        }
    }
    swipe(35, 185, 31000);
    click_label(screen,"Diashow / Abspielreienfolge");
    lv_obj_t *rotation_matrix = find_matrix_with_first_button(screen, "Aus");assert(rotation_matrix);
    for (uint32_t i = 0; i < 3; ++i)
        assert(lv_buttonmatrix_has_button_ctrl(rotation_matrix, i,
               LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT));
    settings_parent();click_label(screen,"Rotation");
    lv_obj_t *angle_matrix = find_matrix_with_first_button(screen, "0°");assert(angle_matrix);
    for (uint32_t i = 0; i < 4; ++i)
        assert(lv_buttonmatrix_has_button_ctrl(angle_matrix, i,
               LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT));

    lv_indev_t *pointer = lv_indev_create();
    assert(pointer);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, display);
    lv_indev_set_read_cb(pointer, pointer_read);
    lv_obj_scroll_to_view(angle_matrix, LV_ANIM_OFF);
    lv_obj_update_layout(screen);
    lv_area_t matrix_area;
    lv_obj_get_coords(angle_matrix, &matrix_area);
    pointer_point.x = (matrix_area.x1 + matrix_area.x2) / 2;
    pointer_point.y = matrix_area.y1 + 16;
    assert(pointer_point.x >= 0 && pointer_point.x < host.width);
    assert(pointer_point.y >= 0 && pointer_point.y < host.height);
    int calls_before_pointer = host.submit_calls;
    pointer_down = true;
    lv_indev_read(pointer);
    assert(host.submit_calls == calls_before_pointer);
    lv_tick_inc(1000);
    lv_indev_read(pointer);
    assert(host.submit_calls == calls_before_pointer);
    pointer_down = false;
    lv_indev_read(pointer);
    assert(host.submit_calls == calls_before_pointer + 1);
    assert(host.last_request.kind == UI_REQUEST_SET_ORIENTATION);
    assert(host.last_request.value.orientation == ORIENTATION_0);
    pointer_down = true;
    lv_indev_read(pointer);
    assert(host.submit_calls == calls_before_pointer + 1);
    lv_indev_reset(pointer, NULL);
    pointer_down = false;
    lv_indev_read(pointer);
    assert(host.submit_calls == calls_before_pointer + 1);
    lv_indev_delete(pointer);
    draw();
    report_memory("final");

    settings_parent();
    /* Settings right -> home, then vertical home gesture reuses status. */
    ui_render(&state, &local, SETTINGS, &clock);
    swipe(60, 140, 50000);
    assert(ui_current_view() == BACKGROUND);
    ui_render(&state, &local, BACKGROUND, &clock);
    assert(!find_label(screen, "Einstellungen"));
    ui_touch_sample(true, 100, 180, true, 51000);
    ui_touch_sample(true, 100, 80, true, 51100);
    ui_touch_sample(false, 0, 0, true, 51200);
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Status"));
    assert(find_label(screen, "WLAN: nicht konfiguriert"));
    assert(find_label(screen, "Mesh-Funkkanal: inaktiv"));
    host.network = (UiNetworkStatus){.configured = true, .connected = true};
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "DNS-IP: unbekannt"));
    assert(find_label(screen, "Zeitserver-IP (letzter lokaler Sync): unbekannt"));
    host.network = (UiNetworkStatus){.configured = true, .connected = true,
        .dns_ip = "192.0.2.53", .sync_server_ip = "203.0.113.1",
        .mesh_active = true, .mesh_channel = 6,
        .mesh_current_count = 2, .mesh_history_count = 3, .mesh_quality = 16,
        .mesh_current_valid = true, .mesh_history_complete = true,
        .station_ip = "192.0.2.10", .gateway = "192.0.2.1", .ap_ip = "192.0.2.129"};
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh-Funkkanal: 6"));
    assert(find_label(screen, "IP: 192.0.2.10"));
    assert(find_label(screen, "Gateway: 192.0.2.1"));
    assert(find_label(screen, "AP-IP: 192.0.2.129"));
    assert(find_label(screen, "DNS-IP: 192.0.2.53"));
    assert(find_label(screen, "Zeitserver-IP (letzter lokaler Sync): 203.0.113.1"));
    assert(find_label(screen, "Mesh aktuell (inkl. Gerät): 2"));
    assert(find_label(screen, "Mesh bisher erfasst (ab Einführung): 3"));
    host.network.mesh_quality = 2;
    host.network.mesh_history_complete = false;
    clock.monotonic_ms += 1000;ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh bisher erfasst (ab Einführung): mindestens 3 (Speicherfehler)"));
    host.network.mesh_quality = 1;
    clock.monotonic_ms += 1000;ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh bisher erfasst (ab Einführung): mindestens 3 (Speicher voll)"));
    host.network.mesh_quality = 4;
    clock.monotonic_ms += 1000;ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh aktuell (inkl. Gerät): mindestens 2 (Wiederaufbau)"));
    assert(find_label(screen, "Mesh bisher erfasst (ab Einführung): mindestens 3 (Abgleich)"));
    host.network.mesh_quality = 8;
    clock.monotonic_ms += 1000;ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh aktuell (inkl. Gerät): mindestens 2 (unvollständig)"));
    host.network.mesh_current_valid = false;
    clock.monotonic_ms += 1000;ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh aktuell (inkl. Gerät): unbekannt"));
    host.network.mesh_quality = 16;host.network.mesh_current_valid = true;
    host.network.mesh_history_complete = true;
    clock.monotonic_ms += 1000;ui_render(&state, &local, ui_current_view(), &clock);
    lv_obj_t *server_label = find_label(screen, "Zeitserver-IP (letzter lokaler Sync): 203.0.113.1");
    lv_obj_update_layout(screen);
    assert(lv_obj_get_width(server_label) <= host.width - (host.width == 240 ? 100 : 20));
    lv_obj_scroll_to_view(server_label, LV_ANIM_OFF);
    lv_refr_now(display);
    snprintf(name, sizeof(name), "%s-status-servers", prefix);
    snapshot(&host, dir, name);
    /* An unchanged one-second refresh preserves rows, text storage and scroll. */
    status_panel = lv_obj_get_parent(server_label);
    status_row_creations = status_row_deletions = 0;
    lv_obj_add_event_cb(status_panel, status_row_event, LV_EVENT_CHILD_CREATED, NULL);
    lv_obj_add_event_cb(server_label, status_row_event, LV_EVENT_DELETE, NULL);
    int32_t scroll_before = lv_obj_get_scroll_y(status_panel);
    const char *server_text = lv_label_get_text(server_label);
    uint32_t rows_before = lv_obj_get_child_count(status_panel);
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, ui_current_view(), &clock);
    lv_obj_update_layout(screen);
    assert(find_label(screen, "Zeitserver-IP (letzter lokaler Sync): 203.0.113.1") == server_label);
    assert(lv_label_get_text(server_label) == server_text);
    assert(lv_obj_get_child_count(status_panel) == rows_before);
    assert(lv_obj_get_scroll_y(status_panel) == scroll_before);
    assert(status_row_creations == 0 && status_row_deletions == 0);
    printf("status_unchanged_refresh rows=%u created=%u deleted=%u scroll=%d\n",
           rows_before, status_row_creations, status_row_deletions, (int)scroll_before);
    host.network.connected = false; /* Even a faulty provider's stale STA strings stay hidden. */
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "WLAN: getrennt"));
    assert(!find_label(screen, "IP: 192.0.2.10"));
    assert(!find_label(screen, "Gateway: 192.0.2.1"));
    assert(find_label(screen, "Gateway: nicht verfügbar"));
    assert(!find_label(screen, "DNS-IP: 192.0.2.53"));
    assert(!find_label(screen, "Zeitserver-IP (letzter lokaler Sync): 203.0.113.1"));
    assert(find_label(screen, "DNS-IP: unbekannt"));
    assert(find_label(screen, "Zeitserver-IP (letzter lokaler Sync): unbekannt"));
    host.network.mesh_channel = 0;
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh-Funkkanal: unbekannt"));
    host.network.mesh_active = false;
    host.network.mesh_channel = 6; /* Inactive must suppress a stale channel. */
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, ui_current_view(), &clock);
    assert(find_label(screen, "Mesh-Funkkanal: inaktiv"));
    click_label(screen, "Zurück");
    assert(ui_current_view() == BACKGROUND && !find_label(screen, "Status"));
    assert(!find_label(screen, "Einstellungen"));

    /* Physical reset: preparing a tile never confirms, and an unrelated
     * page gesture cannot turn the first contact into a destructive click. */
    services.reset_arm = reset_arm;
    services.reset_cancel = reset_cancel;
    services.reset_prepare = reset_prepare_warning;
    services.reset_confirm = reset_confirm;
    ui_set_services(&services);
    ui_render(&state, &local, SETTINGS, &clock);
    click_label(screen, "Werkseinstellungen zurücksetzen");
    assert(host.reset_arms == 1 && host.reset_confirms == 0);
    assert(find_label_prefix(screen, "Um den Factory Reset durchzuführen"));
    swipe(100, 30, clock.monotonic_ms + 1000);
    assert(ui_current_view() == SETTINGS && host.reset_confirms == 0);
    lv_obj_t *cancel_button = lv_obj_get_parent(find_label(screen, "Abbrechen"));
    lv_obj_send_event(cancel_button, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(cancel_button, LV_EVENT_CLICKED, NULL);
    assert(host.reset_cancels == 1 && !host.reset_prepared && !host.reset_marker);

    state.alarm_active = true;
    ui_render(&state, &local, SETTINGS, &clock);
    assert(!ui_reset_offer_from_web() && host.reset_arms == 1);
    state.alarm_active = false;
    ui_render(&state, &local, SETTINGS, &clock);
    assert(ui_reset_offer_from_web());
    assert(host.reset_arms == 2 && host.reset_confirms == 0 && host.reset_marker);
    ui_reset_close_remote(); /* EN/REST reboot destroys the old tile. */
    reset_simulate_hardware_boot(&host, true);
    assert(host.reset_warning && !host.reset_marker);
    ui_reset_show_warning();
    lv_obj_t *warning = find_label_prefix(screen, "WARNUNG: Löscht");
    assert(warning);
    char reset_stress[258];
    const char *stress_unit = "Safety warning: stored settings, alarm history and sessions are erased. ";
    for (size_t i = 0; i < sizeof(reset_stress) - 1; ++i)
        reset_stress[i] = stress_unit[i % strlen(stress_unit)];
    reset_stress[sizeof(reset_stress) - 1] = '\0';
    const char *reset_text = getenv("KATMED_TEST_RESET_WARNING");
    if (!reset_text) reset_text = reset_stress;
    assert(*reset_text);
    lv_label_set_text(warning, reset_text);
    lv_obj_t *yes_text = find_label(screen, "Ja, zurücksetzen");
    assert(yes_text);
    const char *translated_yes = getenv("KATMED_TEST_RESET_YES");
    if (translated_yes) lv_label_set_text(yes_text, translated_yes);
    lv_obj_t *no_text = find_label(screen, "Nein");
    assert(no_text);
    const char *translated_no = getenv("KATMED_TEST_RESET_NO");
    if (translated_no) lv_label_set_text(no_text, translated_no);
    lv_obj_update_layout(screen);
    lv_obj_t *yes = lv_obj_get_parent(yes_text);
    lv_obj_t *no = lv_obj_get_parent(no_text);
    lv_obj_t *warning_view = lv_obj_get_parent(warning);
    lv_area_t warning_area, warning_view_area, yes_area, yes_text_area,
              no_area, no_text_area, dialog_area;
    lv_obj_get_coords(warning, &warning_area);
    lv_obj_get_coords(warning_view, &warning_view_area);
    lv_obj_get_coords(yes, &yes_area);
    lv_obj_get_coords(yes_text, &yes_text_area);
    lv_obj_get_coords(no, &no_area);
    lv_obj_get_coords(no_text, &no_text_area);
    lv_obj_get_coords(lv_obj_get_parent(no), &dialog_area);
    assert(lv_obj_is_scrollable(warning_view));
    assert(!lv_obj_is_scrollable(lv_obj_get_parent(no)));
    assert(yes_area.x2 > yes_area.x1 && yes_area.y2 > yes_area.y1);
    assert(no_area.x2 > no_area.x1 && no_area.y2 > no_area.y1);
    assert(yes_text_area.x1 >= yes_area.x1 && yes_text_area.x2 <= yes_area.x2 &&
           yes_text_area.y1 >= yes_area.y1 && yes_text_area.y2 <= yes_area.y2);
    assert(no_text_area.x1 >= no_area.x1 && no_text_area.x2 <= no_area.x2 &&
           no_text_area.y1 >= no_area.y1 && no_text_area.y2 <= no_area.y2);
    assert(lv_obj_get_width(yes) * 10 == lv_obj_get_width(no) * 7);
    assert(lv_obj_get_height(yes) * 10 == lv_obj_get_height(no) * 7);
    assert(warning_view_area.y1 >= dialog_area.y1 &&
           warning_view_area.y2 < yes_area.y1 &&
           yes_area.y2 < no_area.y1 && no_area.y2 < dialog_area.y2);
    int32_t scroll_needed = lv_obj_get_scroll_bottom(warning_view);
    if (warning_area.y2 > warning_view_area.y2)
        assert(scroll_needed > 0);
    lv_refr_now(display);
    snprintf(name, sizeof(name), "%s-reset-warning-top", prefix);
    snapshot(&host, dir, name);
    lv_indev_t *reset_pointer = NULL;
    if (scroll_needed > 16) {
        reset_pointer = lv_indev_create();
        assert(reset_pointer);
        lv_indev_set_type(reset_pointer, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(reset_pointer, display);
        lv_indev_set_read_cb(reset_pointer, pointer_read);
        pointer_point.x = (warning_view_area.x1 + warning_view_area.x2) / 2;
        pointer_point.y = warning_view_area.y2 - 10;
        pointer_down = true;
        lv_indev_read(reset_pointer);
        lv_tick_inc(80);
        pointer_point.y = warning_view_area.y1 + 10;
        lv_indev_read(reset_pointer);
        pointer_down = false;
        lv_indev_read(reset_pointer);
        assert(lv_obj_get_scroll_y(warning_view) > 0);
        /* Finish pending invalidations before deleting this input device. */
    }
    lv_obj_scroll_to_y(warning_view, 10000, LV_ANIM_OFF);
    lv_obj_update_layout(screen);
    lv_obj_get_coords(warning, &warning_area);
    assert(lv_obj_get_scroll_bottom(warning_view) <= 0);
    assert(warning_area.y2 <= warning_view_area.y2);
    lv_area_t scrolled_no_area;
    lv_obj_get_coords(no, &scrolled_no_area);
    assert(scrolled_no_area.y1 == no_area.y1 && scrolled_no_area.y2 == no_area.y2);
    assert(host.reset_confirms == 0);
    lv_refr_now(display);
    snprintf(name, sizeof(name), "%s-reset-warning-bottom", prefix);
    snapshot(&host, dir, name);
    fprintf(stderr, "reset-long geometry %dx%d: text=%zu scroll=%ld view=%d..%d yes=%d..%d no=%d..%d buttons=%dx%d/%dx%d dialog bottom=%d\n",
            host.width, host.height, strlen(reset_text), (long)scroll_needed,
            warning_view_area.y1, warning_view_area.y2, yes_area.y1, yes_area.y2,
            no_area.y1, no_area.y2, lv_obj_get_width(yes), lv_obj_get_height(yes),
            lv_obj_get_width(no), lv_obj_get_height(no), dialog_area.y2);
    assert(lv_obj_get_width(yes) * 10 == lv_obj_get_width(no) * 7);
    assert(lv_obj_get_height(yes) * 10 == lv_obj_get_height(no) * 7);
    assert(lv_obj_has_state(no, LV_STATE_FOCUSED));
    lv_obj_send_event(yes, LV_EVENT_CLICKED, NULL);
    assert(host.reset_confirms == 0); /* No touch-through into Ja. */
    char cancelled_token[33];
    memcpy(cancelled_token, host.reset_confirmation.token, sizeof(cancelled_token));
    pointer_click(display, no);
    assert(host.reset_confirms == 0 && !host.reset_warning &&
           !host.reset_confirmation.valid);
    assert(reset_confirm(&host, cancelled_token, 32) == 1);

    assert(ui_reset_offer_from_web());
    ui_reset_close_remote();
    reset_simulate_hardware_boot(&host, false); /* Unknown reset cause: no prompt. */
    assert(!host.reset_warning && !host.reset_marker);
    reset_simulate_hardware_boot(&host, true); /* A discarded marker cannot return. */
    assert(!host.reset_warning);
    ui_reset_show_warning();
    assert(!find_label(screen, "Ja, zurücksetzen"));

    assert(ui_reset_offer_from_web());
    ui_reset_close_remote();
    reset_simulate_hardware_boot(&host, true); /* Includes power restoration. */
    assert(host.reset_warning && !host.reset_marker);
    ui_reset_show_warning();
    char accepted_token[33];
    memcpy(accepted_token, host.reset_confirmation.token, sizeof(accepted_token));
    yes = lv_obj_get_parent(find_label(screen, "Ja, zurücksetzen"));
    pointer_click(display, yes);
    assert(host.reset_confirms == 1 && !host.reset_warning &&
           !host.reset_confirmation.valid);
    assert(reset_confirm(&host, accepted_token, 32) == 1);

    /* A boot alarm may be active when the one-boot warning appears. Its
     * gesture path stays blocked, while the warning's Nein remains usable. */
    state.alarm_active = true;
    state.start_reason = START_BOOT;
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, ui_current_view(), &clock);
    host.reset_marker = true;
    reset_simulate_hardware_boot(&host, true);
    ui_reset_show_warning();
    assert(ui_lvgl_touch_allowed(true, host.width / 2, host.height / 2,
                                 true, clock.monotonic_ms));
    assert(ui_lvgl_touch_allowed(false, host.width / 2, host.height / 2,
                                 true, clock.monotonic_ms + 30));
    no = lv_obj_get_parent(find_label(screen, "Nein"));
    lv_obj_send_event(no, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(no, LV_EVENT_CLICKED, NULL);
    assert(host.reset_confirms == 1 && !host.reset_warning);

    /* Real LVGL glyph lookup and framebuffer snapshots for non-Latin copy. */
    lv_font_glyph_dsc_t glyph = {0};
    const uint32_t codepoint[] = {0x0627, 0xfe8d, 0x836f, 0x85ac,
                                  0x0109, 0x011d, 0x0135, 0x015c, 0x015d,
                                  0x0406, 0x0454, 0x0456, 0x0457};
    for (unsigned font_index = 0; font_index < 2; ++font_index) {
        const lv_font_t *font = font_index ? &katmed_font_14 : &katmed_font_12;
        for (unsigned i = 0; i < sizeof(codepoint) / sizeof(codepoint[0]); ++i) {
            assert(lv_font_get_glyph_dsc(font, &glyph, codepoint[i], 0));
            assert(glyph.adv_w > 0 && glyph.box_w > 0 && glyph.box_h > 0);
            lv_draw_buf_t *bitmap = lv_draw_buf_create(glyph.box_w, glyph.box_h,
                                                        LV_COLOR_FORMAT_A8, 0);
            assert(bitmap && lv_font_get_glyph_bitmap(&glyph, bitmap));
            bool drawn = false;
            for (uint32_t byte = 0; byte < bitmap->data_size; ++byte)
                if (((const uint8_t *)bitmap->data)[byte]) { drawn = true; break; }
            assert(drawn);
            lv_font_glyph_release_draw_data(&glyph);
            lv_draw_buf_destroy(bitmap);
        }
    }
    assert(lv_font_get_glyph_dsc(&katmed_font_30, &glyph, '-', 0));
    assert(lv_font_get_glyph_dsc(&katmed_font_30, &glyph, '/', 0));
    state.alarm_active = false;
    for (unsigned i = 0; i < 3; ++i) {
        const uint8_t languages[] = {KATMED_LANG_AR, KATMED_LANG_ZH_HANS, KATMED_LANG_JA};
        const char *suffix[] = {"arabic", "chinese", "japanese"};
        katmed_i18n_select(languages[i]);
        clock.monotonic_ms += 1000;
        ui_render(&state, &local, SETTINGS, &clock);
        ui_render(&state, &local, SETTINGS, &clock);
        assert(ui_current_view() == SETTINGS);
        const char *title = katmed_i18n_text("Einstellungen");
        char shaped[128];
        if (languages[i] == KATMED_LANG_AR) {
            assert(lv_text_ap_strlen(title) < sizeof(shaped));
            lv_text_ap_proc(title, shaped);
            title = shaped;
        }
        lv_obj_t *translated = find_label(screen, title);
        if (!translated) translated = find_label(screen, katmed_i18n_text("Sprache"));
        assert(translated);
        lv_obj_scroll_to_view(translated, LV_ANIM_OFF);
        lv_obj_update_layout(screen);
        report_memory(suffix[i]);
        lv_refr_now(display);
        snprintf(name, sizeof(name), "%s-i18n-%s", prefix, suffix[i]);
        snapshot(&host, dir, name);
    }
    katmed_i18n_select(KATMED_LANG_EN);
    clock.monotonic_ms+=1000;ui_render(&state,&local,SETTINGS,&clock);
    click_label(screen,"Configure night mode");
    for(uint8_t language=0;language<KATMED_LANGUAGE_COUNT;++language){
        katmed_i18n_select(language);clock.monotonic_ms+=1000;
        ui_render(&state,&local,SETTINGS,&clock);
        const char *copy=katmed_i18n_text("Setze Nachtmodus global auf allen Geräten im Mesh");
        char shaped_copy[512];
        if(language==KATMED_LANG_AR){
            assert(lv_text_ap_strlen(copy)<sizeof(shaped_copy));lv_text_ap_proc(copy,shaped_copy);copy=shaped_copy;
        }
        assert(find_label(screen,copy));
        lv_obj_update_layout(screen);lv_refr_now(display);
    }
    puts("night actual LVGL page: all12 locales render within the unchanged pool");
    settings_parent();
    katmed_i18n_select(KATMED_LANG_FR);
    clock.monotonic_ms += 1000;
    /* This is an isolated typography fixture, not a second settings overlay.
     * The actual physical-reset dialog/confirmation is exercised above. */
    ui_render(&state, &local, CLOCK, &clock);
    lv_obj_t *long_dialog = lv_obj_create(screen);
    lv_obj_set_size(long_dialog, host.width - 16, host.height - 16);
    lv_obj_center(long_dialog);
    lv_obj_t *long_label = lv_label_create(long_dialog);
    lv_obj_set_width(long_label, host.width - 40);
    lv_obj_set_style_text_font(long_label, &katmed_font_12, 0);
    lv_label_set_text(long_label, katmed_i18n_text(
        "WARNUNG: Löscht WLAN, Laufzeittexte, Mesh, Alarm/Verlauf, Webzugang, Zertifikate/ACME, Uploads und Cache. Firmware und Standardbilder bleiben. In einem privaten Build bleiben einkompilierte private Vorgaben erhalten."));
    lv_obj_update_layout(screen);
    assert(lv_obj_get_height(long_label) > 60);
    lv_refr_now(display);
    snprintf(name, sizeof(name), "%s-i18n-long-french", prefix);
    snapshot(&host, dir, name);
    lv_obj_delete(long_dialog);
    katmed_i18n_select(KATMED_LANG_EN);
    local.clock_style = DIGITAL_DATE;
    clock.monotonic_ms += 1000;
    ui_render(&state, &local, CLOCK, &clock);
    assert(find_label(screen, "Image not available") &&
           "UI must localize the image fallback before adapter presentation");
    LocalDateTime date_parts = {0};
    assert(app_berlin_local_time(clock.utc_ms, &date_parts) == APP_OK);
    char localized_date[16];
    katmed_i18n_format_date(localized_date, sizeof(localized_date),
                             (unsigned)date_parts.year, date_parts.month, date_parts.day);
    char visible_date[18];
    if (host.width == 172)
        snprintf(visible_date, sizeof(visible_date), "%.5s\n%s", localized_date,
                 localized_date + 5);
    else
        snprintf(visible_date, sizeof(visible_date), "%s", localized_date);
    assert(find_label(screen, visible_date));
    lv_refr_now(display);
    snprintf(name, sizeof(name), "%s-i18n-english-date", prefix);
    snapshot(&host, dir, name);
    katmed_i18n_format_date(localized_date, sizeof(localized_date), 2026, 10, 20);
    assert(strcmp(localized_date, "2026-10-20") == 0);
    katmed_i18n_select(KATMED_LANG_FR);
    katmed_i18n_format_date(localized_date, sizeof(localized_date), 2026, 10, 20);
    assert(strcmp(localized_date, "20/10/2026") == 0);
    katmed_i18n_select(KATMED_LANG_DE);
    katmed_i18n_format_date(localized_date, sizeof(localized_date), 2026, 10, 20);
    assert(strcmp(localized_date, "20.10.2026") == 0);
    katmed_i18n_select(KATMED_LANG_EN);

    if (reset_pointer) lv_indev_delete(reset_pointer);
    lv_display_delete(display);
    lv_deinit();
    free(draw_buffer);
    free(host.frame);
    printf("layout=%dx%d orientation=%d touch/store=%d network=%d artifacts=21\n",
           host.width, host.height, orientation_index * 90,
           host.store_calls, host.network_calls);
    return 0;
}
