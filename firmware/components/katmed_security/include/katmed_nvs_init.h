#pragma once

#include "esp_err.h"

/* Never erases user NVS or repairs a damaged key partition implicitly. */
esp_err_t katmed_nvs_init(void);
