#include "katmed_dhcp_diag.h"
#include <atomic>
#include <cstring>
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif_net_stack.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "lwip/prot/dhcp.h"
#include "lwip/udp.h"

namespace {
std::atomic<esp_netif_t *> station{nullptr};
std::atomic<TaskHandle_t> tcpip_task{nullptr};
struct TxResource {
    uint32_t v, count, dhcp_type, calls, ok, nulls, resource_v;
    uint32_t size, caps, dma_free, dma_largest;
    int32_t result;
} last_resource{};
struct TxScope { TxResource value{}; bool sampling=false; };
std::atomic<TxScope *> active_tx{nullptr};
uint32_t tx_depth=0; // Accessed only by the bound TCPIP task.
constexpr uint32_t dma_caps=MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
static_assert(dma_caps==0x80c);
// The unchanged vendor callback tail-calls malloc. Both final ELFs must prove
// the slot-344 call returns to this exact offset; no PC or pointer is exported.
extern "C" void *esf_buf_alloc_dynamic(void *,size_t,void *);
bool IRAM_ATTR target_task(){
    return !xPortInIsrContext() &&
        xTaskGetCurrentTaskHandle()==tcpip_task.load(std::memory_order_acquire);
}
struct Counts {
    std::atomic<uint32_t> discover{0}, request{0}, other{0}, invalid{0}, ok{0}, fail{0};
    std::atomic<int32_t> error{0};
} udp_counts, tx_counts;
void increment(std::atomic<uint32_t> &value){
    uint32_t old=value.load(std::memory_order_relaxed);
    while(old!=UINT32_MAX && !value.compare_exchange_weak(old,old+1,std::memory_order_relaxed)){}
}
enum class Kind { none, invalid, discover, request, other };
/* One bounded view, including chained pbufs. No packet is retained or changed. */
struct Bytes {
    const pbuf *chain;
    const uint8_t *data;
    size_t length;
    bool read(size_t offset,void *out,size_t count) const {
        if(count>length || offset>length-count)return false;
        if(chain)return pbuf_copy_partial(chain,out,u16_t(count),u16_t(offset))==count;
        if(!data)return false;
        std::memcpy(out,data+offset,count);return true;
    }
    bool byte(size_t offset,uint8_t &out) const {return read(offset,&out,1);}
};
Kind payload(const Bytes &bytes,size_t start,size_t length,const uint8_t *mac){
    // The existing SDK bounds its own outgoing DHCP messages to this structure.
    if(length<240 || length>sizeof(dhcp_msg))return Kind::invalid;
    uint8_t boot[3],chaddr[6],cookie[4];
    if(!bytes.read(start,boot,sizeof(boot)) || boot[0]!=1 || boot[1]!=1 || boot[2]!=6 ||
       !bytes.read(start+28,chaddr,sizeof(chaddr)) || std::memcmp(chaddr,mac,6)!=0 ||
       !bytes.read(start+236,cookie,sizeof(cookie)) ||
       std::memcmp(cookie,"\x63\x82\x53\x63",4)!=0)return Kind::invalid;
    uint8_t type=0;bool ended=false;
    size_t offset=240;
    while(offset<length){
        uint8_t option;
        if(!bytes.byte(start+offset++,option))return Kind::invalid;
        if(option==255){ended=true;break;}
        if(option==0)continue;
        uint8_t size;
        if(offset>=length || !bytes.byte(start+offset++,size) || size>length-offset)
            return Kind::invalid;
        if(option==53){
            if(type || size!=1 || !bytes.byte(start+offset,type) || type<1 || type>8)
                return Kind::invalid;
        }
        offset+=size;
    }
    if(!ended || !type)return Kind::invalid;
    return type==1 ? Kind::discover : type==3 ? Kind::request : Kind::other;
}
uint16_t be16(const uint8_t *p){return uint16_t(uint16_t(p[0])<<8)|p[1];}
Kind ethernet(const void *data,size_t length,const uint8_t *mac){
    Bytes bytes{nullptr,static_cast<const uint8_t *>(data),length};
    uint8_t eth[14],ip[20],udp[8];
    if(!bytes.read(0,eth,sizeof(eth)) || be16(eth+12)!=0x0800 ||
       std::memcmp(eth+6,mac,6)!=0)return Kind::none;
    if(!bytes.read(14,ip,sizeof(ip)) || (ip[0]>>4)!=4)return Kind::invalid;
    const size_t ihl=(ip[0]&15u)*4u,total=be16(ip+2);
    if(ip[9]!=17)return Kind::none;
    if(ihl<20 || total<ihl+8 || length<14+total || (be16(ip+6)&0x3fff))
        return Kind::invalid;
    if(!bytes.read(14+ihl,udp,sizeof(udp)))return Kind::invalid;
    if(be16(udp)!=68 || be16(udp+2)!=67)return Kind::none;
    const size_t udp_length=be16(udp+4);
    if(udp_length<8 || udp_length!=total-ihl)return Kind::invalid;
    return payload(bytes,14+ihl+8,udp_length-8,mac);
}
void record(Counts &counts,Kind kind,int32_t result){
    if(kind==Kind::none)return;
    switch(kind){
    case Kind::discover:increment(counts.discover);break;
    case Kind::request:increment(counts.request);break;
    case Kind::other:increment(counts.other);break;
    default:increment(counts.invalid);break;
    }
    if(result==0)increment(counts.ok);
    else {increment(counts.fail);counts.error.store(result,std::memory_order_relaxed);}
}
esp_err_t snapshot_core(void *context){
    auto *out=static_cast<KatmedDhcpDiagnostics *>(context);
    esp_netif_t *sta=station.load(std::memory_order_acquire);
    auto *netif=sta ? static_cast<struct netif *>(esp_netif_get_netif_impl(sta)) : nullptr;
    if(!netif){tcpip_task.store(nullptr,std::memory_order_release);return ESP_OK;}
    const TaskHandle_t current=xTaskGetCurrentTaskHandle();
    if(tcpip_task.load(std::memory_order_acquire)!=current)last_resource={};
    tcpip_task.store(current,std::memory_order_release);
    out->task_v=1;
#define RESOURCE(field) out->resource_##field=last_resource.field
    RESOURCE(v);RESOURCE(count);RESOURCE(dhcp_type);RESOURCE(calls);RESOURCE(ok);
    RESOURCE(nulls);RESOURCE(resource_v);RESOURCE(size);RESOURCE(caps);
    RESOURCE(dma_free);RESOURCE(dma_largest);RESOURCE(result);
#undef RESOURCE
    out->core_v=1;out->link_up=netif_is_link_up(netif)!=0;
    const struct dhcp *client=netif_dhcp_data(netif);
    if(!client)return ESP_OK;
    out->dhcp_v=1;out->state=client->state;out->tries=client->tries;
    out->timeout=client->request_timeout;
#if ESP_LWIP_DHCP_FINE_TIMERS_ONDEMAND
    out->fine=client->fine_timer_enabled!=0;
#endif
    return ESP_OK;
}
}
extern "C" err_t __real_udp_sendto_if_src(udp_pcb *,pbuf *,const ip_addr_t *,u16_t,
                                          netif *,const ip_addr_t *);
extern "C" err_t __wrap_udp_sendto_if_src(udp_pcb *pcb,pbuf *p,const ip_addr_t *dest,
                                          u16_t port,netif *netif,const ip_addr_t *source){
    Kind kind=Kind::none;
    esp_netif_t *sta=station.load(std::memory_order_acquire);
    if(sta && netif && netif->state==sta && pcb && pcb->local_port==68 && port==67){
        kind=(p && netif->hwaddr_len==6)
            ? payload(Bytes{p,nullptr,p->tot_len},0,p->tot_len,netif->hwaddr) : Kind::invalid;
    }
    const err_t result=__real_udp_sendto_if_src(pcb,p,dest,port,netif,source);
    record(udp_counts,kind,result);return result;
}
extern "C" esp_err_t __real_esp_netif_transmit_wrap(esp_netif_t *,void *,size_t,void *);
extern "C" esp_err_t __wrap_esp_netif_transmit_wrap(esp_netif_t *netif,void *data,
                                                   size_t length,void *buffer){
    esp_netif_t *sta=station.load(std::memory_order_acquire);
    const bool owner=target_task() && tcpip_task.load(std::memory_order_acquire);
    TxScope *previous=owner ? active_tx.load(std::memory_order_acquire) : nullptr;
    if(owner)active_tx.store(nullptr,std::memory_order_release);
    const bool outermost=owner && tx_depth++==0;
    Kind kind=Kind::none;
    if(sta && netif==sta){
        uint8_t mac[6];
        kind=esp_netif_get_mac(sta,mac)==ESP_OK ? ethernet(data,length,mac) : Kind::invalid;
    }
    // Every nested L2 call suspends the outer span, including foreign traffic.
    TxScope scope;
    const bool observe=outermost && (kind==Kind::discover || kind==Kind::request);
    if(observe)active_tx.store(&scope,std::memory_order_release);
    const esp_err_t result=__real_esp_netif_transmit_wrap(netif,data,length,buffer);
    if(owner){--tx_depth;active_tx.store(previous,std::memory_order_release);}
    if(observe){
        scope.value.v=1;scope.value.result=result;
        scope.value.dhcp_type=kind==Kind::discover ? 1 : 3;
        scope.value.count=last_resource.count==UINT32_MAX ? UINT32_MAX : last_resource.count+1;
        // Multiple calls never present the last allocation as a unique sample.
        if(scope.value.calls!=1){
            scope.value.resource_v=scope.value.size=scope.value.caps=0;
            scope.value.dma_free=scope.value.dma_largest=0;
        }
        last_resource=scope.value;
    }
    record(tx_counts,kind,result);return result;
}
extern "C" void *__real_heap_caps_malloc(size_t,uint32_t);
extern "C" __attribute__((noinline)) void *IRAM_ATTR __wrap_heap_caps_malloc(size_t size,uint32_t caps){
    TxScope *scope=nullptr;
    if(active_tx.load(std::memory_order_acquire) && target_task()){
        auto *candidate=active_tx.load(std::memory_order_acquire);
        if(candidate && !candidate->sampling &&
           reinterpret_cast<uintptr_t>(__builtin_return_address(0))==
               reinterpret_cast<uintptr_t>(&esf_buf_alloc_dynamic)+0xc0)
            scope=candidate;
    }
    if(scope){
        scope->sampling=true;
        if(scope->value.calls!=UINT32_MAX)++scope->value.calls;
        scope->value.size=size;scope->value.caps=caps;
        scope->value.resource_v=caps==dma_caps;
        if(caps==dma_caps){
            scope->value.dma_free=heap_caps_get_free_size(caps);
            scope->value.dma_largest=heap_caps_get_largest_free_block(caps);
        }
    }
    void *result=__real_heap_caps_malloc(size,caps);
    if(scope){
        auto &count=result ? scope->value.ok : scope->value.nulls;
        if(count!=UINT32_MAX)++count;
        scope->sampling=false;
    }
    return result;
}
extern "C" void katmed_dhcp_diag_bind_sta(void){
    tcpip_task.store(nullptr,std::memory_order_release);
    station.store(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"),std::memory_order_release);
}
extern "C" void katmed_dhcp_diag_snapshot(KatmedDhcpDiagnostics *out){
    if(!out)return;
    *out={};
    // A not-yet-configured runtime may not have initialized the TCPIP thread.
    if(station.load(std::memory_order_acquire) && esp_netif_tcpip_exec(snapshot_core,out)!=ESP_OK){
        tcpip_task.store(nullptr,std::memory_order_release);
        out->core_v=out->dhcp_v=out->link_up=out->state=out->tries=out->timeout=out->fine=0;
    }
#define COPY(prefix,counts) \
    out->prefix##_discover=counts.discover.load(std::memory_order_relaxed); \
    out->prefix##_request=counts.request.load(std::memory_order_relaxed); \
    out->prefix##_other=counts.other.load(std::memory_order_relaxed); \
    out->prefix##_invalid=counts.invalid.load(std::memory_order_relaxed); \
    out->prefix##_ok=counts.ok.load(std::memory_order_relaxed); \
    out->prefix##_fail=counts.fail.load(std::memory_order_relaxed); \
    out->prefix##_error=counts.error.load(std::memory_order_relaxed)
    COPY(udp,udp_counts);COPY(tx,tx_counts);
#undef COPY
}
