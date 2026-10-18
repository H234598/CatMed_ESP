#ifndef TEST_ESP_NETIF_SNTP_H
#define TEST_ESP_NETIF_SNTP_H
#include <stdbool.h>
#include <sys/time.h>
#include "esp_err.h"
typedef void (*esp_sntp_time_cb_t)(struct timeval *);
typedef struct {
    const char *server;
    bool start;
    bool wait_for_sync;
    esp_sntp_time_cb_t sync_cb;
} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(value) { .server = (value), .start = true, .wait_for_sync = true, .sync_cb = 0 }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
esp_err_t esp_netif_sntp_start(void);
void esp_netif_sntp_deinit(void);
#endif
