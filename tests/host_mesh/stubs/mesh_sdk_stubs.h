#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t=int;
constexpr esp_err_t ESP_OK=0;
constexpr esp_err_t ESP_FAIL=-1;
constexpr esp_err_t ESP_ERR_INVALID_STATE=0x103;
using esp_event_base_t=const char *;
inline constexpr char kHostIpEvent[]="IP_EVENT";
#define IP_EVENT kHostIpEvent
constexpr int32_t IP_EVENT_STA_GOT_IP=1;
constexpr int32_t IP_EVENT_STA_LOST_IP=2;
using esp_event_handler_t=void (*)(void *,esp_event_base_t,int32_t,void *);
esp_err_t esp_event_loop_create_default();
esp_err_t esp_event_handler_register(esp_event_base_t,int32_t,esp_event_handler_t,void *);
esp_err_t esp_netif_init();
struct esp_netif_t {};
struct esp_netif_ip_info_t { struct { uint32_t addr; } ip; };
enum esp_netif_dhcp_status_t { ESP_NETIF_DHCP_INIT, ESP_NETIF_DHCP_STARTED, ESP_NETIF_DHCP_STOPPED };
inline esp_netif_t host_station;
inline bool host_station_present=false, host_station_up=false;
inline char host_hostname[32]{};
inline uint32_t host_station_ip=0;
inline esp_err_t host_station_ip_result=ESP_OK, host_station_dhcp_result=ESP_OK;
inline esp_netif_dhcp_status_t host_station_dhcp=ESP_NETIF_DHCP_INIT;
inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key){
    return host_station_present && key && __builtin_strcmp(key,"WIFI_STA_DEF")==0
        ? &host_station : nullptr;
}
inline esp_err_t esp_netif_get_ip_info(esp_netif_t *,esp_netif_ip_info_t *out){
    out->ip.addr=host_station_ip;return host_station_ip_result;
}
inline esp_err_t esp_netif_dhcpc_get_status(esp_netif_t *,esp_netif_dhcp_status_t *out){
    *out=host_station_dhcp;return host_station_dhcp_result;
}
inline bool esp_netif_is_netif_up(esp_netif_t *){return host_station_up;}
inline esp_err_t esp_netif_set_hostname(esp_netif_t *,const char *name){
    if(name) __builtin_strncpy(host_hostname,name,sizeof(host_hostname)-1);
    return ESP_OK;
}
void esp_bridge_create_all_netif();
void esp_fill_random(void *,size_t);
struct esp_app_desc_t { char version[32]; };
const esp_app_desc_t *esp_app_get_description();

#define ROOT 1
#define ESPNOW_PAYLOAD_MAX_LEN 1470
struct mesh_lite_sta_config_t { uint8_t ssid[32]; uint8_t password[64]; };
struct esp_mesh_lite_config_t {
    uint8_t vendor_id[2],mesh_id,max_node_number;
    bool join_mesh_ignore_router_status,join_mesh_without_configured_wifi,leaf_node;
    const char *softap_ssid,*softap_password;
};
#define ESP_MESH_LITE_DEFAULT_INIT() {}
using raw_msg_process_cb_t=esp_err_t (*)(uint8_t *,uint32_t,uint8_t **,uint32_t *,uint32_t);
struct esp_mesh_lite_raw_msg_action_t { uint32_t msg_id,resp_msg_id;raw_msg_process_cb_t raw_process; };
struct esp_mesh_lite_msg_config_t {
    struct {uint32_t msg_id,expect_resp_msg_id,max_retry;const uint8_t *data;size_t size;
            esp_err_t (*raw_resend)(const uint8_t *,size_t);} raw_msg;
};
constexpr int ESP_MESH_LITE_RAW_MSG=1;
void esp_mesh_lite_init(esp_mesh_lite_config_t *);
void esp_mesh_lite_start();
uint8_t esp_mesh_lite_get_level();
esp_err_t esp_mesh_lite_disconnect();
esp_err_t esp_mesh_lite_set_argot(uint32_t);
esp_err_t esp_mesh_lite_set_softap_info(const char *,const char *);
esp_err_t esp_mesh_lite_set_router_config(mesh_lite_sta_config_t *);
esp_err_t esp_mesh_lite_raw_msg_action_list_register(const esp_mesh_lite_raw_msg_action_t *);
esp_err_t esp_mesh_lite_send_msg(int,esp_mesh_lite_msg_config_t *);
esp_err_t esp_mesh_lite_send_broadcast_raw_msg_to_child(const uint8_t *,size_t);
esp_err_t esp_mesh_lite_send_raw_msg_to_root(const uint8_t *,size_t);

using QueueHandle_t=void *;
constexpr int pdTRUE=1;
QueueHandle_t xQueueCreate(unsigned,size_t);
int xQueueSend(QueueHandle_t,const void *,unsigned);
int xQueueReceive(QueueHandle_t,void *,unsigned);
int xQueueReset(QueueHandle_t);
