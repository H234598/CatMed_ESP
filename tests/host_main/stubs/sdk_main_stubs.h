#ifndef REVIEW_MAIN_STUBS_H
#define REVIEW_MAIN_STUBS_H
#include <stdbool.h>
#include <stdint.h>
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
unsigned esp_get_free_heap_size(void);
int esp_efuse_mac_get_default(uint8_t *);
uint32_t esp_random(void);
void esp_fill_random(void *,size_t);
int64_t esp_timer_get_time(void);
bool lvgl_port_lock(uint32_t);
void lvgl_port_unlock(void);
void vTaskDelay(unsigned);
void esp_mesh_lite_start(void);
void esp_restart(void);
#endif
