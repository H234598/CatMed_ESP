#include "katmed_mesh_status.h"
#include <cstring>
#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#else
#include <zlib.h>
#endif
namespace {
uint64_t get(const uint8_t *p,unsigned n){uint64_t v=0;while(n--)v=(v<<8)|*p++;return v;}
void put(uint8_t *p,uint64_t v,unsigned n){while(n){p[--n]=uint8_t(v);v>>=8;}}
uint32_t crc(const uint8_t *p,size_t n){
#ifdef ESP_PLATFORM
    return esp_rom_crc32_le(0,p,n);
#else
    return uint32_t(crc32(0,p,n));
#endif
}
bool bit(const uint8_t *b,unsigned i){return (b[i/8]&(1u<<(i%8)))!=0;}
void setbit(uint8_t *b,unsigned i){b[i/8]|=uint8_t(1u<<(i%8));}
void clearbit(uint8_t *b,unsigned i){b[i/8]&=uint8_t(~(1u<<(i%8)));}
int index(const MeshHistory *h,uint64_t id){for(unsigned i=0;i<h->count;++i)if(h->ids[i]==id)return int(i);return -1;}
bool contains(const MeshHistory *h,uint64_t id){
    for(unsigned i=0;i<h->count;++i)if(h->ids[i]==id)return true;
    return false;
}
}
static_assert(sizeof(MeshHistory)<=2208,"History static budget");
static_assert(sizeof(MeshStatusState)<=448,"Status context delta budget");
static_assert(sizeof(MeshHistory)+sizeof(MeshStatusState)<=2656,"Total status budget");
extern "C" bool mesh_history_add(MeshHistory *h,uint64_t id){
    if(!h||!id||contains(h,id))return false;
    if(h->count==MESH_HISTORY_MAX){
        if(!h->saturated){h->saturated=true;h->dirty=true;++h->generation;}
        return false;
    }
    setbit(h->processed,h->count);clearbit(h->baseline,h->count);
    h->ids[h->count++]=id;h->dirty=true;++h->generation;return true;
}
extern "C" bool mesh_history_encode(const MeshHistory *h,uint8_t *b){
    if(!h||!b||h->count>MESH_HISTORY_MAX)return false;
    std::memset(b,0,MESH_HISTORY_BLOB);std::memcpy(b,"KMH2",4);
    b[4]=2;b[5]=uint8_t(16|(h->saturated?1:0));put(b+6,h->count,2);
    std::memcpy(b+8,h->group,16);
    for(unsigned i=0;i<h->count;++i)put(b+28+8*i,h->ids[i],8);
    std::memcpy(b+2076,h->admitted,32);std::memcpy(b+2108,h->processed,32);
    put(b+2140,h->generation,8);put(b+2148,crc(b,2148),4);return true;
}
extern "C" StoreResult mesh_history_decode(MeshHistory *h,const uint8_t *b,size_t n,const uint8_t group[16]){
    if(!h||!b||!group)return STORE_CORRUPT;
    const bool legacy=n==MESH_HISTORY_LEGACY_BLOB;
    if((!legacy&&n!=MESH_HISTORY_BLOB)||
       std::memcmp(b,legacy?"KMH1":"KMH2",4)||b[4]!=(legacy?1:2)||(b[5]&~17u)||!(b[5]&16)||
       get(b+24,4)||get(b+6,2)>256||
       get(b+(legacy?2076:2148),4)!=crc(b,legacy?2076:2148))return STORE_CORRUPT;
    if(std::memcmp(b+8,group,16))return STORE_STALE;
    unsigned count=unsigned(get(b+6,2));
    for(unsigned i=0;i<256;++i){
        uint64_t id=get(b+28+8*i,8);
        if(i>=count){if(id)return STORE_CORRUPT;continue;}
        if(!id)return STORE_CORRUPT;
        for(unsigned j=0;j<i;++j)if(id==get(b+28+8*j,8))return STORE_CORRUPT;
    }
    for(unsigned i=0;i<256;++i)h->ids[i]=get(b+28+8*i,8);
    std::memcpy(h->group,group,16);h->count=uint16_t(count);
    h->saturated=(b[5]&1)!=0;h->dirty=legacy;h->error=false;
    std::memset(h->baseline,0xff,32);
    h->generation=legacy?1:get(b+2140,8);if(!h->generation)return STORE_CORRUPT;
    std::memset(h->admitted,0,32);std::memset(h->processed,0xff,32);
    if(!legacy){
        std::memcpy(h->admitted,b+2076,32);std::memcpy(h->processed,b+2108,32);
        // Loaded facts are boot baseline; never resume an old popup or infer UTC.
        for(unsigned i=0;i<h->count;++i)if(!bit(h->processed,i)){setbit(h->processed,i);h->dirty=true;}
    }
    return STORE_OK;
}
extern "C" void mesh_history_open(MeshHistory *h,const uint8_t group[16],uint8_t *scratch){
    StoreBackend backend=h->backend;*h=MeshHistory{};h->backend=backend;
    std::memcpy(h->group,group,16);h->generation=1;
    size_t length=MESH_HISTORY_BLOB;
    StoreResult r=backend.read?backend.read(backend.context,scratch,&length):STORE_UNAVAILABLE;
    if(r==STORE_OK)r=mesh_history_decode(h,scratch,length,group);
    h->error=r!=STORE_OK&&r!=STORE_NOT_FOUND&&r!=STORE_STALE;
    h->read_failed=h->error;
}
extern "C" void mesh_history_commit(MeshHistory *h,uint8_t *scratch){
    if(!h->dirty||h->read_failed)return;
    h->dirty=false;
    if(!mesh_history_encode(h,scratch)||!h->backend.write_commit){h->error=true;return;}
    h->error=h->backend.write_commit(h->backend.context,scratch,MESH_HISTORY_BLOB)!=STORE_OK;
    if(!h->error&&h->self_initial){int at=index(h,h->self_initial);
        if(at>=0&&bit(h->admitted,unsigned(at))&&bit(h->processed,unsigned(at)))h->self_initial=0;
    }
}
extern "C" bool mesh_status_fresh(uint64_t now,uint64_t then,bool valid){
    return valid&&now>=then&&now-then<MESH_STATUS_FRESH_MS;
}
extern "C" bool mesh_status_decode(const uint8_t *b,size_t n,bool from_root,uint64_t sender,MeshStatusPage *p){
    if(!b||!p||n<32||(b[0]!=1&&b[0]!=2)||(b[1]&0xc0)||get(b+18,2)||get(b+28,4))return false;
    p->flags=b[1];p->page=b[2];p->pages=b[3];p->generation=get(b+4,8);
    p->count=uint16_t(get(b+12,2));p->page_ids=uint16_t(get(b+14,2));
    p->current_count=b[17];p->root=get(b+20,8);
    if(!p->generation||!p->root||p->count>256||p->pages!=(p->count?((p->count+99)/100):1)||
       p->page>=p->pages||p->page_ids!=(p->count>100*p->page?
           (p->count-100*p->page>100?100:p->count-100*p->page):0)||
       b[16]!=p->current_count||p->current_count>9||
       (from_root?(p->root!=sender||p->current_count==0||!(p->flags&MS_CURRENT)):
                   (p->current_count!=0||(p->flags&MS_CURRENT)))||
       n!=32+8u*p->current_count+(b[0]==2?9u:8u)*p->page_ids)return false;
    p->stride=b[0]==2?9:8;
    p->current=b+32;p->ids=p->current+8*p->current_count;
    bool self=false;
    for(unsigned i=0;i<p->current_count;++i){
        uint64_t id=get(p->current+8*i,8);
        if(!id||(i&&id<=get(p->current+8*(i-1),8)))return false;
        if(id==sender)self=true;
    }
    if(from_root&&!self)return false;
    for(unsigned i=0;i<p->page_ids;++i){
        uint64_t id=get(p->ids+p->stride*i,8);if(!id||(p->stride==9&&p->ids[p->stride*i+8]>1))return false;
        for(unsigned j=0;j<i;++j)if(id==get(p->ids+p->stride*j,8))return false;
    }
    return true;
}
extern "C" uint16_t mesh_status_encode(const MeshHistory *h,uint8_t flags,uint8_t page,
    uint64_t root,const uint64_t *current,uint8_t count,uint8_t *b){
    unsigned pages=h->count?(h->count+99)/100:1;
    if(page>=pages||count>9||!root)return 0;
    unsigned start=100*page,ids=h->count>start?h->count-start:0;if(ids>100)ids=100;
    std::memset(b,0,32);b[0]=2;b[1]=flags;b[2]=page;b[3]=uint8_t(pages);
    put(b+4,h->generation,8);put(b+12,h->count,2);put(b+14,ids,2);
    b[16]=b[17]=count;put(b+20,root,8);
    for(unsigned i=0;i<count;++i)put(b+32+8*i,current[i],8);
    for(unsigned i=0;i<ids;++i){put(b+32+8*count+9*i,h->ids[start+i],8);b[32+8*count+9*i+8]=(!h->dirty&&!h->error&&bit(h->admitted,start+i))?1:0;}
    return uint16_t(32+8*count+9*ids);
}
extern "C" bool mesh_status_merge(MeshHistory *h,MeshStatusPeer *m,const MeshStatusPage *p,uint64_t now){
    if(m->generation&&p->generation<m->generation)return false;
    if(m->status_valid&&m->generation==p->generation&&m->count!=p->count)return false;
    if(m->generation!=p->generation){m->mask=0;m->generation=p->generation;}
    m->count=p->count;m->pages=p->pages;m->mask|=uint8_t(1u<<p->page);
    m->flags=p->flags;m->status_ms=now;m->status_valid=true;
    if((p->flags&MS_SATURATED)&&!h->saturated){h->saturated=true;h->dirty=true;++h->generation;}
    for(unsigned i=0;i<p->page_ids;++i){
        uint64_t id=get(p->ids+p->stride*i,8);mesh_history_add(h,id);int at=index(h,id);
        if(at>=0&&p->stride==9&&(p->flags&MS_CURRENT)&&p->ids[p->stride*i+8]&&!bit(h->admitted,unsigned(at))){
            setbit(h->admitted,unsigned(at));
            if((!bit(h->baseline,unsigned(at))&&m->baseline_complete)||id==h->self_initial)clearbit(h->processed,unsigned(at));
            h->dirty=true;++h->generation;
        }
        if(at>=0&&!m->baseline_complete&&id!=h->self_initial)setbit(h->baseline,unsigned(at));
    }
    if(m->mask==uint8_t((1u<<m->pages)-1))m->baseline_complete=true;
    return true;
}
extern "C" bool mesh_status_complete(const MeshStatusPeer *p,uint64_t now){
    return mesh_status_fresh(now,p->status_ms,p->status_valid)&&p->pages&&
        p->mask==uint8_t((1u<<p->pages)-1);
}

extern "C" bool mesh_history_admit(MeshHistory *h,uint64_t id){
    if(!h||h->error||h->saturated||h->count==256)return false;
    int at=index(h,id);if(at<0){if(!mesh_history_add(h,id))return false;at=index(h,id);}
    if(bit(h->admitted,unsigned(at)))return false;
    setbit(h->admitted,unsigned(at));clearbit(h->processed,unsigned(at));h->dirty=true;++h->generation;return true;
}
extern "C" bool mesh_history_unprocessed(const MeshHistory *h,uint64_t *id){
    if(!h||!id||h->dirty||h->error||h->saturated)return false;
    for(unsigned i=0;i<h->count;++i)if(bit(h->admitted,i)&&!bit(h->processed,i)){*id=h->ids[i];return true;}
    return false;
}
extern "C" bool mesh_history_processed(MeshHistory *h,uint64_t id){
    int at=h?index(h,id):-1;if(at<0||h->error)return false;
    setbit(h->processed,unsigned(at));h->dirty=true;++h->generation;return true;
}
