#include "katmed_time_idf.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
    uint64_t monotonic_ms;
    int64_t utc_ms;
} SntpObservation;

/* ESP-NETIF exposes one process-wide SNTP client. The callback runs outside
 * the application loop and only transfers the observation to a bounded queue. */
static QueueHandle_t s_observations;
static KatmedTime *s_owner;
static atomic_bool s_running;
static char s_server[64];
static atomic_uint_least32_t s_sync_server_ipv4;

uint32_t katmed_time_idf_sync_server_ipv4(void)
{
    return atomic_load_explicit(&s_running, memory_order_acquire)
        ? atomic_load_explicit(&s_sync_server_ipv4, memory_order_relaxed) : 0;
}

static void on_sntp_sync(struct timeval *tv)
{
    if (!atomic_load_explicit(&s_running, memory_order_acquire) ||
        !s_observations || !tv || tv->tv_sec <= 0 ||
        tv->tv_sec > INT64_MAX / 1000 || tv->tv_usec < 0 ||
        tv->tv_usec >= 1000000) return;
    const int64_t monotonic_us = esp_timer_get_time();
    if (monotonic_us < 0) return;
    const int64_t seconds_ms = (int64_t)tv->tv_sec * 1000;
    const int64_t fraction_ms = tv->tv_usec / 1000;
    if (seconds_ms > INT64_MAX - fraction_ms) return;
    SntpObservation observation = {
        .monotonic_ms = (uint64_t)monotonic_us / 1000u,
        .utc_ms = seconds_ms + fraction_ms,
    };
    /* This callback runs in lwIP context. Only the product's single-server
     * configuration identifies slot zero unambiguously; never guess an index.
     * This is a last local sync address, not a fresh DNS query or reachability. */
#if CONFIG_LWIP_SNTP_MAX_SERVERS == 1
    const ip_addr_t *server = esp_sntp_getserver(0);
    atomic_store_explicit(&s_sync_server_ipv4,
        server && IP_IS_V4(server) ? ip_2_ip4(server)->addr : 0,
        memory_order_relaxed);
#endif
    (void)xQueueOverwrite(s_observations, &observation);
}

static bool valid_server(const uint8_t *name, size_t length)
{
    if (!name || length == 0 || length > 63) return false;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    return true;
}

esp_err_t katmed_time_idf_set_root(KatmedTime *time, bool is_root, bool has_ip,
                                   const uint8_t *ntp_server, size_t length)
{
    if (!time || !time->domain) return ESP_ERR_INVALID_ARG;
    if (s_owner && s_owner != time) return ESP_ERR_INVALID_STATE;
    const bool needed = is_root && has_ip;
    if (!needed) {
        if (s_owner == time) {
            atomic_store_explicit(&s_running, false, memory_order_release);
            esp_netif_sntp_deinit();
            s_owner = NULL;
            if (s_observations) xQueueReset(s_observations);
            memset(s_server, 0, sizeof(s_server));
        }
        return ESP_OK;
    }
    if (!valid_server(ntp_server, length)) return ESP_ERR_INVALID_ARG;
    if (s_owner && s_owner != time) return ESP_ERR_INVALID_STATE;
    if (atomic_load_explicit(&s_running, memory_order_acquire) &&
        strlen(s_server) == length &&
        memcmp(s_server, ntp_server, length) == 0) return ESP_OK;
    if (atomic_load_explicit(&s_running, memory_order_acquire)) {
        atomic_store_explicit(&s_running, false, memory_order_release);
        esp_netif_sntp_deinit();
        xQueueReset(s_observations);
    }
    if (!s_observations) {
        s_observations = xQueueCreate(1, sizeof(SntpObservation));
        if (!s_observations) return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_sync_server_ipv4, 0, memory_order_relaxed);
    memcpy(s_server, ntp_server, length);
    s_server[length] = 0;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_server);
    config.start = false;
    config.wait_for_sync = false;
    config.sync_cb = on_sntp_sync;
    esp_err_t result = esp_netif_sntp_init(&config);
    if (result == ESP_OK) {
        s_owner = time;
        atomic_store_explicit(&s_running, true, memory_order_release);
        result = esp_netif_sntp_start();
    }
    if (result != ESP_OK) {
        atomic_store_explicit(&s_running, false, memory_order_release);
        esp_netif_sntp_deinit();
        xQueueReset(s_observations);
        memset(s_server, 0, sizeof(s_server));
        s_owner = NULL;
        return result;
    }
    return ESP_OK;
}

bool katmed_time_idf_poll(KatmedTime *time, ClockSample *new_sync)
{
    if (!time || !new_sync || time != s_owner ||
        !atomic_load_explicit(&s_running, memory_order_acquire) ||
        !s_observations) return false;
    SntpObservation observation;
    if (xQueueReceive(s_observations, &observation, 0) != pdTRUE ||
        !katmed_time_accept_utc(time, observation.monotonic_ms,
                                observation.utc_ms, CLOCK_SYNCHRONIZED)) return false;
    *new_sync = katmed_time_sample(time, observation.monotonic_ms);
    return new_sync->utc_valid;
}
