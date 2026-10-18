#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t nvs_handle_t;
typedef int esp_err_t;
typedef struct {size_t used_entries,free_entries,total_entries,namespace_count;} nvs_stats_t;
enum {ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,ESP_ERR_NVS_NOT_ENOUGH_SPACE=2,
      ESP_ERR_NVS_NO_FREE_PAGES=3,ESP_ERR_NVS_NEW_VERSION_FOUND=4,NVS_READWRITE=1};
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_get_stats(const char *,nvs_stats_t *);
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_flash_init(void);
#ifdef __cplusplus
}
#endif
