#ifndef KATMED_HOST_MAIN_ESP_WIFI_H
#define KATMED_HOST_MAIN_ESP_WIFI_H
typedef enum {WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA} wifi_mode_t;
typedef struct {int unused;} wifi_ap_record_t;
static inline int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{(void)ap; return -1;}
static inline int esp_wifi_get_mode(wifi_mode_t *mode)
{*mode=WIFI_MODE_NULL; return -1;}
typedef struct { int unused; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
#define WIFI_STORAGE_RAM 0
#define WIFI_IF_AP 1
#define WIFI_AUTH_WPA2_PSK 3
typedef struct { struct { unsigned char ssid[32],password[64]; unsigned ssid_len,max_connection,channel,authmode; } ap; } wifi_config_t;
static inline int esp_wifi_init(const wifi_init_config_t *c){(void)c;return 0;}
static inline int esp_wifi_set_storage(int v){(void)v;return 0;}
static inline int esp_wifi_set_mode(wifi_mode_t v){(void)v;return 0;}
#ifdef KATMED_TEST_WIFI_CONFIG
int katmed_test_wifi_config(int v, wifi_config_t *c);
static inline int esp_wifi_set_config(int v,wifi_config_t *c){return katmed_test_wifi_config(v,c);}
#else
static inline int esp_wifi_set_config(int v,wifi_config_t *c){(void)v;(void)c;return 0;}
#endif
static inline int esp_wifi_start(void){return 0;}
#endif
