#ifndef KATMED_HOST_MAIN_ESP_NETIF_H
#define KATMED_HOST_MAIN_ESP_NETIF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct esp_netif esp_netif_t;
typedef struct {uint32_t addr;} esp_ip4_addr_t;
typedef struct {esp_ip4_addr_t ip, netmask, gw;} esp_netif_ip_info_t;
#define ESP_NETIF_DNS_MAIN 0
#define ESP_IPADDR_TYPE_V4 0
typedef struct {struct {int type; union {esp_ip4_addr_t ip4;} u_addr;} ip;} esp_netif_dns_info_t;
static inline int esp_netif_get_dns_info(esp_netif_t *netif, int type, esp_netif_dns_info_t *out)
{(void)netif; (void)type; (void)out; return -1;}
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) (unsigned)((const uint8_t *)&(ip)->addr)[0], \
    (unsigned)((const uint8_t *)&(ip)->addr)[1], \
    (unsigned)((const uint8_t *)&(ip)->addr)[2], \
    (unsigned)((const uint8_t *)&(ip)->addr)[3]
static inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{(void)key; return NULL;}
static inline bool esp_netif_is_netif_up(esp_netif_t *netif)
{(void)netif; return false;}
static inline int esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip)
{(void)netif; (void)ip; return -1;}
#endif
