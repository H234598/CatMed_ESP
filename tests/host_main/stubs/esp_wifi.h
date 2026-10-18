#ifndef KATMED_HOST_MAIN_ESP_WIFI_H
#define KATMED_HOST_MAIN_ESP_WIFI_H
typedef enum {WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA} wifi_mode_t;
typedef struct {int unused;} wifi_ap_record_t;
static inline int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{(void)ap; return -1;}
static inline int esp_wifi_get_mode(wifi_mode_t *mode)
{*mode=WIFI_MODE_NULL; return -1;}
#endif
