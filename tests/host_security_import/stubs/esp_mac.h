#pragma once
#include <stdint.h>
#include "esp_err.h"
esp_err_t esp_efuse_mac_get_default(uint8_t mac[6]);

/* Public SDKv6.1 selector order; factory identity remains fixed six bytes. */
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH,
    ESP_MAC_IEEE802154, ESP_MAC_BASE, ESP_MAC_EFUSE_FACTORY,
    ESP_MAC_EFUSE_CUSTOM, ESP_MAC_EFUSE_EXT } esp_mac_type_t;
esp_err_t esp_read_mac(uint8_t *mac,esp_mac_type_t type);
