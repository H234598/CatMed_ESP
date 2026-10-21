#include "katmed_names.h"
#include <cstring>
#include <cstdio>
#include <limits>
#include <new>
#ifdef KATMED_MESH_HOST
#include <zlib.h>
extern "C" {
    struct evp_md_ctx_st;
    struct evp_md_st;
    evp_md_ctx_st *EVP_MD_CTX_new(void);
    void EVP_MD_CTX_free(evp_md_ctx_st *);
    const evp_md_st *EVP_sha256(void);
    int EVP_DigestInit_ex(evp_md_ctx_st *,const evp_md_st *,void *);
    int EVP_DigestUpdate(evp_md_ctx_st *,const void *,size_t);
    int EVP_DigestFinal_ex(evp_md_ctx_st *,unsigned char *,unsigned *);
}
#else
#include <esp_rom_crc.h>
#include <psa/crypto.h>
#endif
namespace {
    uint64_t get(const uint8_t *p,unsigned n) { uint64_t v=0;
        for(unsigned i=0;i<n;++i)v|=uint64_t(p[i])<<(8*i);
        return v;
    }
    void put(uint8_t *p,uint64_t v,unsigned n) { for(unsigned i=0;i<n;++i)p[i]=uint8_t(v>>(8*i));
    }
    bool zero(const uint8_t *p,size_t n){uint8_t v=0;
        while(n--)v|=*p++;
        return v==0;
    }
    uint32_t crc(const uint8_t *p,size_t n){
#ifdef KATMED_MESH_HOST
        return uint32_t(crc32(0,p,n));
#else
        return esp_rom_crc32_le(0,p,n);
#endif
    }
    void seal(uint8_t *p,size_t n){put(p+n-4,crc(p,n-4),4);
    }
    bool sealed(const uint8_t *p,size_t n){return get(p+n-4,4)==crc(p,n-4);
    }
    struct Hash {
#ifdef KATMED_MESH_HOST
        evp_md_ctx_st *ctx=EVP_MD_CTX_new();
        bool good=ctx && EVP_DigestInit_ex(ctx,EVP_sha256(),nullptr)==1;
        ~Hash(){EVP_MD_CTX_free(ctx);
        }
        bool add(const uint8_t *p,size_t n){return good=good && EVP_DigestUpdate(ctx,p,n)==1;
        }
        bool finish(uint8_t *p){unsigned n=0;
            return good && EVP_DigestFinal_ex(ctx,p,&n)==1 && n==32;
        }
#else
        psa_hash_operation_t ctx=PSA_HASH_OPERATION_INIT;
        bool good=psa_crypto_init()==PSA_SUCCESS && psa_hash_setup(&ctx,PSA_ALG_SHA_256)==PSA_SUCCESS;
        ~Hash(){psa_hash_abort(&ctx);
        }
        bool add(const uint8_t *p,size_t n){return good=good && psa_hash_update(&ctx,p,n)==PSA_SUCCESS;
        }
        bool finish(uint8_t *p){size_t n=0;
            return good && psa_hash_finish(&ctx,p,32,&n)==PSA_SUCCESS && n==32;
        }
#endif
    };
    enum { PB=1,TB=2,PV=4,TV=8,TC=16,REPLACE=64 };
    unsigned bank(const NamesStore *s,bool t){return (s->ctl[51]&(t?TB:PB))?1:0;
    }
    uint8_t *desc(NamesStore *s,bool t){return s->ctl+(t?192:112);
    }
    const uint8_t *desc(const NamesStore *s,bool t){return s->ctl+(t?192:112);
    }
    bool members_valid(const uint64_t *ids,size_t n,uint64_t controller){
        if(n>8||(!ids&&n)||!controller)return false;
        for(size_t i=0;i<n;++i){if(!ids[i]||ids[i]==controller)return false;for(size_t j=0;j<i;++j)if(ids[i]==ids[j])return false;}
        return true;
    }
    void observe_serial(NamesStore *s,const uint8_t *d){
        uint64_t highest=get(d+8,8);if(get(d+24,8)>highest)highest=get(d+24,8);
        const uint64_t next=highest==UINT64_MAX?highest:highest+1;
        if(next>get(s->ctl+40,8))put(s->ctl+40,next,8);
    }
    NamesResult result(StoreResult r){switch(r){case STORE_OK:return NAMES_OK;
            case STORE_FULL:return NAMES_FULL;
            case STORE_COMMIT_UNKNOWN:return NAMES_COMMIT_UNKNOWN;
            case STORE_CORRUPT:return NAMES_CORRUPT;
            case STORE_NOT_FOUND:return NAMES_NEED_SYNC;
            default:return NAMES_IO;
    }}
    NamesResult fail(NamesStore *s,NamesResult r){if(r!=NAMES_OK){s->poisoned=true;
            s->error=r;
        }return r;
    }
    NamesResult save(NamesStore *s){seal(s->ctl,480);
        return fail(s,result(s->backend.commit(s->backend.context,NAMES_CTL,s->ctl,480)));
    }
    NamesResult guard(NamesStore *s){return !s||!s->opened?NAMES_IO:s->poisoned?s->error:NAMES_OK;
    }
    void header(const NamesStore *s,const uint8_t *d,uint8_t *h){std::memset(h,0,64);
        std::memcpy(h,"KMN2",4);
        put(h+4,2,2);
        put(h+6,64,2);
        std::memcpy(h+8,s->ctl+24,16);
        std::memcpy(h+24,s->ctl+8,16);
        std::memcpy(h+40,d+16,16);
        put(h+56,8,2);
        seal(h,64);
    }
    NamesResult digest(NamesStore *s,unsigned b,const uint8_t *d,uint32_t slots,uint8_t out[32]){
        if(slots>65535)return NAMES_CORRUPT;
        uint8_t bytes[512],expected[64];
        header(s,d,expected);
        StoreResult r=s->backend.read_file(s->backend.context,b,0,bytes,64);
        if(r!=STORE_OK)return result(r);
        if(std::memcmp(bytes,expected,64)!=0)return NAMES_CORRUPT;
        Hash hash;
        if(!hash.add(bytes,64))return NAMES_IO;
        uint32_t offset=64,end=64+8*slots;
        while(offset<end){size_t n=end-offset;
            if(n>sizeof(bytes))n=sizeof(bytes);
            r=s->backend.read_file(s->backend.context,b,offset,bytes,n);
            if(r!=STORE_OK)return result(r);
            if(!hash.add(bytes,n))return NAMES_IO;
            offset+=uint32_t(n);
        }
        return hash.finish(out)?NAMES_OK:NAMES_IO;
    }
    // Plan the immutable target digest without modifying a referenced bank.
    NamesResult planned_digest(NamesStore *s,uint8_t *d,uint64_t appended) {
        Hash hash;
        uint8_t bytes[512];
        header(s,d,bytes);
        if(!hash.add(bytes,64)) return NAMES_IO;
        const uint32_t slots=(s->ctl[51]&PV)?uint32_t(get(desc(s,false)+64,4)):0;
        for(uint32_t offset=64;offset<64+8*slots;) {
            size_t n=64+8*slots-offset;
            if(n>sizeof(bytes)) n=sizeof(bytes);
            StoreResult r=s->backend.read_file(s->backend.context,bank(s,false),offset,bytes,n);
            if(r!=STORE_OK) return result(r);
            if(!hash.add(bytes,n)) return NAMES_IO;
            offset+=uint32_t(n);
        }
        if(appended) { put(bytes,appended,8); if(!hash.add(bytes,8)) return NAMES_IO; }
        return hash.finish(d+32)?NAMES_OK:NAMES_IO;
    }
    NamesResult slot(NamesStore *s,unsigned b,uint32_t index,uint64_t *id){if(!index||index>65535)return NAMES_CORRUPT;
        uint8_t bytes[8];
        StoreResult r=s->backend.read_file(s->backend.context,b,64+8*(index-1),bytes,8);
        if(r!=STORE_OK)return result(r);
        *id=get(bytes,8);
        return NAMES_OK;
    }
    NamesResult validate(NamesStore *s,bool t){const uint8_t *d=desc(s,t);
        if(!names_descriptor_valid(d))return NAMES_CORRUPT;
        uint8_t hash[32];
        NamesResult r=digest(s,bank(s,t),d,uint32_t(get(d+64,4)),hash);
        if(r!=NAMES_OK)return r;
        return std::memcmp(hash,d+32,32)==0?NAMES_OK:NAMES_CORRUPT;
    }
    NamesResult preserve(NamesStore *s){
        const uint32_t newslots=uint32_t(get(desc(s,true)+64,4));
        if(s->ctl[51]&PV){
            const uint32_t oldslots=uint32_t(get(desc(s,false)+64,4));
            if(newslots<oldslots)return NAMES_CONFLICT;
            for(uint32_t i=1;i<=oldslots;++i){
                uint64_t a=0,b=0;NamesResult r=slot(s,bank(s,false),i,&a);
                if(r!=NAMES_OK)return r;
                if(a){r=slot(s,bank(s,true),i,&b);if(r!=NAMES_OK)return r;if(a!=b)return NAMES_CONFLICT;}
            }
        }
        const uint64_t reserve=get(s->ctl+96,8);const uint16_t index=uint16_t(get(s->ctl+104,2));
        if(reserve){uint64_t id=0;if(index>newslots)return NAMES_CONFLICT;NamesResult r=slot(s,bank(s,true),index,&id);if(r!=NAMES_OK)return r;if(id!=reserve)return NAMES_CONFLICT;}
        return NAMES_OK;
    }
    NamesResult load_view(NamesStore *s){uint8_t v[48];
        size_t n=sizeof(v);
        StoreResult r=s->backend.read(s->backend.context,NAMES_VIEW,v,&n);
        s->visible_index=0;
        if(r==STORE_NOT_FOUND)return NAMES_OK;
        if(r!=STORE_OK)return result(r);
        if(n!=48||std::memcmp(v,"KNV2",4)||get(v+4,2)!=2||get(v+6,2)!=48||!sealed(v,48)||std::memcmp(v+8,s->ctl+24,16)||get(v+42,2)!=1||!get(v+24,8)||!get(v+32,8)||!get(v+40,2)||!(s->ctl[51]&PV))return NAMES_CORRUPT;
        const uint16_t index=uint16_t(get(v+40,2));
        if(index>get(desc(s,false)+64,4))return NAMES_CORRUPT;
        uint64_t id=0;
        NamesResult nr=slot(s,bank(s,false),index,&id);
        if(nr!=NAMES_OK)return nr;
        if(id!=s->local_device)return NAMES_CORRUPT;
        s->visible_index=index;
        return NAMES_OK;
    }
    NamesResult progress(NamesStore *s,uint32_t slots){uint8_t hash[32];
        NamesResult r=digest(s,bank(s,true),desc(s,true),slots,hash);
        if(r!=NAMES_OK)return fail(s,r);
        put(s->ctl+52,slots,4);
        std::memcpy(s->ctl+56,hash,32);
        return save(s);
    }
    NamesResult prepared(NamesStore *s){NamesResult r=validate(s,true);
        if(r!=NAMES_OK)return fail(s,r);
        r=preserve(s);
        if(r!=NAMES_OK)return fail(s,r);
        s->ctl[48]=NAMES_PREPARED;
        s->ctl[51]|=TC;
        return save(s);
    }
    NamesResult start_file(NamesStore *s){
        if((s->ctl[51]&PV)&&bank(s,true)==bank(s,false))return fail(s,NAMES_CORRUPT);
        uint8_t h[64];
        header(s,desc(s,true),h);
        StoreResult r=s->backend.truncate(s->backend.context,bank(s,true),0);
        if(r==STORE_OK)r=s->backend.write_file(s->backend.context,bank(s,true),0,h,64);
        if(r==STORE_OK)r=s->backend.sync(s->backend.context,bank(s,true));
        return fail(s,result(r));
    }
}
extern "C" bool names_descriptor_valid(const uint8_t d[80]){return d && !zero(d+32,32) && get(d,8)&&get(d+8,8)&&get(d+16,8)&&get(d+24,8)&&get(d+64,4)<=65535&&get(d+68,4)==get(d+64,4)+1&&get(d+72,4)<=get(d+64,4)&&get(d+76,2)==1&&get(d+78,2)==0;
}
extern "C" void names_descriptor_wire(uint8_t out[80],const uint8_t in[80],bool encode){
    std::memcpy(out,in,80);
    const unsigned offsets[]={0,8,16,24,64,68,72,76,78};
    const unsigned widths[]={8,8,8,8,4,4,4,2,2};
    for(unsigned f=0;f<9;++f){unsigned o=offsets[f],n=widths[f];
        uint64_t v=0;
        if(encode){v=get(in+o,n);
            for(unsigned i=0;i<n;++i)out[o+i]=uint8_t(v>>(8*(n-1-i)));
        }else{for(unsigned i=0;i<n;++i)v=(v<<8)|in[o+i];
            put(out+o,v,n);
    }}
}
extern "C" NamesResult names_open(NamesStore *s,NamesBackend backend,uint64_t device,const uint8_t group[16]){
    if(!s||!device||!group||!backend.read||!backend.commit||!backend.size||!backend.read_file||!backend.write_file||!backend.truncate||!backend.sync)return NAMES_IO;
    std::memset(s,0,sizeof(*s));
    s->backend=backend;
    std::memcpy(s->expected_group,group,16);
    s->local_device=device;
    s->opened=true;
    size_t n=480;
    StoreResult r=backend.read(backend.context,NAMES_CTL,s->ctl,&n);
    if(r==STORE_NOT_FOUND){uint8_t v[48];
        size_t vn=48;
        if(backend.read(backend.context,NAMES_VIEW,v,&vn)!=STORE_NOT_FOUND)return fail(s,NAMES_CORRUPT);
        for(unsigned b=0;b<2;++b){uint32_t length=0;
            if(backend.size(backend.context,b,&length)!=STORE_NOT_FOUND)return fail(s,NAMES_CORRUPT);
        }std::memset(s->ctl,0,480);
        std::memcpy(s->ctl,"KNC2",4);
        put(s->ctl+4,2,2);
        put(s->ctl+6,480,2);
        std::memcpy(s->ctl+8,group,16);
        put(s->ctl+40,1,8);
        put(s->ctl+344,device,8);
        s->fresh=true;
        return NAMES_OK;
    }
    if(r!=STORE_OK)return fail(s,result(r));
    if(n!=480||std::memcmp(s->ctl,"KNC2",4)||get(s->ctl+4,2)!=2||get(s->ctl+6,2)!=480||!sealed(s->ctl,480)||std::memcmp(s->ctl+8,group,16)||zero(s->ctl+24,16)||get(s->ctl+344,8)!=device||!get(s->ctl+40,8)||s->ctl[48]>5||s->ctl[49]>4||s->ctl[50]>8||(s->ctl[51]&128)||!zero(s->ctl+108,4)||!zero(s->ctl+440,36))return fail(s,NAMES_CORRUPT);
    const unsigned flags=s->ctl[51],phase=s->ctl[48],intent=s->ctl[49];
    if((!(flags&PV)&&!zero(desc(s,false),80))||(!(flags&TV)&&!zero(desc(s,true),80))||
       (bool(get(s->ctl+96,8))!=bool(get(s->ctl+104,2)))||
       (phase==NAMES_IDLE&&(!(flags&PV)||(flags&~(PB|PV))||intent||get(s->ctl+96,8)||!zero(s->ctl+352,88)))||
       (phase!=NAMES_IDLE&&!(flags&TV))||
       ((flags&TV)&&get(s->ctl+104,2)>get(desc(s,true)+64,4))||
       (phase!=NAMES_IDLE&&bool(flags&TC)!=(phase>=NAMES_PREPARED))||
       (phase==NAMES_RX_APPEND&&(intent!=2||!(flags&PV)||bank(s,false)!=bank(s,true)))||
       (phase==NAMES_RX_REPLACE&&(intent!=3||((flags&PV)&&bank(s,false)==bank(s,true))))||
       (phase==NAMES_LOCAL_PREPARE&&intent!=0&&intent!=1&&intent!=4)||
       (phase==NAMES_FINALIZING&&(!(flags&PV)||bank(s,false)!=bank(s,true)||std::memcmp(desc(s,false),desc(s,true),80)))||
       (intent<2&&(!zero(s->ctl+352,88)))||
       (intent>=2&&(!names_descriptor_valid(s->ctl+352)||!get(s->ctl+432,8))))return fail(s,NAMES_CORRUPT);
    for(unsigned i=0;i<8;++i){const uint64_t id=get(s->ctl+272+8*i,8);
        if((i<s->ctl[50])!=bool(id))return fail(s,NAMES_CORRUPT);
        for(unsigned j=0;j<i;++j)if(id && id==get(s->ctl+272+8*j,8))return fail(s,NAMES_CORRUPT);
    }
    if(s->ctl[51]&PV){NamesResult nr=validate(s,false);
        if(nr!=NAMES_OK)return fail(s,nr);
    }
    NamesResult view_result=load_view(s);
    if(view_result!=NAMES_OK)return fail(s,view_result);
    if(s->ctl[51]&TV){if(!names_descriptor_valid(desc(s,true)))return fail(s,NAMES_CORRUPT);
        if(s->ctl[51]&TC){NamesResult nr=validate(s,true);
            if(nr!=NAMES_OK)return fail(s,nr);
        }else if(s->ctl[48]!=NAMES_LOCAL_PREPARE){uint32_t slots=uint32_t(get(s->ctl+52,4));
            if(slots>get(desc(s,true)+64,4))return fail(s,NAMES_CORRUPT);
            uint8_t hash[32];
            NamesResult nr=NAMES_OK;
            if(!(slots==0 && zero(s->ctl+56,32) && (!(s->ctl[51]&PV)||bank(s,true)!=bank(s,false))))nr=digest(s,bank(s,true),desc(s,true),slots,hash);
            else std::memset(hash,0,32);
            if(nr!=NAMES_OK||std::memcmp(hash,s->ctl+56,32))return fail(s,NAMES_CORRUPT);
    }}
    return NAMES_OK;
}
extern "C" NamesResult names_initialize(NamesStore *s,const uint8_t registry[16]){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!s->fresh||!registry||zero(registry,16))return NAMES_STALE;
    std::memcpy(s->ctl+24,registry,16);
    s->ctl[48]=NAMES_LOCAL_PREPARE;
    s->ctl[51]=TV;
    put(s->ctl+336,s->local_device,8);
    uint8_t *d=desc(s,true);
    put(d,s->local_device,8);
    put(d+8,1,8);
    put(d+16,s->local_device,8);
    put(d+24,1,8);
    put(d+68,1,4);
    put(d+76,1,2);
    put(s->ctl+40,2,8);
    NamesResult planned=planned_digest(s,d,0);
    if(planned!=NAMES_OK)return fail(s,planned);
    NamesResult r=save(s);
    if(r!=NAMES_OK)return r;
    s->fresh=false;
    return names_resume(s);
}
extern "C" NamesResult names_lookup(NamesStore *s,uint64_t device,uint16_t *confirmed,uint16_t *reserved){
    if(confirmed)*confirmed=0;
    if(reserved)*reserved=0;
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!device)return NAMES_CORRUPT;
    if(s->ctl[51]&PV)for(uint32_t i=1;i<=get(desc(s,false)+64,4);++i){uint64_t id=0;
        NamesResult r=slot(s,bank(s,false),i,&id);
        if(r!=NAMES_OK)return fail(s,r);
        if(id==device){if(confirmed)*confirmed=uint16_t(i);
            return NAMES_OK;
    }}
    if(get(s->ctl+96,8)==device){if(reserved)*reserved=uint16_t(get(s->ctl+104,2));return NAMES_OK;}
    if(s->ctl[51]&TV){
        const uint32_t slots=(s->ctl[51]&TC)?uint32_t(get(desc(s,true)+64,4)):uint32_t(get(s->ctl+52,4));
        for(uint32_t i=1;i<=slots;++i){uint64_t id=0;NamesResult r=slot(s,bank(s,true),i,&id);if(r!=NAMES_OK)return fail(s,r);if(id==device){if(reserved)*reserved=uint16_t(i);return NAMES_OK;}}
    }
    return NAMES_OK;
}
extern "C" NamesResult names_set_barrier(NamesStore *s,const uint64_t *peers,size_t n,uint64_t controller){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(n>8||(!peers&&n)||!controller)return NAMES_CORRUPT;
    for(size_t i=0;i<n;++i){if(!peers[i]||peers[i]==controller)return NAMES_CORRUPT;
        for(size_t j=0;j<i;++j)if(peers[i]==peers[j])return NAMES_CORRUPT;
    }
    uint64_t epoch=get(s->ctl+88,8);
    if(epoch==UINT64_MAX)return fail(s,NAMES_INDEX_EXHAUSTED);
    put(s->ctl+88,epoch+1,8);
    put(s->ctl+336,controller,8);
    s->ctl[50]=uint8_t(n);
    std::memset(s->ctl+272,0,64);
    for(size_t i=0;i<n;++i)put(s->ctl+272+8*i,peers[i],8);
    return save(s);
}
extern "C" NamesResult names_reserve(NamesStore *s,uint64_t device,const uint64_t *peers,size_t n){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!members_valid(peers,n,s->local_device))return NAMES_CORRUPT;
    uint16_t known=0,pending=0;
    NamesResult r=names_lookup(s,device,&known,&pending);
    if(r!=NAMES_OK)return r;
    if(known)return NAMES_OK;
    if(pending)return NAMES_BUSY;
    if(s->ctl[48]!=NAMES_IDLE||!(s->ctl[51]&PV))return NAMES_BUSY;
    uint32_t cursor=names_cursor(s);
    if(cursor>65535)return NAMES_INDEX_EXHAUSTED;
    uint64_t serial=get(s->ctl+40,8);
    if(serial==UINT64_MAX)return NAMES_INDEX_EXHAUSTED;
    std::memcpy(desc(s,true),desc(s,false),80);
    uint8_t *d=desc(s,true);
    put(d,s->local_device,8);
    put(d+8,serial,8);
    put(d+64,cursor,4);
    put(d+68,cursor+1,4);
    put(d+72,get(d+72,4)+1,4);
    std::memset(d+32,0,32);
    put(s->ctl+40,serial+1,8);
    put(s->ctl+96,device,8);
    put(s->ctl+104,cursor,2);
    s->ctl[48]=NAMES_LOCAL_PREPARE;
    s->ctl[49]=1;
    s->ctl[51]=uint8_t((s->ctl[51]&PB)|PV|TV|((s->ctl[51]&PB)?TB:0));
    r=planned_digest(s,d,device);
    if(r!=NAMES_OK)return fail(s,r);
    /* The same atomic control commit binds reservation, serial, T and E before
    * any append. set_barrier saves all those fields together. */
    r=names_set_barrier(s,peers,n,s->local_device);
    if(r!=NAMES_OK)return r;
    return names_resume(s);
}
extern "C" NamesResult names_resume(NamesStore *s){
    if(guard(s)!=NAMES_OK)return guard(s);
    if((s->ctl[48]==NAMES_RX_REPLACE) && get(s->ctl+52,4)==0 && zero(s->ctl+56,32)) {
        if((s->ctl[51]&PV)&&bank(s,true)==bank(s,false))return fail(s,NAMES_CORRUPT);
        NamesResult r=start_file(s);
        return r==NAMES_OK?progress(s,0):r;
    }
    if((s->ctl[48]==NAMES_RX_APPEND||s->ctl[48]==NAMES_RX_REPLACE)&&
       get(s->ctl+52,4)==get(desc(s,true)+64,4)&&!zero(s->ctl+56,32))return prepared(s);
    if(s->ctl[48]!=NAMES_LOCAL_PREPARE)return NAMES_STALE;
    if(s->ctl[49]==4)return NAMES_NEED_SYNC;
    NamesResult r=NAMES_OK;
    if(!(s->ctl[51]&PV)){r=start_file(s);
        if(r!=NAMES_OK)return r;
    }
    else {uint8_t id[8];
        put(id,get(s->ctl+96,8),8);
        StoreResult sr=s->backend.write_file(s->backend.context,bank(s,true),64+8*(uint32_t(get(s->ctl+104,2))-1),id,8);
        if(sr==STORE_OK)sr=s->backend.sync(s->backend.context,bank(s,true));
        if(sr!=STORE_OK)return fail(s,result(sr));
    }
    uint8_t hash[32];
    r=digest(s,bank(s,true),desc(s,true),uint32_t(get(desc(s,true)+64,4)),hash);
    if(r!=NAMES_OK)return fail(s,r);
    if(std::memcmp(desc(s,true)+32,hash,32))return fail(s,NAMES_CORRUPT);
    put(s->ctl+52,get(desc(s,true)+64,4),4);
    std::memcpy(s->ctl+56,hash,32);
    return prepared(s);
}
extern "C" NamesResult names_publish(NamesStore *s){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(s->ctl[48]==NAMES_FINALIZING)return NAMES_OK;
    if(s->ctl[48]!=NAMES_PREPARED)return NAMES_STALE;
    NamesResult r=validate(s,true);
    if(r!=NAMES_OK)return fail(s,r);
    r=preserve(s);
    if(r!=NAMES_OK)return fail(s,r);
    std::memcpy(desc(s,false),desc(s,true),80);
    s->ctl[51]=uint8_t((s->ctl[51]&~PB)|PV|((s->ctl[51]&TB)?PB:0));
    s->ctl[48]=NAMES_FINALIZING;
    return save(s);
}
extern "C" NamesResult names_confirm(NamesStore *s,const uint8_t d[80]){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!(s->ctl[51]&PV)||std::memcmp(desc(s,false),d,80))return NAMES_STALE;
    uint16_t index=0,pending=0;
    NamesResult r=names_lookup(s,s->local_device,&index,&pending);
    if(r!=NAMES_OK)return r;
    if(!index)return NAMES_NEED_SYNC;
    if(s->visible_index==index)return NAMES_OK;
    uint8_t v[48]{};
    std::memcpy(v,"KNV2",4);
    put(v+4,2,2);
    put(v+6,48,2);
    std::memcpy(v+8,s->ctl+24,16);
    std::memcpy(v+24,d,16);
    put(v+40,index,2);
    put(v+42,1,2);
    seal(v,48);
    r=result(s->backend.commit(s->backend.context,NAMES_VIEW,v,48));
    if(r!=NAMES_OK)return fail(s,r);
    s->visible_index=index;
    return NAMES_OK;
}
extern "C" NamesResult names_finish(NamesStore *s){if(guard(s)!=NAMES_OK)return guard(s);
    if(s->ctl[48]==NAMES_IDLE)return NAMES_OK;
    if(s->ctl[48]!=NAMES_FINALIZING)return NAMES_STALE;
    s->ctl[48]=NAMES_IDLE;
    s->ctl[49]=0;
    s->ctl[50]=0;
    s->ctl[51]&=uint8_t(PB|PV);
    std::memset(s->ctl+52,0,36);
    std::memset(s->ctl+96,0,12);
    std::memset(s->ctl+192,0,144);
    std::memset(s->ctl+352,0,88);
    NamesResult saved=save(s);if(saved==NAMES_OK&&s->error!=NAMES_CONFLICT)s->error=NAMES_OK;
    return saved;
}
extern "C" NamesResult names_read_page(NamesStore *s,bool target,uint32_t start,uint64_t *ids,size_t *n){if(guard(s)!=NAMES_OK)return guard(s);
    if(!ids||!n||*n>89||!(s->ctl[51]&(target?TV:PV)))return NAMES_STALE;
    uint32_t slots=uint32_t(get(desc(s,target)+64,4));
    if(start>slots)return NAMES_STALE;
    if(*n>slots-start)*n=slots-start;
    for(size_t i=0;i<*n;++i){NamesResult r=slot(s,bank(s,target),start+uint32_t(i)+1,ids+i);
        if(r!=NAMES_OK)return fail(s,r);
    }return NAMES_OK;
}
extern "C" NamesResult names_receive_begin(NamesStore *s,const uint8_t registry[16],const uint8_t target[80],bool append,uint64_t controller,uint64_t epoch,const uint64_t *peers,size_t count,uint64_t reserve,uint16_t index){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!names_descriptor_valid(target)||!members_valid(peers,count,controller)||!epoch||zero(registry,16)||bool(reserve)!=bool(index)||(reserve&&index>get(target+64,4)))return NAMES_CORRUPT;
    if(!s->fresh&&std::memcmp(registry,s->ctl+24,16))return NAMES_CONFLICT;
    if(s->ctl[51]&PV && !std::memcmp(target,desc(s,false),80))return NAMES_OK;
    if(s->ctl[51]&TV) {
        if(std::memcmp(target,desc(s,true),80))return NAMES_BUSY;
        if(reserve&&get(s->ctl+96,8)&&
           (reserve!=get(s->ctl+96,8)||index!=get(s->ctl+104,2)))return NAMES_CONFLICT;
        if(reserve&&!get(s->ctl+96,8)){put(s->ctl+96,reserve,8);put(s->ctl+104,index,2);}
        if(s->ctl[48]==NAMES_LOCAL_PREPARE){
            // A new current coordinator supplies the exact immutable result,
            // rather than silently changing a still-running root merge source.
            const bool append=(s->ctl[51]&PV)&&bank(s,false)==bank(s,true);
            s->ctl[48]=append?NAMES_RX_APPEND:NAMES_RX_REPLACE;s->ctl[49]=append?2:3;
            std::memcpy(s->ctl+352,target,80);put(s->ctl+432,controller,8);
            if(append){put(s->ctl+52,get(desc(s,false)+64,4),4);std::memcpy(s->ctl+56,desc(s,false)+32,32);}
        }
        put(s->ctl+88,epoch,8);put(s->ctl+336,controller,8);
        s->ctl[50]=uint8_t(count);std::memset(s->ctl+272,0,64);
        for(size_t i=0;i<count;++i)put(s->ctl+272+8*i,peers[i],8);
        NamesResult saved=save(s);
        if(saved!=NAMES_OK)return saved;
        if((s->ctl[48]==NAMES_RX_APPEND||s->ctl[48]==NAMES_RX_REPLACE)&&
           (zero(s->ctl+56,32)||get(s->ctl+52,4)==get(target+64,4)))return names_resume(s);
        return NAMES_OK;
    }
    if(s->ctl[48]!=NAMES_IDLE)return NAMES_BUSY;
    if(s->fresh)std::memcpy(s->ctl+24,registry,16);
    observe_serial(s,target);
    const bool share=append&&(s->ctl[51]&PV)&&!std::memcmp(target+16,desc(s,false)+16,16)&&get(target+64,4)>=get(desc(s,false)+64,4);
    if(append&&!share)return NAMES_STALE;
    const unsigned candidate=share?bank(s,false):((s->ctl[51]&PV)?1-bank(s,false):0);
    std::memcpy(desc(s,true),target,80);
    s->ctl[48]=share?NAMES_RX_APPEND:NAMES_RX_REPLACE;
    s->ctl[49]=share?2:3;
    s->ctl[50]=uint8_t(count);
    s->ctl[51]=uint8_t((s->ctl[51]&(PB|PV))|TV|(candidate?TB:0)|(share?0:REPLACE));
    put(s->ctl+88,epoch,8);
    put(s->ctl+336,controller,8);
    put(s->ctl+96,reserve,8);
    put(s->ctl+104,index,2);
    std::memset(s->ctl+272,0,64);
    for(size_t i=0;i<count;++i)put(s->ctl+272+8*i,peers[i],8);
    std::memcpy(s->ctl+352,target,80);
    put(s->ctl+432,controller,8);
    put(s->ctl+52,share?get(desc(s,false)+64,4):0,4);
    if(share)std::memcpy(s->ctl+56,desc(s,false)+32,32);
    else std::memset(s->ctl+56,0,32);
    /* Durable binding before first write, including first-ever registry. */
    NamesResult r=save(s);
    if(r!=NAMES_OK)return r;
    s->fresh=false;
    if(!share){r=start_file(s);
        if(r!=NAMES_OK)return r;
    }
    r=progress(s,share?uint32_t(get(desc(s,false)+64,4)):0);
    if(r!=NAMES_OK)return r;
    return get(s->ctl+52,4)==get(target+64,4)?prepared(s):NAMES_OK;
}
extern "C" NamesResult names_receive_page(NamesStore *s,uint32_t start,const uint64_t *ids,size_t n){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!ids||!n||n>89||(s->ctl[48]!=NAMES_RX_APPEND&&s->ctl[48]!=NAMES_RX_REPLACE&&s->ctl[48]!=NAMES_PREPARED))return NAMES_STALE;
    const uint32_t at=uint32_t(get(s->ctl+52,4)),slots=uint32_t(get(desc(s,true)+64,4));
    if(start>slots||n>slots-start)return NAMES_STALE;
    if(start<at){if(n>at-start)return NAMES_STALE;
        for(size_t i=0;i<n;++i){uint64_t id=0;
            NamesResult r=slot(s,bank(s,true),start+uint32_t(i)+1,&id);
            if(r!=NAMES_OK)return fail(s,r);
            if(id!=ids[i])return NAMES_CONFLICT;
        }return NAMES_OK;
    }
    if(start!=at)return NAMES_STALE;
    uint8_t bytes[8];StoreResult sr=STORE_OK;
    for(size_t i=0;i<n;++i){
        put(bytes,ids[i],8);
        sr=s->backend.write_file(s->backend.context,bank(s,true),64+8*(start+uint32_t(i)),bytes,8);
        if(sr!=STORE_OK)return fail(s,result(sr));
    }
    if(sr==STORE_OK)sr=s->backend.sync(s->backend.context,bank(s,true));
    if(sr!=STORE_OK)return fail(s,result(sr));
    NamesResult r=progress(s,start+uint32_t(n));
    if(r!=NAMES_OK)return r;
    return start+n==slots?prepared(s):NAMES_OK;
}
extern "C" const uint8_t *names_published(const NamesStore *s){return s && (s->ctl[51]&PV)?desc(s,false):nullptr;
}
extern "C" const uint8_t *names_target(const NamesStore *s){return s && (s->ctl[51]&TV)?desc(s,true):nullptr;
}
extern "C" uint32_t names_cursor(const NamesStore *s){const uint8_t *p=names_published(s);
    return p?uint32_t(get(p+68,4)):1;
}
extern "C" NamesPhase names_phase(const NamesStore *s){return s?NamesPhase(s->ctl[48]):NAMES_IDLE;
}
extern "C" void names_visible(const NamesStore *s,char out[32],bool *assigned,bool *uncertain){
    const unsigned index=s?s->visible_index:0;
    if(out)std::snprintf(out,32,index?"KatmedESP-%03u":"KatmedESP",index);
    if(assigned)*assigned=index!=0;
    if(uncertain)*uncertain=!s||!s->opened||s->poisoned||s->error!=NAMES_OK||s->ctl[48]!=NAMES_IDLE;
}

namespace {
Hash *verify_hash(NamesRetarget *v){return reinterpret_cast<Hash *>(v->hash_state);}
static_assert(sizeof(Hash)<=sizeof(NamesRetarget::hash_state),"retarget hash context bound");
}
extern "C" void names_retarget_cancel(NamesRetarget *v){
    if(v&&v->active)verify_hash(v)->~Hash();
    if(v)std::memset(v,0,sizeof(*v));
}
extern "C" NamesResult names_retarget_begin(NamesStore *s,NamesRetarget *v,const uint8_t target[80]){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!v||!names_descriptor_valid(target)||!(s->ctl[51]&TV)||
       s->ctl[48]==NAMES_FINALIZING||get(target+68,4)<get(desc(s,true)+68,4))return NAMES_STALE;
    names_retarget_cancel(v);
    std::memcpy(v->target,target,80);new(v->hash_state)Hash();v->active=true;
    uint8_t h[64];header(s,target,h);
    if(!verify_hash(v)->add(h,64)){names_retarget_cancel(v);return NAMES_IO;}
    return NAMES_OK;
}
extern "C" NamesResult names_retarget_page(NamesStore *s,NamesRetarget *v,uint32_t start,const uint64_t *ids,size_t n){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!v||!v->active||v->complete||!ids||!n||n>89||start!=v->progress||
       start>get(v->target+64,4)||n>get(v->target+64,4)-start)return NAMES_STALE;
    const uint32_t oldprogress=(s->ctl[51]&TC)?uint32_t(get(desc(s,true)+64,4)):uint32_t(get(s->ctl+52,4));
    uint8_t bytes[8];
    for(size_t j=0;j<n;++j){
        const uint32_t at=start+uint32_t(j)+1;
        for(unsigned source=0;source<2;++source){
            const uint32_t slots=source?oldprogress:((s->ctl[51]&PV)?uint32_t(get(desc(s,false)+64,4)):0);
            if(at<=slots){uint64_t old=0;NamesResult r=slot(s,bank(s,source!=0),at,&old);if(r!=NAMES_OK)return fail(s,r);if(old&&old!=ids[j])return NAMES_CONFLICT;}
        }
        if(at==get(s->ctl+104,2)&&get(s->ctl+96,8)&&ids[j]!=get(s->ctl+96,8))return NAMES_CONFLICT;
        put(bytes,ids[j],8);if(!verify_hash(v)->add(bytes,8))return NAMES_IO;
        if(ids[j])++v->assigned;
    }
    v->progress+=uint32_t(n);
    if(v->progress==get(v->target+64,4)){
        uint8_t hash[32];if(!verify_hash(v)->finish(hash))return NAMES_IO;
        if(std::memcmp(hash,v->target+32,32)||v->assigned!=get(v->target+72,4))return NAMES_CORRUPT;
        verify_hash(v)->~Hash();v->active=false;v->complete=true;
    }
    return NAMES_OK;
}
extern "C" NamesResult names_retarget_accept(NamesStore *s,NamesRetarget *v,uint64_t controller,uint64_t epoch,const uint64_t *peers,size_t n){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!v||!v->complete||!controller||!epoch||n>8||(!peers&&n))return NAMES_STALE;
    const unsigned candidate=(s->ctl[51]&PV)?1-bank(s,false):bank(s,true);
    observe_serial(s,v->target);
    std::memcpy(desc(s,true),v->target,80);std::memcpy(s->ctl+352,v->target,80);
    put(s->ctl+432,controller,8);put(s->ctl+336,controller,8);put(s->ctl+88,epoch,8);
    s->ctl[48]=NAMES_RX_REPLACE;s->ctl[49]=3;s->ctl[50]=uint8_t(n);
    s->ctl[51]=uint8_t((s->ctl[51]&(PB|PV))|TV|REPLACE|(candidate?TB:0));
    put(s->ctl+52,0,4);std::memset(s->ctl+56,0,32);std::memset(s->ctl+272,0,64);
    for(size_t i=0;i<n;++i)put(s->ctl+272+8*i,peers[i],8);
    NamesResult r=save(s); // Only known-new intent authorizes candidate reset.
    if(r!=NAMES_OK)return r;
    std::memset(v,0,sizeof(*v));
    r=start_file(s);return r==NAMES_OK?progress(s,0):r;
}

extern "C" NamesResult names_reopen(NamesStore *s){
    if(!s||!s->opened)return NAMES_IO;
    NamesBackend backend=s->backend;uint8_t group[16];std::memcpy(group,s->expected_group,16);
    uint64_t device=s->local_device;
    if(backend.reopen){StoreResult r=backend.reopen(backend.context);if(r!=STORE_OK)return fail(s,result(r));}
    return names_open(s,backend,device,group);
}

namespace {
Hash *merge_source_hash(NamesMerge *m){return reinterpret_cast<Hash *>(m->hashes);}
Hash *merge_target_hash(NamesMerge *m){return reinterpret_cast<Hash *>(m->hashes+32);}
NamesResult merge_start_hashes(NamesStore *s,NamesMerge *m){
    new(m->hashes)Hash();new(m->hashes+32)Hash();m->active=true;
    uint8_t h[64];header(s,m->source,h);
    if(!merge_source_hash(m)->add(h,64))return NAMES_IO;
    header(s,m->target,h);return merge_target_hash(m)->add(h,64)?NAMES_OK:NAMES_IO;
}
}
extern "C" void names_merge_cancel(NamesMerge *m){
    if(m&&m->active){merge_source_hash(m)->~Hash();merge_target_hash(m)->~Hash();}
    if(m)std::memset(m,0,sizeof(*m));
}
extern "C" NamesResult names_merge_begin(NamesStore *s,NamesMerge *m,uint64_t source_device,const uint8_t source[80]){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!m||!source_device||!names_descriptor_valid(source)||!get(source+64,4)||!(s->ctl[51]&PV))return NAMES_STALE;
    if(s->ctl[48]!=NAMES_IDLE||m->active)return NAMES_BUSY;
    if(get(s->ctl+40,8)==UINT64_MAX)return NAMES_INDEX_EXHAUSTED;
    names_merge_cancel(m);m->source_device=source_device;std::memcpy(m->source,source,80);
    uint8_t *t=m->target;uint64_t serial=get(s->ctl+40,8);
    uint64_t observed=get(source+8,8);if(get(source+24,8)>observed)observed=get(source+24,8);
    if(observed==UINT64_MAX)return NAMES_INDEX_EXHAUSTED;
    if(serial<=observed)serial=observed+1;
    put(t,s->local_device,8);put(t+8,serial,8);put(t+16,s->local_device,8);put(t+24,serial,8);
    const uint32_t a=uint32_t(get(source+64,4)),b=uint32_t(get(desc(s,false)+64,4));
    put(t+64,a>b?a:b,4);put(t+68,(a>b?a:b)+1,4);put(t+76,1,2);
    return merge_start_hashes(s,m);
}
extern "C" NamesResult names_merge_resume(NamesStore *s,NamesMerge *m){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!m||s->ctl[48]!=NAMES_LOCAL_PREPARE||s->ctl[49]!=4)return NAMES_STALE;
    names_merge_cancel(m);std::memcpy(m->source,s->ctl+352,80);std::memcpy(m->target,desc(s,true),80);
    m->source_device=get(s->ctl+432,8);m->writing=true;
    if(!m->source_device||!names_descriptor_valid(m->source))return fail(s,NAMES_CORRUPT);
    if(get(s->ctl+52,4)==0&&zero(s->ctl+56,32)){NamesResult r=start_file(s);if(r!=NAMES_OK)return r;r=progress(s,0);if(r!=NAMES_OK)return r;}
    return merge_start_hashes(s,m);
}
extern "C" NamesResult names_merge_page(NamesStore *s,NamesMerge *m,uint32_t start,const uint64_t *ids,size_t n,const uint64_t *peers,size_t count){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!m||!m->active||!ids||!n||n>89||start!=m->progress||start>get(m->source+64,4)||n>get(m->source+64,4)-start)return NAMES_STALE;
    uint8_t encoded[8];
    const uint32_t pslots=uint32_t(get(desc(s,false)+64,4));
    for(size_t j=0;j<n;++j){
        const uint32_t at=start+uint32_t(j)+1;uint64_t old=0;
        if(at<=pslots){NamesResult r=slot(s,bank(s,false),at,&old);if(r!=NAMES_OK)return fail(s,r);}
        if(old&&ids[j]&&old!=ids[j])return NAMES_CONFLICT;
        if(ids[j]){
            uint16_t assigned=0,pending=0;NamesResult r=names_lookup(s,ids[j],&assigned,&pending);
            if(r!=NAMES_OK)return r;
            if(assigned&&assigned!=at)return NAMES_CONFLICT;
            ++m->source_assigned;
        }
        put(encoded,ids[j],8);if(!merge_source_hash(m)->add(encoded,8))return NAMES_IO;
        const uint64_t merged=old?old:ids[j];put(encoded,merged,8);
        if(!merge_target_hash(m)->add(encoded,8))return NAMES_IO;
        if(merged)++m->assigned;
        if(m->writing){
            const uint32_t durable=uint32_t(get(s->ctl+52,4));
            if(at<=durable){uint64_t prior=0;NamesResult r=slot(s,bank(s,true),at,&prior);if(r!=NAMES_OK)return fail(s,r);if(prior!=merged)return NAMES_CONFLICT;}
            else {
                // Reject identity collisions even if both source pairs were new.
                if(merged)for(uint32_t k=1;k<at;++k){uint64_t prior=0;NamesResult r=slot(s,bank(s,true),k,&prior);if(r!=NAMES_OK)return fail(s,r);if(prior==merged)return NAMES_CONFLICT;}
                StoreResult r=s->backend.write_file(s->backend.context,bank(s,true),64+8*(at-1),encoded,8);if(r!=STORE_OK)return fail(s,result(r));
            }
        }
    }
    m->progress+=uint32_t(n);
    if(m->writing){StoreResult r=s->backend.sync(s->backend.context,bank(s,true));if(r!=STORE_OK)return fail(s,result(r));const uint32_t prior=uint32_t(get(s->ctl+52,4));NamesResult nr=progress(s,m->progress>prior?m->progress:prior);if(nr!=NAMES_OK)return nr;}
    if(m->progress!=get(m->source+64,4))return NAMES_OK;
    // Source is complete. The remaining union suffix consists solely of P.
    for(uint32_t at=m->progress+1;at<=pslots;++at){
        uint64_t id=0;NamesResult nr=slot(s,bank(s,false),at,&id);if(nr!=NAMES_OK)return fail(s,nr);
        put(encoded,id,8);if(!merge_target_hash(m)->add(encoded,8))return NAMES_IO;if(id)++m->assigned;
        if(m->writing){StoreResult r=s->backend.write_file(s->backend.context,bank(s,true),64+8*(at-1),encoded,8);if(r!=STORE_OK)return fail(s,result(r));}
    }
    uint8_t sh[32],th[32];if(!merge_source_hash(m)->finish(sh)||!merge_target_hash(m)->finish(th))return NAMES_IO;
    if(std::memcmp(sh,m->source+32,32)||m->source_assigned!=get(m->source+72,4))return NAMES_CORRUPT;
    merge_source_hash(m)->~Hash();merge_target_hash(m)->~Hash();m->active=false;
    if(m->writing){
        if(std::memcmp(th,m->target+32,32)||m->assigned!=get(m->target+72,4))return NAMES_CORRUPT;
        StoreResult r=s->backend.sync(s->backend.context,bank(s,true));if(r!=STORE_OK)return fail(s,result(r));
        NamesResult nr=progress(s,uint32_t(get(m->target+64,4)));return nr==NAMES_OK?prepared(s):nr;
    }
    std::memcpy(m->target+32,th,32);put(m->target+72,m->assigned,4);
    std::memcpy(desc(s,true),m->target,80);std::memcpy(s->ctl+352,m->source,80);put(s->ctl+432,m->source_device,8);
    put(s->ctl+40,get(m->target+8,8)+1,8);put(s->ctl+96,0,8);put(s->ctl+104,0,2);
    s->ctl[48]=NAMES_LOCAL_PREPARE;s->ctl[49]=4;s->ctl[51]=uint8_t((s->ctl[51]&(PB|PV))|TV|(bank(s,false)?0:TB));
    put(s->ctl+52,0,4);std::memset(s->ctl+56,0,32);
    NamesResult nr=names_set_barrier(s,peers,count,s->local_device);if(nr!=NAMES_OK)return nr;
    nr=start_file(s);if(nr!=NAMES_OK)return nr;nr=progress(s,0);if(nr!=NAMES_OK)return nr;
    return names_merge_resume(s,m);
}
