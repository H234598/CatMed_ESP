#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define KATMED_FULL_STACK 1
#define KATMED_WITH_CORE 1
#include "../../firmware/main/main.c"

static jmp_buf stop_loop;
static unsigned ticks;
static unsigned writes;
static unsigned metric_collections;
static uint64_t metric_sample_ms[3];
static unsigned ui_renders;
static uint64_t last_render_ms;
static int64_t fake_microseconds;
static uint8_t persisted[KATMED_STORE_BLOB_MAX];
static size_t persisted_length;
static UiServices installed_services;

static StoreResult fake_read(void *context, uint8_t *out, size_t *length)
{
    (void)context;
    if (persisted_length == 0) return STORE_NOT_FOUND;
    if (*length < persisted_length) return STORE_CORRUPT;
    memcpy(out, persisted, persisted_length);
    *length = persisted_length;
    return STORE_OK;
}

static StoreResult fake_write(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    assert(length <= sizeof(persisted));
    ++writes;
    memcpy(persisted, data, length);
    persisted_length = length;
    return STORE_OK;
}

StoreResult store_idf_open_backend(StoreIdfNvs *nvs, StoreBackend *backend)
{
    nvs->opened = true;
    *backend = (StoreBackend){NULL, fake_read, fake_write};
    return STORE_OK;
}

void store_idf_close_backend(StoreIdfNvs *nvs) { nvs->opened = false; }
unsigned esp_get_free_heap_size(void) { return 80000; }
int esp_read_mac(uint8_t *mac,esp_mac_type_t type)
{
    assert(type==ESP_MAC_EFUSE_FACTORY);
    const uint8_t id[6] = {0xac, 0x27, 0x6e, 0x46, 0xbc, 0x2c};
    memcpy(mac, id, sizeof(id));
    return ESP_OK;
}
uint32_t esp_random(void) { return 17; }
int64_t esp_timer_get_time(void) { return fake_microseconds; }
bool lvgl_port_lock(uint32_t wait) { (void)wait; return true; }
void lvgl_port_unlock(void) {}
void vTaskDelay(unsigned wait)
{
    assert(wait == 25);
    if (++ticks > 801) longjmp(stop_loop, 1);
    fake_microseconds += 25000;
}
void esp_mesh_lite_start(void) {}

BoardResult board_init(lv_display_t **display)
{
    *display = (lv_display_t *)(uintptr_t)1;
    return BOARD_OK;
}
void board_deinit(void) {}
BoardResult board_set_orientation(Orientation orientation)
{
    (void)orientation;
    return BOARD_OK;
}
BoardResult board_read_touch(BoardTouch *touch)
{
    touch->down = false;
    return BOARD_OK;
}
KatmedImageStatus katmed_images_mount_partition(void) { return KATMED_IMAGE_UNAVAILABLE; }
void katmed_images_unmount(void) {}
void katmed_images_register_lvgl_fs(void) {}
uint32_t katmed_images_count(void) { return 0; }
const uint8_t *katmed_images_catalog_sha256(void)
{
    static const uint8_t catalog[KATMED_CATALOG_BYTES] = {1, 2, 3};
    return catalog;
}

void ui_set_services(const UiServices *services) { installed_services = *services; }
UiView ui_current_view(void) { return BACKGROUND; }
void ui_render(const AppState *state, const LocalSettings *local,
               UiView view, const ClockSample *clock)
{
    assert(state && local && clock && view == BACKGROUND);
    assert(clock->quality == CLOCK_UNAVAILABLE && !clock->utc_valid);
    assert(clock->monotonic_ms >= last_render_ms);
    last_render_ms = clock->monotonic_ms;
    ++ui_renders;
}
void ui_touch_sample(bool down, int16_t x, int16_t y, bool valid, uint64_t ms)
{
    (void)x; (void)y; (void)valid; (void)ms;
    assert(!down);
}

bool katmed_usb_console_start(void) { return true; }
bool katmed_usb_console_receive(KatmedUsbLine *line) { (void)line; return false; }
void katmed_usb_console_reply(const char *reply) { (void)reply; assert(false); }
const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t description = {.version = "host-test"};
    return &description;
}
int esp_app_get_elf_sha256(char *buffer, size_t size)
{
    if (size > 0) buffer[0] = '\0';
    return 0;
}

/* Wrap only the SDK collection boundary. The real main loop and gate run. */
void __wrap_katmed_runtime_metrics_collect(uint64_t now_ms, KatmedRuntimeMetricInput *out)
{
    assert(metric_collections < 3);
    metric_sample_ms[metric_collections] = now_ms;
    ++metric_collections;
    assert(out != NULL);
    memset(out, 0, sizeof(*out));
    out->uptime_ms = now_ms;
}

int main(void)
{
    if (setjmp(stop_loop) == 0) app_main();
    assert(ticks == 802);
    assert(metric_collections == 3); /* First 25 ms, 10.025 s and 20.025 s. */
    assert(metric_sample_ms[0] == 25 && metric_sample_ms[1] == 10025 &&
           metric_sample_ms[2] == 20025);
    assert(ui_renders > 300); /* Normal visible clock/image loop kept running. */
    assert(installed_services.submit == submit_ui);
    assert(writes == 1); /* Initial durable boot record only. */
    assert(runtime.store.record.pending_count == 0);
    assert(!runtime.state.has_last_confirmation);
    assert(runtime.state.alarm_active);
    puts("main runtime sampling without NVS/confirmation side effects passed");
    return 0;
}
