#pragma once
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* The LittleFS mount and the pre-NVS runtime guard share this exact label. */
#define KATMED_USER_IMAGE_PARTITION_LABEL "user_images"

/* Security layouts share their start, but C6 has an actual 8 MiB flash. */
#if CONFIG_IDF_TARGET_ESP32C6
#define KATMED_USER_IMAGE_PARTITION_BYTES 0x5ca000u
#else
#define KATMED_USER_IMAGE_PARTITION_BYTES 0x1ca000u
#endif
