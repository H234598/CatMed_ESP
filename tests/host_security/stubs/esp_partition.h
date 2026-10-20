#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_NVS 2
#define ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS 4
#define ESP_PARTITION_SUBTYPE_DATA_LITTLEFS 0x83
#define ESP_PARTITION_SUBTYPE_ANY 255
typedef int esp_partition_subtype_t;
typedef struct {
    int type;
    int subtype;
    uint32_t address;
    size_t size;
    char label[17];
    bool encrypted;
    bool readonly;
    uint8_t *storage;
} esp_partition_t;
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
esp_err_t esp_partition_read_raw(const esp_partition_t *partition, size_t offset,
                                 void *dst, size_t length);
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset,
                             void *dst, size_t length);
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset,
                              const void *src, size_t length);
