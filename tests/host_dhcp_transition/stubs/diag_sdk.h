#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t=int;
using err_t=int8_t;
using u16_t=uint16_t;
constexpr esp_err_t ESP_OK=0,ESP_FAIL=-1;
#define ESP_LWIP_DHCP_FINE_TIMERS_ONDEMAND 1
struct dhcp {uint8_t state,tries,fine_timer_enabled;uint32_t request_timeout;};
struct netif {void *state;uint8_t hwaddr_len,hwaddr[6];bool link;dhcp *client;};
struct esp_netif_t {netif *impl;};
struct pbuf {pbuf *next;const void *payload;u16_t tot_len,len;};
struct udp_pcb {u16_t local_port;};
struct ip_addr_t {uint32_t address;};
struct dhcp_msg {uint8_t bytes[309];};
#define netif_dhcp_data(n) ((n)->client)
#define netif_is_link_up(n) ((n)->link)
extern "C" {
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *);
void *esp_netif_get_netif_impl(esp_netif_t *);
esp_err_t esp_netif_get_mac(esp_netif_t *,uint8_t *);
esp_err_t esp_netif_tcpip_exec(esp_err_t (*)(void *),void *);
u16_t pbuf_copy_partial(const pbuf *,void *,u16_t,u16_t);
}

using TaskHandle_t=void *;
constexpr uint32_t MALLOC_CAP_DMA=8,MALLOC_CAP_INTERNAL=0x800,MALLOC_CAP_8BIT=4;
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle();
extern "C" bool xPortInIsrContext();
extern "C" size_t heap_caps_get_free_size(uint32_t);
extern "C" size_t heap_caps_get_largest_free_block(uint32_t);
