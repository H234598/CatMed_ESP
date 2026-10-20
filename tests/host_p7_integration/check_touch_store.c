#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_touch_buffer.h"
#define KATMED_FULL_STACK 1
#define KATMED_WITH_CORE 1
#define KATMED_HOST_TOUCH_SAMPLER 1
#include "../../firmware/main/main.c"

static uint8_t durable[KATMED_STORE_BLOB_MAX];
static size_t durable_length;
static unsigned writes;
static unsigned mesh_notifications;
static bool mesh_connected;
static bool mesh_coordinator;
static int64_t microseconds;
static BoardTouchBuffer samples;
static uint16_t pixels[240 * 16];

static StoreResult read_backend(void *context, uint8_t *out, size_t *length)
{
    (void)context;
    if (!durable_length) return STORE_NOT_FOUND;
    assert(*length >= durable_length);
    memcpy(out, durable, durable_length);
    *length = durable_length;
    return STORE_OK;
}
static StoreResult write_backend(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    assert(length <= sizeof(durable));
    memcpy(durable, data, length);
    durable_length = length;
    ++writes;
    return STORE_OK;
}

int64_t esp_timer_get_time(void) { return microseconds; }
bool lvgl_port_lock(uint32_t timeout) { (void)timeout; return true; }
void lvgl_port_unlock(void) {}
bool board_touch_sampler_running(void) { return true; }
bool board_touch_sampler_take(BoardTouch *sample, BoardResult *result)
{ return board_touch_buffer_take(&samples, sample, result); }
BoardResult board_set_orientation(Orientation value)
{ (void)value; return BOARD_OK; }
bool katmed_mesh_connected(void) { return mesh_connected; }
bool katmed_mesh_is_coordinator(void) { return mesh_coordinator; }
void katmed_mesh_notify_local_commit(void) { ++mesh_notifications; }
KatmedMeshResult katmed_mesh_submit_shared(const AppEvent *event,
                                            const ClockSample *now,
                                            AppEffects *effects)
{ (void)event; (void)now; (void)effects; return MESH_UNAVAILABLE; }
KatmedImageStatus katmed_images_present(lv_obj_t *image, lv_obj_t *fallback,
                                         uint32_t image_id, Orientation orientation)
{
    (void)image_id; (void)orientation;
    lv_obj_set_hidden(image, true);
    lv_obj_set_hidden(fallback, false);
    return KATMED_IMAGE_OK;
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *map)
{ (void)area; (void)map; lv_display_flush_ready(display); }

static void sample(uint64_t at, bool down, BoardResult result)
{
    BoardTouch touch = {.monotonic_ms = at, .down = down, .x = 100, .y = 100};
    board_touch_buffer_push(&samples, touch, result);
    microseconds = (int64_t)at * 1000;
    ClockSample now = clock_sample(runtime.boot_domain);
    assert(render_ui(&runtime.state, &runtime.local, &now, NULL, false, true));
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(240, 240);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);

    const uint8_t catalog[KATMED_CATALOG_BYTES] = {3, 4, 5};
    StoreRecovery recovery = {0};
    StoreBackend backend = {NULL, read_backend, write_backend};
    microseconds = 1000000;
    assert(store_open(&runtime.store, backend, UINT64_C(0xAC276E46BC2C),
                      catalog, 75, &recovery) == STORE_OK);
    runtime.store_ready = true;
    runtime.mesh_started = true; /* Exercise the production Mesh notification adapter. */
    runtime.boot = recovery.boot_identity;
    runtime.boot_domain = UINT64_C(0x456789abc);
    katmed_time_init(&runtime.time, runtime.boot_domain);
    runtime.local = recovery.local;
    ClockSample now = clock_sample(runtime.boot_domain);
    assert(app_init(&runtime.state, &recovery.shared, &now, runtime.boot,
                    75, catalog) == APP_OK);
    UiServices services = {.context = &runtime, .submit = submit_ui};
    ui_set_services(&services);
    ui_render(&runtime.state, &runtime.local, ALARM, &now);
    const unsigned initial_writes = writes;
    assert(initial_writes == 1);

    sample(1010, false, BOARD_OK); /* A fresh release arms contact detection. */
    sample(1040, true, BOARD_OK);
    sample(1080, true, BOARD_IO_ERROR);
    sample(1120, false, BOARD_OK);
    assert(writes == initial_writes && runtime.state.alarm_active);
    assert(!runtime.state.has_last_confirmation);

    sample(1200, true, BOARD_OK);
    sample(1280, false, BOARD_OK);
    assert(writes == initial_writes && mesh_notifications == 0);
    assert(!runtime.state.alarm_active && runtime.state.start_reason == START_BOOT);
    assert(!runtime.state.has_last_confirmation &&
           runtime.state.deadline.effective_monotonic_ms == 0);
    StoreRecovery after = {0};
    assert(store_get_recovery(&runtime.store, &after) == STORE_OK);
    assert(after.pending_count == 0 && !after.has_last_confirmation &&
           !runtime.store.record.has_history);
    sample(1360, true, BOARD_OK);
    sample(1440, false, BOARD_OK);
    assert(writes == initial_writes && mesh_notifications == 0);

    /* A coordinator-created schedule yields a genuine due alarm. Its touch
     * still takes the full Core/Store/Mesh-adapter confirmation path. */
    ClockSample scheduled = clock_sample(runtime.boot_domain);
    EventId scheduled_id;
    assert(store_next_event_id(&runtime.store, &scheduled_id) == STORE_OK);
    AppEvent set_interval = {.kind = EVENT_SET_MEDICATION_INTERVAL,
        .id = scheduled_id, .sender = scheduled_id.device,
        .round = runtime.state.round, .parent_revision = runtime.state.revision,
        .occurred_at = scheduled, .coordinator_accepted = true};
    set_interval.value.interval_seconds = 1;
    AppEffects effects = {0};
    AppResult core_result = APP_INVALID;
    assert(store_apply_event(&runtime.store, &runtime.state, &set_interval,
                             &scheduled, false, &effects, &core_result) == STORE_OK);
    microseconds += 1000000;
    ClockSample due = clock_sample(runtime.boot_domain);
    assert(app_tick(&runtime.state, &due, &effects) == APP_OK);
    assert(runtime.state.alarm_active && runtime.state.start_reason == START_SETTINGS);
    ui_render(&runtime.state, &runtime.local, ALARM, &due);
    sample(due.monotonic_ms + 30, false, BOARD_OK);
    sample(due.monotonic_ms + 80, true, BOARD_OK);
    sample(due.monotonic_ms + 160, false, BOARD_OK);
    assert(!runtime.state.alarm_active && runtime.state.has_last_confirmation);
    assert(runtime.store.record.pending_count == 1 && mesh_notifications == 1);
    printf("P7 touch/main/core/store/mesh adapter: boot notice only closes; due reminder confirms\n");
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
