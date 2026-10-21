#include "katmed_names_wire.h"
#include <cstring>
namespace {
void wire_put(uint8_t *p,uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)p[i]=uint8_t(v>>(8*(n-i-1)));}
uint64_t wire_get(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v=(v<<8)|p[i];return v;}
bool wire_zero(const uint8_t *p,size_t n){unsigned v=0;while(n--)v|=*p++;return !v;}
bool wire_valid(const NamesMessage &m){
    if(m.opcode<1||m.opcode>11||m.flags&~31||((m.flags&3)==3)||
       m.barrier_count>8||m.barrier_flags&~3||m.status>10||
       (m.opcode!=NAMES_ERROR&&m.status)||(m.opcode==NAMES_ERROR&&!m.status)||
       bool(m.reserved_device)!=bool(m.reserved_index)||
       bool(m.flags&16)!=bool(m.reserved_device))return false;
    for(unsigned i=0;i<8;++i){
        if((i<m.barrier_count)!=bool(m.barrier[i])||(m.barrier[i]&&m.barrier[i]==m.controller))return false;
        for(unsigned j=0;j<i;++j)if(m.barrier[i]&&m.barrier[i]==m.barrier[j])return false;
    }
    if(m.opcode==NAMES_CAPS||m.opcode==NAMES_HEARTBEAT)
        return !m.flags&&!m.epoch&&!m.controller&&wire_zero(m.registry,16)&&
            wire_zero(m.base,80)&&wire_zero(m.target,80)&&!m.progress&&!m.start&&
            wire_zero(m.progress_hash,32)&&!m.reserved_device&&!m.barrier_count&&!m.barrier_flags;
    if(wire_zero(m.registry,16)||!m.controller||!m.epoch||!names_descriptor_valid(m.target)||
       (!wire_zero(m.base,80)&&!names_descriptor_valid(m.base)))return false;
    /* Descriptor slots are little endian in this API. */
    uint32_t slots=0;for(unsigned i=0;i<4;++i)slots|=uint32_t(m.target[64+i])<<(8*i);
    return m.progress<=slots&&m.start<=slots&&(!m.reserved_device||m.reserved_index<=slots);
}
}
extern "C" bool names_wire_encode(const NamesMessage *m,const uint64_t *ids,size_t count,uint8_t *out,size_t capacity,size_t *length){
    if(!m||!out||!length||!wire_valid(*m)||count>89||
       (m->opcode==NAMES_PAGE?(!count||!ids):count!=0)||capacity<328+8*count)return false;
    uint32_t slots=0;for(unsigned i=0;i<4;++i)slots|=uint32_t(m->target[64+i])<<(8*i);
    if(count>slots-m->start)return false;
    std::memset(out,0,328);out[0]=2;out[1]=m->opcode;wire_put(out+2,m->flags,2);
    wire_put(out+4,8*count,2);wire_put(out+6,count,2);wire_put(out+8,m->epoch,8);
    std::memcpy(out+16,m->registry,16);wire_put(out+32,m->controller,8);
    names_descriptor_wire(out+40,m->base,true);names_descriptor_wire(out+120,m->target,true);
    wire_put(out+200,m->progress,4);wire_put(out+204,m->start,4);std::memcpy(out+208,m->progress_hash,32);
    wire_put(out+240,m->reserved_device,8);wire_put(out+248,m->reserved_index,2);
    out[250]=m->barrier_count;out[251]=m->barrier_flags;
    for(unsigned i=0;i<8;++i)wire_put(out+252+8*i,m->barrier[i],8);
    if(m->opcode==NAMES_CAPS){wire_put(out+316,2,2);wire_put(out+318,2,2);wire_put(out+320,30000,4);}
    wire_put(out+324,m->status,2);
    for(size_t i=0;i<count;++i)wire_put(out+328+8*i,ids[i],8);
    *length=328+8*count;return true;
}
extern "C" bool names_wire_decode(const uint8_t *p,size_t n,NamesMessage *out){
    if(!p||!out||n<328||n>1040||p[0]!=2||p[326]||p[327])return false;
    const size_t count=size_t(wire_get(p+6,2));
    if(count>89||wire_get(p+4,2)!=8*count||n!=328+8*count||
       (p[1]==NAMES_PAGE?count==0:count!=0))return false;
    if(p[1]==NAMES_CAPS){if(wire_get(p+316,2)!=2||wire_get(p+318,2)!=2||wire_get(p+320,4)!=30000)return false;}
    else if(!wire_zero(p+316,8))return false;
    NamesMessage m{};m.opcode=p[1];m.flags=uint16_t(wire_get(p+2,2));m.epoch=wire_get(p+8,8);
    std::memcpy(m.registry,p+16,16);m.controller=wire_get(p+32,8);
    names_descriptor_wire(m.base,p+40,false);names_descriptor_wire(m.target,p+120,false);
    m.progress=uint32_t(wire_get(p+200,4));m.start=uint32_t(wire_get(p+204,4));
    std::memcpy(m.progress_hash,p+208,32);m.reserved_device=wire_get(p+240,8);
    m.reserved_index=uint16_t(wire_get(p+248,2));m.barrier_count=p[250];m.barrier_flags=p[251];
    for(unsigned i=0;i<8;++i)m.barrier[i]=wire_get(p+252+8*i,8);
    m.status=uint16_t(wire_get(p+324,2));
    if(!wire_valid(m))return false;
    uint32_t slots=0;for(unsigned i=0;i<4;++i)slots|=uint32_t(m.target[64+i])<<(8*i);
    if(count>slots-m.start)return false;
    *out=m;return true;
}
extern "C" uint64_t names_wire_slot(const uint8_t *payload,size_t index){return index<89?wire_get(payload+328+8*index,8):0;}
