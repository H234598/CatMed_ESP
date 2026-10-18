#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_netif_sntp.h"
#include "freertos/queue.h"
#include "katmed_time.h"
#include "katmed_time_idf.h"

static esp_sntp_time_cb_t callback;
static int64_t monotonic_us;
static struct {
    unsigned length;
    int present;
    unsigned char bytes[32];
} queue;

int64_t esp_timer_get_time(void) { return monotonic_us; }
QueueHandle_t xQueueCreate(unsigned length, unsigned size)
{
    assert(length == 1 && size <= sizeof(queue.bytes));
    queue.length = size;
    return &queue;
}
int xQueueOverwrite(QueueHandle_t handle, const void *item)
{
    assert(handle == &queue);
    memcpy(queue.bytes, item, queue.length);
    queue.present = 1;
    return 1;
}
int xQueueReceive(QueueHandle_t handle, void *item, unsigned ticks)
{
    assert(handle == &queue && ticks == 0);
    if (!queue.present) return 0;
    memcpy(item, queue.bytes, queue.length);
    queue.present = 0;
    return 1;
}
int xQueueReset(QueueHandle_t handle)
{
    assert(handle == &queue);
    queue.present = 0;
    return 1;
}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    assert(config->sync_cb != NULL);
    callback = config->sync_cb;
    return ESP_OK;
}
esp_err_t esp_netif_sntp_start(void) { return ESP_OK; }
void esp_netif_sntp_deinit(void) { callback = NULL; }

int main(void)
{
    KatmedTime time;
    ClockSample observation;
    const uint8_t server[] = "time.example.org";
    katmed_time_init(&time, 123);
    assert(katmed_time_idf_set_root(&time, true, true, server, sizeof(server) - 1) == ESP_OK);
    assert(callback != NULL);
    monotonic_us = 123000;

    /* The multiplication fits, but adding the fractional milliseconds does not. */
    struct timeval limit = {.tv_sec = INT64_MAX / 1000, .tv_usec = 999000};
    fputs("entering maximal SNTP callback\n", stderr);
    callback(&limit);
    fputs("returned from maximal SNTP callback\n", stderr);
    assert(!katmed_time_idf_poll(&time, &observation));

    struct timeval normal = {.tv_sec = 1792000000, .tv_usec = 123000};
    monotonic_us += 1000;
    callback(&normal);
    assert(katmed_time_idf_poll(&time, &observation));
    assert(observation.utc_valid && observation.utc_ms == 1792000000123LL);
    assert(katmed_time_idf_set_root(&time, false, false, NULL, 0) == ESP_OK);
    puts("SNTP callback rejects overflowing fractional milliseconds and accepts valid UTC");
    return 0;
}
