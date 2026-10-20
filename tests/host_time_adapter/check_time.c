#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "katmed_time.h"
#include "katmed_time_idf.h"
#include "katmed_core.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/queue.h"

static ip_addr_t fake_server;
const ip_addr_t *esp_sntp_getserver(unsigned char idx) {assert(idx == 0); return &fake_server;}

static int64_t fake_us;
static esp_sntp_time_cb_t fake_callback;
static int init_count, start_count, stop_count;
static int fail_start;
static char server_seen[64];
static struct { unsigned item_size; int occupied; unsigned char item[32]; } fake_queue;

int64_t esp_timer_get_time(void) { return fake_us; }
QueueHandle_t xQueueCreate(unsigned length, unsigned item_size)
{
    assert(length == 1 && item_size <= sizeof(fake_queue.item));
    fake_queue.item_size = item_size;
    return &fake_queue;
}
int xQueueOverwrite(QueueHandle_t queue, const void *item)
{
    assert(queue == &fake_queue);
    memcpy(fake_queue.item, item, fake_queue.item_size);
    fake_queue.occupied = 1;
    return 1;
}
int xQueueReceive(QueueHandle_t queue, void *item, unsigned ticks)
{
    assert(queue == &fake_queue && ticks == 0);
    if (!fake_queue.occupied) return 0;
    memcpy(item, fake_queue.item, fake_queue.item_size);
    fake_queue.occupied = 0;
    return 1;
}
int xQueueReset(QueueHandle_t queue)
{
    assert(queue == &fake_queue);
    fake_queue.occupied = 0;
    return 1;
}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    assert(!config->start && !config->wait_for_sync);
    assert(config->server && config->sync_cb);
    ++init_count;
    strncpy(server_seen, config->server, sizeof(server_seen) - 1);
    fake_callback = config->sync_cb;
    return ESP_OK;
}
esp_err_t esp_netif_sntp_start(void)
{
    ++start_count;
    return fail_start ? ESP_ERR_INVALID_STATE : ESP_OK;
}
void esp_netif_sntp_deinit(void) { ++stop_count; fake_callback = NULL; }

static void test_clock(void)
{
    KatmedTime time;
    katmed_time_init(&time, 17);
    ClockSample sample = katmed_time_sample(&time, UINT64_C(4294967296) + 3);
    assert(sample.domain == 17 && !sample.utc_valid && sample.utc_ms == 0);
    katmed_time_set_link(&time, true);
    assert(!katmed_time_accept_utc(&time, 100, 1, CLOCK_SYNCHRONIZED));
    assert(!katmed_time_accept_utc(&time, 100, 1792000000000LL, CLOCK_UNAVAILABLE));
    assert(katmed_time_accept_utc(&time, UINT64_C(4294967296), 1792000000000LL, CLOCK_SYNCHRONIZED));
    sample = katmed_time_sample(&time, UINT64_C(4294967296) + 42);
    assert(sample.utc_valid && sample.utc_ms == 1792000000042LL &&
           sample.quality == CLOCK_SYNCHRONIZED);
    assert(!katmed_time_accept_utc(&time, UINT64_C(4294967296), 1792000001000LL, CLOCK_SYNCHRONIZED));
    assert(!katmed_time_accept_utc(&time, UINT64_C(4294967296) - 1, 1792000001000LL, CLOCK_SYNCHRONIZED));
    assert(katmed_time_accept_utc(&time, UINT64_C(4294967296) + 50, 1791999990000LL, CLOCK_HOLDOVER));
    sample = katmed_time_sample(&time, UINT64_C(4294967296) + 55);
    assert(sample.utc_ms == 1791999990005LL && sample.quality == CLOCK_HOLDOVER);
    katmed_time_set_link(&time, false);
    sample = katmed_time_sample(&time, UINT64_C(4294967296) + 56);
    assert(sample.utc_valid && sample.quality == CLOCK_HOLDOVER);
    katmed_time_set_link(&time, true);
    sample = katmed_time_sample(&time, UINT64_C(4294967296) + 51 + KATMED_TIME_FRESH_MS);
    assert(sample.utc_valid && sample.quality == CLOCK_HOLDOVER);
    sample = katmed_time_sample(&time, UINT64_C(4294967296) + 49);
    assert(!sample.utc_valid);
    katmed_time_init(&time, 18);
    sample = katmed_time_sample(&time, 100);
    assert(sample.domain == 18 && !sample.utc_valid);
}

static void test_sntp_gate(void)
{
    KatmedTime time;
    katmed_time_init(&time, 19);
    const uint8_t source[] = "time.example.org";
    ClockSample sync;
    assert(katmed_time_idf_set_root(&time, false, true, source, sizeof(source)-1) == ESP_OK);
    assert(katmed_time_idf_set_root(&time, true, false, source, sizeof(source)-1) == ESP_OK);
    assert(init_count == 0 && !katmed_time_idf_poll(&time, &sync));
    assert(katmed_time_idf_set_root(&time, true, true, source, 0) == ESP_ERR_INVALID_ARG);
    const uint8_t invalid_source[] = "bad/host";
    assert(katmed_time_idf_set_root(&time, true, true, invalid_source,
                                   sizeof(invalid_source)-1) == ESP_ERR_INVALID_ARG);
    assert(katmed_time_idf_set_root(&time, true, true, source, sizeof(source)-1) == ESP_OK);
    assert(init_count == 1 && start_count == 1 && strcmp(server_seen, "time.example.org") == 0);
    assert(katmed_time_idf_set_root(&time, true, true, source, sizeof(source)-1) == ESP_OK);
    assert(init_count == 1 && !katmed_time_idf_poll(&time, &sync));
    struct timeval tv = {.tv_sec = 1792000000, .tv_usec = 123000};
    fake_us = UINT64_C(4294967296) * 1000;
    struct timeval overflow_tv = {.tv_sec = INT64_MAX / 1000, .tv_usec = 999000};
    fake_callback(&overflow_tv);
    assert(!fake_queue.occupied && !katmed_time_idf_poll(&time, &sync));
    overflow_tv.tv_usec = (INT64_MAX % 1000) * 1000;
    fake_callback(&overflow_tv);
    assert(fake_queue.occupied); /* Arithmetic boundary itself remains representable. */
    assert(!katmed_time_idf_poll(&time, &sync)); /* Core date range is narrower. */
    struct timeval invalid_tv = {.tv_sec = 1, .tv_usec = 0};
    fake_callback(&invalid_tv);
    assert(!katmed_time_idf_poll(&time, &sync));
    invalid_tv.tv_sec = 1792000000;
    invalid_tv.tv_usec = 1000000;
    fake_callback(&invalid_tv);
    assert(!katmed_time_idf_poll(&time, &sync));
    assert(katmed_time_idf_sync_server_ipv4() == 0);
    fake_server.addr = 0x017100CB;
    fake_callback(&tv);
    assert(katmed_time_idf_sync_server_ipv4() == fake_server.addr);
    fake_server.type = 6;
    fake_callback(&tv);
    assert(katmed_time_idf_sync_server_ipv4() == 0);
    fake_server.type = 0;
    fake_callback(&tv);
    assert(katmed_time_idf_poll(&time, &sync));
    assert(sync.utc_valid && sync.utc_ms == 1792000000123LL && sync.domain == 19);
    assert(!katmed_time_idf_poll(&time, &sync));
    fake_callback(&tv);
    assert(katmed_time_idf_set_root(&time, false, false, NULL, 0) == ESP_OK);
    assert(katmed_time_idf_sync_server_ipv4() == 0);
    assert(stop_count == 1 && !katmed_time_idf_poll(&time, &sync));
    assert(katmed_time_sample(&time, UINT64_C(4294967296) + 1).utc_valid);
    assert(katmed_time_idf_set_root(&time, true, false, source, sizeof(source)-1) == ESP_OK);
    assert(katmed_time_idf_set_root(&time, true, true, source, sizeof(source)-1) == ESP_OK);
    assert(init_count == 2 && start_count == 2);
    assert(katmed_time_idf_sync_server_ipv4() == 0);
    fake_us += 1000000;
    fake_callback(&tv);
    assert(katmed_time_idf_poll(&time, &sync));
    assert(sync.quality == CLOCK_HOLDOVER); /* Link must be asserted by network owner. */
    assert(katmed_time_idf_set_root(&time, false, false, NULL, 0) == ESP_OK);
    fail_start = 1;
    assert(katmed_time_idf_set_root(&time, true, true, source,
                                   sizeof(source)-1) == ESP_ERR_INVALID_STATE);
    assert(!katmed_time_idf_poll(&time, &sync));
    fail_start = 0;
    assert(katmed_time_idf_set_root(&time, true, true, source,
                                   sizeof(source)-1) == ESP_OK);
    assert(katmed_time_idf_set_root(&time, false, false, NULL, 0) == ESP_OK);
}

static void test_core_correction(void)
{
    KatmedTime time;
    katmed_time_init(&time, 20);
    katmed_time_set_link(&time, true);
    assert(katmed_time_accept_utc(&time, 1000, 1768464000000LL, CLOCK_SYNCHRONIZED));
    ClockSample now = katmed_time_sample(&time, 1000);
    SharedSettings settings = app_default_settings();
    uint8_t catalog[KATMED_CATALOG_BYTES] = {1};
    AppState state;
    assert(app_init(&state, &settings, &now, (EventId){10,1,1}, 3, catalog) == APP_OK);
    state.start_reason = START_SETTINGS;
    state.deadline.regular_monotonic_ms = now.monotonic_ms;
    state.deadline.effective_monotonic_ms = now.monotonic_ms;
    state.deadline.regular_utc = (UtcInstant){true,now.utc_ms};
    state.deadline.effective_utc = (UtcInstant){true,now.utc_ms};
    AppEvent confirm = {
        .kind = EVENT_CONFIRM, .id = {10,1,2}, .sender = 10,
        .round = state.round, .parent_revision = state.revision,
        .occurred_at = now, .coordinator_accepted = true,
    };
    assert(app_apply_event(&state, &confirm, &now, NULL) == APP_OK);
    const uint64_t original_due = state.deadline.effective_monotonic_ms;
    assert(katmed_time_accept_utc(&time, 3601000, 1768456800000LL, CLOCK_SYNCHRONIZED));
    now = katmed_time_sample(&time, 3601000);
    AppEvent correction = {
        .kind = EVENT_TIME_SYNC, .id = {10,1,3}, .sender = 10,
        .round = state.round, .parent_revision = state.revision,
        .occurred_at = now, .coordinator_accepted = true,
    };
    correction.value.time_sync = now;
    AppResult sync_result = app_apply_event(&state, &correction, &now, NULL);
    if (sync_result != APP_OK) fprintf(stderr, "first correction result=%d\n", sync_result);
    assert(sync_result == APP_OK);
    assert(state.deadline.effective_monotonic_ms <= original_due);
    assert(katmed_time_accept_utc(&time, 3602000, 1768500000000LL, CLOCK_SYNCHRONIZED));
    now = katmed_time_sample(&time, 3602000);
    correction.id.sequence = 4;
    correction.round = state.round;
    correction.parent_revision = state.revision;
    correction.occurred_at = now;
    correction.value.time_sync = now;
    sync_result = app_apply_event(&state, &correction, &now, NULL);
    if (sync_result != APP_OK) fprintf(stderr, "second correction result=%d\n", sync_result);
    assert(sync_result == APP_OK);
    assert(state.alarm_active && state.deadline.effective_monotonic_ms <= original_due);
}

int main(void)
{
    test_clock();
    test_sntp_gate();
    test_core_correction();
    puts("time adapter: pure clock, 49-day boundary, correction, holdover and IDF SNTP gate passed");
    return 0;
}
