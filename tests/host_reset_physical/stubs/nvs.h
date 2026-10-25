#ifndef TEST_NVS_H
#define TEST_NVS_H
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define ESP_ERR_NVS_NOT_FOUND 0x1102
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size);
esp_err_t nvs_erase_all(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
#endif
