#pragma once
#include <stdint.h>
#include "esp_partition.h"
typedef struct { uint8_t eky[32]; uint8_t tky[32]; } nvs_sec_cfg_t;
esp_err_t nvs_flash_read_security_cfg(const esp_partition_t *partition, nvs_sec_cfg_t *cfg);
esp_err_t nvs_flash_generate_keys(const esp_partition_t *partition, nvs_sec_cfg_t *cfg);
esp_err_t nvs_flash_secure_init(nvs_sec_cfg_t *cfg);
esp_err_t nvs_flash_init(void);
