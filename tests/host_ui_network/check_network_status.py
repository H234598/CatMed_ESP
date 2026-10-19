#!/usr/bin/env python3
"""Compile the actual read-only main callback against controlled SDK states."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
main = (root / 'firmware/main/main.c').read_text()
callback = (main[main.index('static void network_status('):main.index('static void web_wipe(')] +
            main[main.index('static bool web_device_info('):main.index('static bool group_connected(')])
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "katmed_ui.h"
typedef struct {char device_name[24]; bool assigned, uncertain;} KatmedWebDeviceInfo;
typedef struct {bool store_ready, mesh_started; struct {struct {struct {bool configured;} provision; char mesh_name[24]; unsigned short mesh_name_index;} record; bool uncertain;} store;} Runtime;
typedef struct {unsigned addr;} Address;
typedef Address esp_ip4_addr_t;
typedef struct {struct {int type; union {Address ip4;} u_addr;} ip;} esp_netif_dns_info_t;
#define ESP_NETIF_DNS_MAIN 0
#define ESP_IPADDR_TYPE_V4 0
static bool fail_dns, unknown_dns, ipv6_dns, disconnect_dns;
static unsigned sync_server=0x017100CB;
static bool mesh_active=true;
static uint8_t radio_channel=6;
static bool katmed_mesh_radio_channel(uint8_t *channel) {*channel=radio_channel;return mesh_active;}
typedef struct {unsigned short current,history; unsigned char quality; bool active,current_valid,history_complete;} KatmedMeshCounts;
static void katmed_mesh_counts(KatmedMeshCounts *out){*out=(KatmedMeshCounts){2,3,16,true,true,true};}
static unsigned katmed_time_idf_sync_server_ipv4(void) {return sync_server;}
static int esp_netif_get_dns_info(void *netif, int kind, esp_netif_dns_info_t *out);
typedef struct {Address ip, gw;} esp_netif_ip_info_t;
typedef struct {bool up;} esp_netif_t;
typedef struct {int unused;} wifi_ap_record_t;
typedef enum {WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA} wifi_mode_t;
#define ESP_OK 0
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) (unsigned)((unsigned char *)&(ip)->addr)[0], (unsigned)((unsigned char *)&(ip)->addr)[1], (unsigned)((unsigned char *)&(ip)->addr)[2], (unsigned)((unsigned char *)&(ip)->addr)[3]
static esp_netif_t sta = {true}, ap = {true};
static bool associated = true, fail_read, disconnect_mid_read, zero_ip, zero_gateway;
static wifi_mode_t mode = WIFI_MODE_APSTA;
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {return !strcmp(key,"WIFI_STA_DEF") ? &sta : &ap;}
static bool esp_netif_is_netif_up(esp_netif_t *netif) {return netif->up;}
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *out) {(void)out; return associated ? 0 : -1;}
static int esp_wifi_get_mode(wifi_mode_t *out) {*out=mode; return 0;}
static int esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *out) {
    out->ip = (Address){netif==&sta ? 0x0A0200C0 : 0x810200C0};
    out->gw = (Address){0x010200C0};
    if (netif==&sta && zero_ip) out->ip.addr=0;
    if (zero_gateway) out->gw.addr=0;
    if (disconnect_mid_read && netif==&sta) associated=false;
    return fail_read ? -1 : 0;
}
static int esp_netif_get_dns_info(void *netif, int kind, esp_netif_dns_info_t *out) {
    assert(netif==&sta && kind==ESP_NETIF_DNS_MAIN);
    out->ip.type=ipv6_dns ? 6 : ESP_IPADDR_TYPE_V4;
    out->ip.u_addr.ip4.addr=unknown_dns ? 0 : 0x350200C0;
    if(disconnect_dns) associated=false;
    return fail_dns ? -1 : ESP_OK;
}
'''
tests = r'''
int main(void) {
    Runtime rt = {.store_ready=true,.mesh_started=true};
    rt.store.record.provision.configured=true;
    strcpy(rt.store.record.mesh_name, "KatmedESP-002");
    rt.store.record.mesh_name_index=2;
    UiNetworkStatus out;
    network_status(&rt,&out);
    assert(out.mesh_active && out.mesh_channel==6);
    assert(out.mesh_current_count==2 && out.mesh_history_count==3 && out.mesh_current_valid && out.mesh_history_complete);
    assert(out.configured && out.connected);
    assert(!strcmp(out.device_name,"KatmedESP-002") && out.device_name_assigned && !out.device_name_uncertain);
    KatmedWebDeviceInfo info;
    assert(web_device_info(&rt,&info) && !strcmp(info.device_name,"KatmedESP-002") &&
           info.assigned && !info.uncertain);
    assert(!strcmp(out.station_ip,"192.0.2.10") && !strcmp(out.gateway,"192.0.2.1"));
    assert(!strcmp(out.ap_ip,"192.0.2.129"));
    assert(!strcmp(out.dns_ip,"192.0.2.53") && !strcmp(out.sync_server_ip,"203.0.113.1"));
    fail_dns=true; sync_server=0;
    network_status(&rt,&out);
    assert(out.connected && !out.dns_ip[0] && !out.sync_server_ip[0]);
    fail_dns=false; unknown_dns=true;
    network_status(&rt,&out); assert(!out.dns_ip[0]);
    unknown_dns=false; ipv6_dns=true;
    network_status(&rt,&out); assert(!out.dns_ip[0]);
    ipv6_dns=false; disconnect_dns=true;
    network_status(&rt,&out);
    assert(!out.connected && !out.dns_ip[0] && !out.sync_server_ip[0]);
    disconnect_dns=false;
    associated=false;
    network_status(&rt,&out);
    assert(!out.connected && !out.station_ip[0] && !out.gateway[0] && !out.dns_ip[0] && !out.sync_server_ip[0]);
    associated=true; sta.up=false;
    network_status(&rt,&out);
    assert(!out.connected && !out.station_ip[0]);
    sta.up=true; disconnect_mid_read=true;
    network_status(&rt,&out);
    assert(!out.connected && !out.station_ip[0] && !out.gateway[0] && !out.dns_ip[0] && !out.sync_server_ip[0]);
    associated=true; disconnect_mid_read=false; fail_read=true;
    network_status(&rt,&out);
    assert(!out.connected && !out.station_ip[0] && !out.ap_ip[0]);
    fail_read=false; mode=WIFI_MODE_STA;
    network_status(&rt,&out);
    assert(out.connected && !out.ap_ip[0]);
    zero_ip=true;
    network_status(&rt,&out);
    assert(!out.connected && !out.station_ip[0] && !out.gateway[0] && !out.dns_ip[0] && !out.sync_server_ip[0]);
    zero_ip=false; zero_gateway=true;
    network_status(&rt,&out);
    assert(out.connected && !out.gateway[0]);
    rt.mesh_started=false;
    network_status(&rt,&out);
    assert(out.configured && !out.connected && !out.station_ip[0] && !out.ap_ip[0]);
    rt.mesh_started=true;
    rt.store.record.provision.configured=false;
    network_status(&rt,&out);
    assert(!out.configured && !out.connected && !out.station_ip[0] && !out.gateway[0] && !out.ap_ip[0]);
    network_status(NULL,&out);
    assert(!out.configured && !out.connected);
    puts("Current network callback: association, interface, transition, error, AP and unconfigured cases passed");
}
'''
with tempfile.TemporaryDirectory(prefix='c3-ui-network-') as tmp:
    src = Path(tmp) / 'check.c'
    binary = Path(tmp) / 'check'
    src.write_text(stubs + callback + tests)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'firmware/components/katmed_ui/include'),
                    '-I'+str(root/'firmware/components/katmed_core/include'),
                    str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Exercise the actual Mesh adapter channel reader as well as main's forwarding.
mesh = (root/'firmware/components/katmed_mesh/katmed_mesh_idf.cpp').read_text()
reader = mesh[mesh.index('extern "C" bool katmed_mesh_radio_channel('):
              mesh.index('extern "C" void katmed_mesh_history_backend(')]
channel_stubs = r'''
#include <cassert>
#include <cstdint>
#define ESP_OK 0
using wifi_second_chan_t = int;
static bool active=true, read_error=false;
static unsigned reads=0;
static uint8_t actual=11;
static bool current_group(){return active;}
static int esp_wifi_get_channel(uint8_t *primary,wifi_second_chan_t *secondary){
    ++reads;*primary=actual;*secondary=0;return read_error ? -1 : ESP_OK;
}
'''
channel_tests = r'''
int main(){
    uint8_t channel=99;
    assert(!katmed_mesh_radio_channel(nullptr) && reads==0);
    active=false;
    assert(!katmed_mesh_radio_channel(&channel) && channel==0 && reads==0);
    active=true;
    assert(katmed_mesh_radio_channel(&channel) && channel==11);
    actual=1;assert(katmed_mesh_radio_channel(&channel) && channel==1);
    actual=14;assert(katmed_mesh_radio_channel(&channel) && channel==14);
    actual=0;assert(katmed_mesh_radio_channel(&channel) && channel==0);
    actual=15;assert(katmed_mesh_radio_channel(&channel) && channel==0);
    actual=6;read_error=true;
    assert(katmed_mesh_radio_channel(&channel) && channel==0);
}
'''
with tempfile.TemporaryDirectory(prefix='katmed-radio-channel-') as tmp:
    src=Path(tmp)/'check.cpp'
    binary=Path(tmp)/'check'
    src.write_text(channel_stubs+reader+channel_tests)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('Actual Mesh radio channel: inactive, live changes, invalid and SDK-error cases passed')
