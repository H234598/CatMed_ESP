#pragma once
#include "mesh_sdk_stubs.h"
typedef enum { WIFI_SECOND_CHAN_NONE=0 } wifi_second_chan_t;
inline esp_err_t esp_wifi_get_channel(uint8_t *primary,wifi_second_chan_t *secondary){
    *primary=6;*secondary=WIFI_SECOND_CHAN_NONE;return ESP_OK;
}
