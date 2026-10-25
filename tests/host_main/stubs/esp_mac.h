#ifndef KATMED_TEST_ESP_MAC_H
#define KATMED_TEST_ESP_MAC_H
#include "sdk_main_stubs.h"
/* Public SDK v6.1 selector order; factory is always MAC-48, not EUI-64. */
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH,
    ESP_MAC_IEEE802154, ESP_MAC_BASE, ESP_MAC_EFUSE_FACTORY,
    ESP_MAC_EFUSE_CUSTOM, ESP_MAC_EFUSE_EXT } esp_mac_type_t;
int esp_read_mac(uint8_t *mac,esp_mac_type_t type);
#endif
