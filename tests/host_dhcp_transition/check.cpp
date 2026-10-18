#include <cassert>
#include <cstdio>
#include <vector>
#include "../../firmware/components/katmed_mesh/katmed_dhcp_diag.cpp"
namespace {
esp_netif_t sta,other;
netif local;
dhcp client{6,7,1,8};
bool present=true,in_tcpip=false,exec_fail=false,nested=false;
unsigned udp_calls=0,tx_calls=0,executor_calls=0;
TaskHandle_t host_task=reinterpret_cast<void *>(1);
bool isr=false,helper_recurse=false,nested_foreign=false,nested_target=false;
unsigned malloc_calls=0,resource_reads=0,allocation_mode=0;
size_t seen_size=0;uint32_t seen_caps=0;
void *malloc_result=reinterpret_cast<void *>(7);
extern "C" void *host_target_malloc(size_t,uint32_t) asm("esf_buf_alloc_dynamic");
// Actual x86 call/return attribution: return PC is symbol+0xc0, as on target.
asm(".text\n.global esf_buf_alloc_dynamic\nesf_buf_alloc_dynamic:\n"
    "sub $8,%rsp\n.fill 183,1,0x90\ncall __wrap_heap_caps_malloc\n"
    "add $8,%rsp\nret\n");
err_t udp_result=0;
esp_err_t tx_result=0;
udp_pcb *seen_pcb; pbuf *seen_p;
const ip_addr_t *seen_dest,*seen_source;
netif *seen_netif;
u16_t seen_port;
void *seen_data,*seen_buffer;size_t seen_length;
esp_netif_t *seen_tx_netif;
std::vector<uint8_t> frame;
void put16(std::vector<uint8_t> &p,size_t n,uint16_t x){p[n]=uint8_t(x>>8);p[n+1]=uint8_t(x);}
std::vector<uint8_t> message(uint8_t type=1){
    std::vector<uint8_t> p(244);p[0]=1;p[1]=1;p[2]=6;
    std::memcpy(p.data()+28,local.hwaddr,6);
    std::memcpy(p.data()+236,"\x63\x82\x53\x63",4);
    p[240]=53;p[241]=1;p[242]=type;p[243]=255;return p;
}
std::vector<uint8_t> ethernet_frame(const std::vector<uint8_t> &p,size_t ihl=20){
    std::vector<uint8_t> f(14+ihl+8+p.size());
    std::memcpy(f.data()+6,local.hwaddr,6);put16(f,12,0x0800);
    f[14]=uint8_t(0x40+ihl/4);put16(f,16,uint16_t(ihl+8+p.size()));f[23]=17;
    put16(f,14+ihl,68);put16(f,16+ihl,67);put16(f,18+ihl,uint16_t(8+p.size()));
    std::memcpy(f.data()+14+ihl+8,p.data(),p.size());return f;
}
KatmedDhcpDiagnostics snapshot(){KatmedDhcpDiagnostics d;katmed_dhcp_diag_snapshot(&d);return d;}
}
extern "C" esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key){
    assert(std::strcmp(key,"WIFI_STA_DEF")==0);return present ? &sta : nullptr;
}
extern "C" esp_err_t esp_netif_get_mac(esp_netif_t *n,uint8_t *mac){
    assert(n==&sta);std::memcpy(mac,local.hwaddr,6);return ESP_OK;
}
extern "C" void *esp_netif_get_netif_impl(esp_netif_t *n){assert(in_tcpip);return n->impl;}
extern "C" esp_err_t esp_netif_tcpip_exec(esp_err_t (*fn)(void *),void *arg){
    ++executor_calls;if(exec_fail)return ESP_FAIL;
    assert(!in_tcpip);in_tcpip=true;auto previous=host_task;host_task=reinterpret_cast<void *>(1);
    const auto r=fn(arg);host_task=previous;in_tcpip=false;return r;
}
extern "C" u16_t pbuf_copy_partial(const pbuf *p,void *out,u16_t count,u16_t offset){
    auto *target=static_cast<uint8_t *>(out);size_t copied=0;
    while(p && offset>=p->len){offset=u16_t(offset-p->len);p=p->next;}
    while(p && copied<count){
        const size_t available=size_t(p->len-offset);
        const size_t take=available<(count-copied) ? available : count-copied;
        std::memcpy(target+copied,static_cast<const uint8_t *>(p->payload)+offset,take);
        copied+=take;offset=0;p=p->next;
    }
    return u16_t(copied);
}
extern "C" err_t __real_udp_sendto_if_src(udp_pcb *pcb,pbuf *p,const ip_addr_t *dest,
                                          u16_t port,netif *n,const ip_addr_t *source){
    ++udp_calls;seen_pcb=pcb;seen_p=p;seen_dest=dest;seen_port=port;seen_netif=n;seen_source=source;
    if(nested)__wrap_esp_netif_transmit_wrap(&sta,frame.data(),frame.size(),p);
    return udp_result;
}
extern "C" esp_err_t __real_esp_netif_transmit_wrap(esp_netif_t *n,void *data,size_t len,void *buffer){
    ++tx_calls;seen_tx_netif=n;seen_data=data;seen_length=len;seen_buffer=buffer;
    if(nested_foreign){
        nested_foreign=false;nested_target=true;auto foreign=frame;foreign[6]^=1;
        __wrap_esp_netif_transmit_wrap(&sta,foreign.data(),foreign.size(),buffer);
    }else if(nested_target){
        nested_target=false;
        __wrap_esp_netif_transmit_wrap(&sta,frame.data(),frame.size(),buffer);
    }
    if(allocation_mode){
        if(allocation_mode==3)host_task=reinterpret_cast<void *>(2);
        if(allocation_mode==4)isr=true;
        if(allocation_mode==5)assert(__wrap_heap_caps_malloc(333,0x80c)==malloc_result); // wrong callsite
        else assert(host_target_malloc(333,allocation_mode==6 ? 4 : 0x80c)==malloc_result);
        if(allocation_mode==2)host_target_malloc(444,0x80c);
        host_task=reinterpret_cast<void *>(1);isr=false;
    }
    return tx_result;
}
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(){return host_task;}
extern "C" bool xPortInIsrContext(){return isr;}
extern "C" void *__real_heap_caps_malloc(size_t size,uint32_t caps){
    ++malloc_calls;seen_size=size;seen_caps=caps;return malloc_result;
}
extern "C" size_t heap_caps_get_free_size(uint32_t caps){
    ++resource_reads;assert(caps==0x80c && !isr);
    if(helper_recurse)host_target_malloc(99,caps);
    return 2000;
}
extern "C" size_t heap_caps_get_largest_free_block(uint32_t caps){
    ++resource_reads;assert(caps==0x80c && !isr);return 1000;
}
int main(){
    assert(host_target_malloc(10,4)==malloc_result);
    local={&sta,6,{1,2,3,4,5,6},true,&client};sta.impl=&local;
    assert(snapshot().core_v==0 && executor_calls==0);
    katmed_dhcp_diag_bind_sta();auto d=snapshot();
    assert(d.core_v==1&&d.dhcp_v==1&&d.link_up==1&&d.state==6&&d.tries==7&&d.timeout==8&&d.fine==1);
    local.client=nullptr;d=snapshot();assert(d.core_v==1&&d.dhcp_v==0&&d.state==0);
    local.client=&client;exec_fail=true;assert(snapshot().core_v==0);exec_fail=false;
    sta.impl=nullptr;assert(snapshot().core_v==0);sta.impl=&local;
    auto packet=message();const auto unchanged=packet;
    pbuf tail{nullptr,packet.data()+9,u16_t(packet.size()-9),u16_t(packet.size()-9)};
    pbuf head{&tail,packet.data(),u16_t(packet.size()),9};
    udp_pcb pcb{68};ip_addr_t dest{},source{};
    assert(__wrap_udp_sendto_if_src(&pcb,&head,&dest,67,&local,&source)==0);
    assert(udp_calls==1&&seen_pcb==&pcb&&seen_p==&head&&seen_dest==&dest&&seen_port==67&&seen_netif==&local&&seen_source==&source);
    assert(packet==unchanged);d=snapshot();assert(d.udp_discover==1&&d.udp_ok==1);
    packet=message(3);tail.payload=packet.data()+9;head.payload=packet.data();frame=ethernet_frame(packet,24);
    nested=true;udp_result=-7;tx_result=0x300d;
    assert(__wrap_udp_sendto_if_src(&pcb,&head,&dest,67,&local,&source)==-7);
    assert(udp_calls==2&&tx_calls==1&&seen_tx_netif==&sta&&seen_data==frame.data()&&seen_length==frame.size()&&seen_buffer==&head);
    d=snapshot();assert(d.udp_request==1&&d.udp_fail==1&&d.udp_error==-7&&d.tx_request==1&&d.tx_fail==1&&d.tx_error==0x300d);
    nested=false;const auto before=d;
    pcb.local_port=53;assert(__wrap_udp_sendto_if_src(&pcb,&head,&dest,67,&local,&source)==-7);
    assert(udp_calls==3&&snapshot().udp_fail==before.udp_fail);pcb.local_port=68;
    assert(__wrap_esp_netif_transmit_wrap(&other,frame.data(),frame.size(),&head)==0x300d);
    assert(tx_calls==2&&snapshot().tx_fail==before.tx_fail);
    packet[236]=0;assert(__wrap_udp_sendto_if_src(&pcb,&head,&dest,67,&local,&source)==-7);
    assert(udp_calls==4&&snapshot().udp_invalid==1);
    tail.len=2;assert(__wrap_udp_sendto_if_src(&pcb,&head,&dest,67,&local,&source)==-7);
    assert(udp_calls==5&&snapshot().udp_invalid==2);tail.len=u16_t(packet.size()-9);
    packet=message();frame=ethernet_frame(packet);
    assert(ethernet(frame.data(),frame.size(),local.hwaddr)==Kind::discover);
    auto foreign=frame;foreign[6]^=1;foreign[14+20+8+28]^=1;
    assert(ethernet(foreign.data(),foreign.size(),local.hwaddr)==Kind::none);
    const auto target_before=snapshot();
    assert(__wrap_esp_netif_transmit_wrap(&sta,foreign.data(),foreign.size(),&head)==0x300d);
    assert(snapshot().tx_fail==target_before.tx_fail);
    auto bad=frame;bad[14]=0x44;assert(ethernet(bad.data(),bad.size(),local.hwaddr)==Kind::invalid);
    bad=frame;bad[20]=0x20;assert(ethernet(bad.data(),bad.size(),local.hwaddr)==Kind::invalid);
    assert(ethernet(frame.data(),frame.size()-1,local.hwaddr)==Kind::invalid);
    bad=frame;bad[18+20]=0;bad[19+20]=7;assert(ethernet(bad.data(),bad.size(),local.hwaddr)==Kind::invalid);
    bad=frame;bad[14+20+8+28]^=1;assert(ethernet(bad.data(),bad.size(),local.hwaddr)==Kind::invalid);
    auto options=message();options[241]=7;assert(payload(Bytes{nullptr,options.data(),options.size()},0,options.size(),local.hwaddr)==Kind::invalid);
    options=message();options[243]=0;assert(payload(Bytes{nullptr,options.data(),options.size()},0,options.size(),local.hwaddr)==Kind::invalid);
    // Saturation is exercised on the actual private counter, without a product reset API.
    udp_counts.discover.store(UINT32_MAX);record(udp_counts,Kind::discover,0);
    assert(snapshot().udp_discover==UINT32_MAX);
    // Exercise the new scope independently of the historical classification checks.
    packet=message();frame=ethernet_frame(packet);allocation_mode=1;tx_result=257;
    katmed_dhcp_diag_bind_sta();auto calls_before=malloc_calls;
    __wrap_esp_netif_transmit_wrap(&sta,frame.data(),frame.size(),&head);
    assert(malloc_calls==calls_before+1 && snapshot().resource_v==0); // unbound task
    d=snapshot();assert(d.task_v==1);
    calls_before=malloc_calls;helper_recurse=true;
    assert(__wrap_esp_netif_transmit_wrap(&sta,frame.data(),frame.size(),&head)==257);
    helper_recurse=false;d=snapshot();
    assert(malloc_calls==calls_before+2 && seen_size==333 && seen_caps==0x80c);
    assert(d.resource_v==1 && d.resource_calls==1 && d.resource_ok==1 && d.resource_nulls==0);
    assert(d.resource_size==333 && d.resource_caps==0x80c && d.resource_resource_v==1);
    assert(d.resource_dma_free==2000 && d.resource_dma_largest==1000 && d.resource_result==257);
    malloc_result=nullptr;calls_before=malloc_calls;
    __wrap_esp_netif_transmit_wrap(&sta,frame.data(),frame.size(),&head);d=snapshot();
    assert(malloc_calls==calls_before+1 && d.resource_nulls==1 && d.resource_ok==0);
    for(unsigned mode: {0u,2u,3u,4u,5u,6u}){
        allocation_mode=mode;const auto reads=resource_reads;calls_before=malloc_calls;
        __wrap_esp_netif_transmit_wrap(&sta,frame.data(),frame.size(),&head);d=snapshot();
        assert(d.resource_v==1 && active_tx.load()==nullptr);
        if(mode==2){assert(d.resource_calls==2 && d.resource_nulls==2 && !d.resource_resource_v && !d.resource_size);}
        else if(mode==6){assert(d.resource_calls==1 && d.resource_caps==4 && !d.resource_resource_v);}
        else {assert(d.resource_calls==0 && !d.resource_resource_v);}
        if(mode==0)assert(malloc_calls==calls_before);
        if(mode>=3)assert(resource_reads==reads);
    }
    allocation_mode=1;const auto count=snapshot().resource_count;
    auto foreign_frame=frame;foreign_frame[6]^=1;
    __wrap_esp_netif_transmit_wrap(&sta,foreign_frame.data(),foreign_frame.size(),&head);
    assert(snapshot().resource_count==count); // foreign TX never opens span
    nested_foreign=true;__wrap_esp_netif_transmit_wrap(&sta,frame.data(),frame.size(),&head);
    d=snapshot();assert(d.resource_calls==1 && d.resource_count==count+1);
    host_task=reinterpret_cast<void *>(2);const auto before_foreign=d.resource_count;
    __wrap_esp_netif_transmit_wrap(&sta,frame.data(),frame.size(),&head);
    host_task=reinterpret_cast<void *>(1);assert(snapshot().resource_count==before_foreign);
    isr=true;__wrap_esp_netif_transmit_wrap(&sta,frame.data(),frame.size(),&head);
    isr=false;assert(snapshot().resource_count==before_foreign);
    exec_fail=true;assert(snapshot().task_v==0 && tcpip_task.load()==nullptr);exec_fail=false;
    std::puts("TX resources: exact original calls/returns, real caller PC, task/ISR isolation, nested foreign TX, helper recursion, NULL/success/multiple/no-call OK");
    std::puts("DHCP transition: exact pass-through, chained payload, bounds, errors, TCPIP validity and saturation OK");
}
