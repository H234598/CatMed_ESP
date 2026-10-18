#ifndef TEST_ESP_SNTP_H
#define TEST_ESP_SNTP_H
#include <stdint.h>
#define CONFIG_LWIP_SNTP_MAX_SERVERS 1
typedef struct {uint32_t addr; int type;} ip_addr_t;
#define IP_IS_V4(ip) ((ip)->type == 0)
#define ip_2_ip4(ip) (ip)
const ip_addr_t *esp_sntp_getserver(unsigned char idx);
#endif
