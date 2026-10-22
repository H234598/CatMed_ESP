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
    NamesResult observe_serial(NamesStore *s,const uint8_t *d){
        uint64_t highest=get(d+8,8);if(get(d+24,8)>highest)highest=get(d+24,8);
        if(highest==UINT64_MAX)return NAMES_INDEX_EXHAUSTED;
        const uint64_t next=highest+1;
        if(next>get(s->ctl+40,8))put(s->ctl+40,next,8);
        return NAMES_OK;
    }
    NamesResult result(StoreResult r){switch(r){case STORE_OK:return NAMES_OK;
            case STORE_FULL:return NAMES_FULL;
            case STORE_COMMIT_UNKNOWN:return NAMES_COMMIT_UNKNOWN;
            case STORE_CORRUPT:return NAMES_CORRUPT;
            case STORE_NOT_FOUND:return NAMES_NEED_SYNC;
            default:return NAMES_IO;
    }}
    NamesResult fail(NamesStore *s,NamesResult r){if(r!=NAMES_OK&&r!=NAMES_BUSY){s->poisoned=true;
            s->error=r;
        }return r;
    }
    NamesResult save(NamesStore *s){seal(s->ctl,480);
        return fail(s,result(s->backend.commit(s->backend.context,NAMES_CTL,s->ctl,480)));
    }
    NamesResult guard(NamesStore *s){return !s||!s->opened?NAMES_IO:s->opening?NAMES_BUSY:s->poisoned?s->error:NAMES_OK;
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
    void cancel_hash(NamesStore *s){
        if(s->scan_kind==4)delete static_cast<Hash *>(s->scan.digest.hash);
        s->scan_kind=0;
    }
    // The cursor and SHA state survive NAMES_BUSY. A new operation cannot
    // displace a partly checked immutable descriptor.
    NamesResult file_digest(NamesStore *s,unsigned b,const uint8_t *d,uint32_t slots,
                            uint64_t appended,bool planned,uint8_t out[32]){
        if(slots>65535)return NAMES_CORRUPT;
        auto &v=s->scan.digest;
        const uint8_t mode=planned?1:0;
        if(s->scan_kind==4){
            if(v.mode!=mode||v.bank!=b||v.slots!=slots||v.appended!=appended||
               std::memcmp(v.descriptor,d,80))return NAMES_BUSY;
        }else{
            if((s->scan_kind==1&&!s->scan.unique.done)||s->scan_kind==2)return NAMES_BUSY;
            s->scan_kind=4;
            std::memcpy(v.descriptor,d,80);v.bank=uint8_t(b);v.slots=slots;
            v.appended=appended;v.mode=mode;v.offset=0;
            v.hash=new(std::nothrow) Hash;
            if(!v.hash){s->scan_kind=0;return NAMES_IO;}
            if(!static_cast<Hash *>(v.hash)->good){cancel_hash(s);return NAMES_IO;}
        }
        auto *hash=static_cast<Hash *>(v.hash);
        uint8_t bytes[512],expected[64];
        if(!v.offset){
            header(s,d,expected);
            if(planned)std::memcpy(bytes,expected,64);
            else{
                if(s->scan_limited){if(!s->scan_budget)return NAMES_BUSY;--s->scan_budget;}
                const StoreResult sr=s->backend.read_file(s->backend.context,b,0,bytes,64);
                if(sr!=STORE_OK){cancel_hash(s);return result(sr);}
                if(std::memcmp(bytes,expected,64)){cancel_hash(s);return NAMES_CORRUPT;}
            }
            if(!hash->add(bytes,64)){cancel_hash(s);return NAMES_IO;}
            v.offset=64;
        }
        const uint32_t end=64+8*slots;
        while(v.offset<end){
            if(s->scan_limited){if(!s->scan_budget)return NAMES_BUSY;--s->scan_budget;}
            size_t n=end-v.offset;if(n>sizeof(bytes))n=sizeof(bytes);
            const StoreResult sr=s->backend.read_file(s->backend.context,b,v.offset,bytes,n);
            if(sr!=STORE_OK){cancel_hash(s);return result(sr);}
            if(!hash->add(bytes,n)){cancel_hash(s);return NAMES_IO;}
            v.offset+=uint32_t(n);
        }
        if(planned&&appended){put(bytes,appended,8);if(!hash->add(bytes,8)){cancel_hash(s);return NAMES_IO;}}
        const bool ok=hash->finish(out);cancel_hash(s);
        return ok?NAMES_OK:NAMES_IO;
    }
    NamesResult digest(NamesStore *s,unsigned b,const uint8_t *d,uint32_t slots,uint8_t out[32]){
        return file_digest(s,b,d,slots,0,false,out);
    }
    NamesResult planned_digest(NamesStore *s,uint8_t *d,uint64_t appended){
        const uint32_t slots=(s->ctl[51]&PV)?uint32_t(get(desc(s,false)+64,4)):0;
        return file_digest(s,bank(s,false),d,slots,appended,true,d+32);
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
        if(t&&s->target_validated)return NAMES_OK;
        uint8_t hash[32];
        NamesResult r=digest(s,bank(s,t),d,uint32_t(get(d+64,4)),hash);
        if(r!=NAMES_OK)return r;
        if(std::memcmp(hash,d+32,32))return NAMES_CORRUPT;
        if(t)s->target_validated=true;
        return NAMES_OK;
    }
    void heap_down(uint64_t *h,unsigned n,unsigned at){
        for(;;){unsigned child=at*2+1;if(child>=n)return;
            if(child+1<n&&h[child+1]>h[child])++child;
            if(h[at]>=h[child])return;
            const uint64_t v=h[at];h[at]=h[child];h[child]=v;at=child;
        }
    }
    void heap_add(NamesStore *s,uint64_t id){
        auto &u=s->scan.unique;
        const unsigned n=u.count;
        if(n<16){unsigned at=n;u.count=n+1;
            while(at){unsigned parent=(at-1)/2;if(u.heap[parent]>=id)break;
                u.heap[at]=u.heap[parent];at=parent;
            }u.heap[at]=id;
        }else if(id<u.heap[0]){
            u.heap[0]=id;heap_down(u.heap,16,0);
        }
    }
    NamesResult unique(NamesStore *s){
        if(s->scan_kind==2||s->scan_kind==4)return NAMES_BUSY;
        auto &u=s->scan.unique;
        const uint8_t *d=desc(s,true);
        const uint32_t slots=uint32_t(get(d+64,4));
        const uint8_t b=uint8_t(bank(s,true));
        if(s->scan_kind!=1||u.bank!=b||u.slots!=slots||
           std::memcmp(u.target,d,80)){
            std::memcpy(u.target,d,80);u.bank=b;
            u.slots=slots;u.after=0;u.offset=0;
            u.count=u.equal=u.assigned=0;
            s->preserve_offset=0;
            s->scan_kind=1;u.done=false;
        }
        if(u.done)return NAMES_OK;
        uint8_t bytes[512];
        for(;;){
            while(u.offset<slots){
                if(s->scan_limited){if(!s->scan_budget)return NAMES_BUSY;--s->scan_budget;}
                const uint32_t left=slots-u.offset;
                const uint32_t n=left>64?64:left;
                const StoreResult sr=s->backend.read_file(s->backend.context,b,
                    64+8*u.offset,bytes,8*n);
                if(sr!=STORE_OK)return result(sr);
                for(uint32_t i=0;i<n;++i){
                    const uint64_t id=get(bytes+8*i,8);
                    if(id&&!u.after)++u.assigned;
                    if(id==u.after&&id){
                        if(++u.equal>1)return NAMES_CONFLICT;
                    }else if(id>u.after)heap_add(s,id);
                }
                u.offset+=n;
            }
            const unsigned count=u.count;
            // Heap sort the selected smallest identities. Equal neighbours
            // prove a duplicate even when all file hashes and counts match.
            for(unsigned n=count;n>1;--n){
                const uint64_t v=u.heap[0];
                u.heap[0]=u.heap[n-1];
                u.heap[n-1]=v;heap_down(u.heap,n-1,0);
            }
            for(unsigned i=1;i<count;++i)
                if(u.heap[i]==u.heap[i-1])return NAMES_CONFLICT;
            if(count<16){
                if(u.assigned!=get(d+72,4))return NAMES_CORRUPT;
                u.done=true;return NAMES_OK;
            }
            u.after=u.heap[count-1];
            u.count=u.equal=0;u.offset=0;
        }
    }
    NamesResult preserve(NamesStore *s){
        const uint32_t newslots=uint32_t(get(desc(s,true)+64,4));
        if(s->ctl[51]&PV){
            const uint32_t oldslots=uint32_t(get(desc(s,false)+64,4));
            if(newslots<oldslots)return NAMES_CONFLICT;
            uint8_t old[256],candidate[256];
            while(s->preserve_offset<oldslots){
                if(s->scan_limited&&s->scan_budget<2)return NAMES_BUSY;
                const uint32_t count=(oldslots-s->preserve_offset)>32?32:oldslots-s->preserve_offset;
                const uint32_t at=64+8*s->preserve_offset;
                if(s->scan_limited)s->scan_budget-=2;
                StoreResult sr=s->backend.read_file(s->backend.context,bank(s,false),at,old,8*count);
                if(sr!=STORE_OK)return result(sr);
                sr=s->backend.read_file(s->backend.context,bank(s,true),at,candidate,8*count);
                if(sr!=STORE_OK)return result(sr);
                for(uint32_t j=0;j<count;++j){
                    const uint64_t id=get(old+8*j,8);
                    if(id&&id!=get(candidate+8*j,8))return NAMES_CONFLICT;
                }
                s->preserve_offset+=count;
            }
        }
        const uint64_t reserve=get(s->ctl+96,8);const uint16_t index=uint16_t(get(s->ctl+104,2));
        if(reserve){uint64_t id=0;if(index>newslots)return NAMES_CONFLICT;
            if(s->scan_limited){if(!s->scan_budget)return NAMES_BUSY;--s->scan_budget;}
            NamesResult r=slot(s,bank(s,true),index,&id);if(r!=NAMES_OK)return r;if(id!=reserve)return NAMES_CONFLICT;}
        s->preserve_offset=0;
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
        if(s->scan_limited){if(!s->scan_budget)return NAMES_BUSY;--s->scan_budget;}
        NamesResult nr=slot(s,bank(s,false),index,&id);
        if(nr!=NAMES_OK)return nr;
        if(id!=s->local_device)return NAMES_CORRUPT;
        s->visible_index=index;
        return NAMES_OK;
    }
    NamesResult progress(NamesStore *s,uint32_t slots){uint8_t hash[32];
        NamesResult r=digest(s,bank(s,true),desc(s,true),slots,hash);
        if(r!=NAMES_OK)return fail(s,r);
        if(slots==get(desc(s,true)+64,4)){
            if(std::memcmp(hash,desc(s,true)+32,32))return fail(s,NAMES_CORRUPT);
            s->target_validated=true;
        }
        put(s->ctl+52,slots,4);
        std::memcpy(s->ctl+56,hash,32);
        return save(s);
    }
    NamesResult prepared(NamesStore *s){NamesResult r=validate(s,true);
        if(r!=NAMES_OK)return fail(s,r);
        r=unique(s);
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
    NamesResult finish_open(NamesStore *s){
        if(s->open_stage==0){
            if(s->ctl[51]&PV){
                const NamesResult nr=validate(s,false);
                if(nr!=NAMES_OK)return fail(s,nr);
            }
            s->open_stage=1;
        }
        if(s->open_stage==1){
            const NamesResult nr=load_view(s);
            if(nr!=NAMES_OK)return fail(s,nr);
            s->open_stage=2;
        }
        if(s->open_stage==2){
            if(s->ctl[51]&TV){
                if(!names_descriptor_valid(desc(s,true)))return fail(s,NAMES_CORRUPT);
                if(s->ctl[51]&TC){
                    const NamesResult nr=validate(s,true);
                    if(nr!=NAMES_OK)return fail(s,nr);
                }else if(s->ctl[48]!=NAMES_LOCAL_PREPARE){
                    const uint32_t slots=uint32_t(get(s->ctl+52,4));
                    if(slots>get(desc(s,true)+64,4))return fail(s,NAMES_CORRUPT);
                    uint8_t hash[32];NamesResult nr=NAMES_OK;
                    if(!(slots==0&&zero(s->ctl+56,32)&&
                         (!(s->ctl[51]&PV)||bank(s,true)!=bank(s,false))))
                        nr=digest(s,bank(s,true),desc(s,true),slots,hash);
                    else std::memset(hash,0,32);
                    if(nr!=NAMES_OK)return fail(s,nr);
                    if(std::memcmp(hash,s->ctl+56,32))return fail(s,NAMES_CORRUPT);
                }
            }
            s->open_stage=3;
        }
        s->opening=false;
        return NAMES_OK;
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
static NamesResult open_impl(NamesStore *s,NamesBackend backend,uint64_t device,const uint8_t group[16],
                             bool limited,uint16_t budget){
    if(!s||!device||!group||!backend.read||!backend.commit||!backend.size||!backend.read_file||!backend.write_file||!backend.truncate||!backend.sync)return NAMES_IO;
    std::memset(s,0,sizeof(*s));
    s->scan_limited=limited;s->scan_budget=budget;
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
    s->opening=true;s->open_stage=0;
    return finish_open(s);
}
extern "C" NamesResult names_open(NamesStore *s,NamesBackend backend,uint64_t device,const uint8_t group[16]){
    return open_impl(s,backend,device,group,false,0);
}
extern "C" NamesResult names_open_bounded(NamesStore *s,NamesBackend backend,uint64_t device,const uint8_t group[16],uint16_t reads){
    return open_impl(s,backend,device,group,true,reads);
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
extern "C" void names_poll_budget(NamesStore *s,uint16_t reads){
    if(s){s->scan_limited=true;s->scan_budget=reads;}
}
extern "C" void names_cancel_scan(NamesStore *s){if(s)cancel_hash(s);}
extern "C" NamesResult names_lookup(NamesStore *s,uint64_t device,uint16_t *confirmed,uint16_t *reserved){
    if(confirmed)*confirmed=0;
    if(reserved)*reserved=0;
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!device)return NAMES_CORRUPT;
    if((s->scan_kind==1&&!s->scan.unique.done)||s->scan_kind==4)return NAMES_BUSY;
    auto &v=s->scan.lookup;
    const uint8_t flags=s->ctl[51],phase=s->ctl[48];
    const bool anchored=(s->scan_kind==2||s->scan_kind==3)&&v.flags==flags&&v.phase==phase&&
        v.reservation==get(s->ctl+96,8)&&
        !std::memcmp(v.p,desc(s,false),80)&&!std::memcmp(v.t,desc(s,true),80);
    if(anchored&&s->scan_kind==2&&v.device!=device)return NAMES_BUSY;
    if(anchored&&s->scan_kind==3&&v.device==device){
        if(confirmed)*confirmed=v.found;
        if(reserved)*reserved=v.held;
        return NAMES_OK;
    }
    if(!anchored){
        s->scan_kind=2;v.device=device;v.flags=flags;v.phase=phase;
        v.reservation=get(s->ctl+96,8);v.found=v.held=0;
        std::memcpy(v.p,desc(s,false),80);std::memcpy(v.t,desc(s,true),80);
        v.stage=0;v.next=1;
    }else if(s->scan_kind==3){
        s->scan_kind=2;v.device=device;v.found=v.held=0;v.stage=0;v.next=1;
    }
    for(;v.stage<3;++v.stage,v.next=1){
        if(v.stage==1){
            if(get(s->ctl+96,8)==device){v.held=uint16_t(get(s->ctl+104,2));if(reserved)*reserved=v.held;s->scan_kind=3;return NAMES_OK;}
            continue;
        }
        const bool target=v.stage==2;
        if(!(flags&(target?TV:PV)))continue;
        const uint32_t slots=target&&!(flags&TC)?uint32_t(get(s->ctl+52,4)):
            uint32_t(get(desc(s,target)+64,4));
        for(;v.next<=slots;++v.next){
            if(s->scan_limited){if(!s->scan_budget)return NAMES_BUSY;--s->scan_budget;}
            uint64_t id=0;NamesResult r=slot(s,bank(s,target),v.next,&id);
            if(r!=NAMES_OK){s->scan_kind=0;return fail(s,r);}
            if(id==device){
                if(target){v.held=uint16_t(v.next);if(reserved)*reserved=v.held;}
                else {v.found=uint16_t(v.next);if(confirmed)*confirmed=v.found;}
                s->scan_kind=3;return NAMES_OK;
            }
        }
    }
    s->scan_kind=3;return NAMES_OK;
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
    if(serial==UINT64_MAX||get(s->ctl+88,8)==UINT64_MAX)return NAMES_INDEX_EXHAUSTED;
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
    s->target_validated=false;
    s->ctl[48]=NAMES_LOCAL_PREPARE;
    s->ctl[49]=1;
    s->ctl[51]=uint8_t((s->ctl[51]&PB)|PV|TV|((s->ctl[51]&PB)?TB:0));
    s->ctl[50]=uint8_t(n);std::memset(s->ctl+272,0,64);
    for(size_t i=0;i<n;++i)put(s->ctl+272+8*i,peers[i],8);
    /* This RAM-only plan may span polls. No file or control commit occurs
     * until the immutable digest is complete. */
    s->planning=true;
    return names_resume(s);
}
extern "C" NamesResult names_resume(NamesStore *s){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(s->planning){
        NamesResult r=planned_digest(s,desc(s,true),get(s->ctl+96,8));
        if(r!=NAMES_OK)return fail(s,r);
        uint64_t members[8];const size_t count=s->ctl[50];
        for(size_t i=0;i<count;++i)members[i]=get(s->ctl+272+8*i,8);
        s->planning=false;
        r=names_set_barrier(s,members,count,s->local_device);
        if(r!=NAMES_OK)return r;
    }
    if((s->ctl[48]==NAMES_RX_APPEND||s->ctl[48]==NAMES_RX_REPLACE)&&
       s->scan_kind==4&&!s->scan.digest.mode&&
       s->scan.digest.bank==bank(s,true)&&
       !std::memcmp(s->scan.digest.descriptor,desc(s,true),80)&&
       s->scan.digest.slots>get(s->ctl+52,4)){
        const uint32_t slots=s->scan.digest.slots;
        NamesResult r=progress(s,slots);
        if(r!=NAMES_OK)return r;
        return slots==get(desc(s,true)+64,4)?prepared(s):NAMES_OK;
    }
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
    if(!s->target_validated){
        uint8_t hash[32];
        r=digest(s,bank(s,true),desc(s,true),uint32_t(get(desc(s,true)+64,4)),hash);
        if(r!=NAMES_OK)return fail(s,r);
        if(std::memcmp(desc(s,true)+32,hash,32))return fail(s,NAMES_CORRUPT);
        put(s->ctl+52,get(desc(s,true)+64,4),4);
        std::memcpy(s->ctl+56,hash,32);
        s->target_validated=true;
    }
    return prepared(s);
}
extern "C" NamesResult names_publish(NamesStore *s){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(s->ctl[48]==NAMES_FINALIZING)return NAMES_OK;
    if(s->ctl[48]!=NAMES_PREPARED)return NAMES_STALE;
    NamesResult r=validate(s,true);
    if(r!=NAMES_OK)return fail(s,r);
    r=unique(s);
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
    s->target_validated=false;
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
    uint8_t bytes[256];
    for(size_t i=0;i<*n;){
        if(s->scan_limited){if(!s->scan_budget)return NAMES_BUSY;--s->scan_budget;}
        size_t count=*n-i;if(count>32)count=32;
        const StoreResult sr=s->backend.read_file(s->backend.context,bank(s,target),
            64+8*(start+uint32_t(i)),bytes,8*count);
        if(sr!=STORE_OK)return fail(s,result(sr));
        for(size_t j=0;j<count;++j)ids[i+j]=get(bytes+8*j,8);
        i+=count;
    }
    return NAMES_OK;
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
    names_cancel_scan(s);
    if(s->fresh)std::memcpy(s->ctl+24,registry,16);
    if(observe_serial(s,target)!=NAMES_OK)return NAMES_INDEX_EXHAUSTED;
    const bool share=append&&(s->ctl[51]&PV)&&!std::memcmp(target+16,desc(s,false)+16,16)&&get(target+64,4)>=get(desc(s,false)+64,4);
    if(append&&!share)return NAMES_STALE;
    const unsigned candidate=share?bank(s,false):((s->ctl[51]&PV)?1-bank(s,false):0);
    std::memcpy(desc(s,true),target,80);
    s->target_validated=false;
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
    if(s->scan_kind==4&&!s->scan.digest.mode&&
       s->scan.digest.slots>get(s->ctl+52,4)){
        const NamesResult pending=names_resume(s);
        if(pending!=NAMES_OK)return pending;
    }
    const uint32_t at=uint32_t(get(s->ctl+52,4)),slots=uint32_t(get(desc(s,true)+64,4));
    if(start>slots||n>slots-start)return NAMES_STALE;
    if(start<at){if(n>at-start)return NAMES_STALE;
        uint8_t bytes[256];
        for(size_t i=0;i<n;){
            if(s->scan_limited){if(!s->scan_budget)return NAMES_BUSY;--s->scan_budget;}
            size_t count=n-i;if(count>32)count=32;
            const StoreResult sr=s->backend.read_file(s->backend.context,bank(s,true),
                64+8*(start+uint32_t(i)),bytes,8*count);
            if(sr!=STORE_OK)return fail(s,result(sr));
            for(size_t j=0;j<count;++j)if(get(bytes+8*j,8)!=ids[i+j])return NAMES_CONFLICT;
            i+=count;
        }
        return NAMES_OK;
    }
    if(start!=at)return NAMES_STALE;
    uint8_t bytes[8];StoreResult sr=STORE_OK;
    s->target_validated=false;
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
    if(uncertain)*uncertain=!s||!s->opened||s->opening||s->poisoned||s->error!=NAMES_OK||s->ctl[48]!=NAMES_IDLE;
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
    names_cancel_scan(s);
    std::memcpy(v->target,target,80);new(v->hash_state)Hash();v->active=true;
    std::memcpy(v->anchor_p,desc(s,false),80);
    std::memcpy(v->anchor_t,desc(s,true),80);
    v->anchor_flags=s->ctl[51];v->anchor_phase=s->ctl[48];
    v->anchor_view=s->visible_index;
    v->anchor_progress=uint32_t(get(s->ctl+52,4));
    std::memcpy(v->anchor_progress_hash,s->ctl+56,32);
    v->anchor_reserve_device=get(s->ctl+96,8);v->anchor_reserve_index=uint16_t(get(s->ctl+104,2));
    v->anchor_controller=get(s->ctl+336,8);v->anchor_epoch=get(s->ctl+88,8);
    uint8_t h[64];header(s,target,h);
    if(!verify_hash(v)->add(h,64)){names_retarget_cancel(v);return NAMES_IO;}
    return NAMES_OK;
}
extern "C" NamesResult names_retarget_page(NamesStore *s,NamesRetarget *v,uint32_t start,const uint64_t *ids,size_t n){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!v||!v->active||v->complete||!ids||!n||n>89||start!=v->progress||
       start>get(v->target+64,4)||n>get(v->target+64,4)-start)return NAMES_STALE;
    if(v->anchor_flags!=s->ctl[51]||v->anchor_phase!=s->ctl[48]||
       v->anchor_progress!=get(s->ctl+52,4)||std::memcmp(v->anchor_progress_hash,s->ctl+56,32)||
       v->anchor_reserve_device!=get(s->ctl+96,8)||v->anchor_reserve_index!=get(s->ctl+104,2)||
       v->anchor_view!=s->visible_index||v->anchor_controller!=get(s->ctl+336,8)||
       v->anchor_epoch!=get(s->ctl+88,8)||std::memcmp(v->anchor_p,desc(s,false),80)||
       std::memcmp(v->anchor_t,desc(s,true),80))return NAMES_STALE;
    const uint32_t oldprogress=(s->ctl[51]&TC)?uint32_t(get(desc(s,true)+64,4)):uint32_t(get(s->ctl+52,4));
    if(s->scan_limited){
        const uint16_t cost=uint16_t(2*((n+31)/32));
        if(s->scan_budget<cost)return NAMES_BUSY;
        s->scan_budget-=cost;
    }
    uint8_t bytes[8];
    const uint32_t pslots=(s->ctl[51]&PV)?uint32_t(get(desc(s,false)+64,4)):0;
    for(size_t offset=0;offset<n;offset+=32){
        const size_t count=(n-offset)>32?32:n-offset;
        const uint32_t first=start+uint32_t(offset)+1;
        uint8_t published[256]{},pending[256]{};
        if(first<=pslots){
            const size_t available=(pslots-first+1)<count?pslots-first+1:count;
            const StoreResult sr=s->backend.read_file(s->backend.context,bank(s,false),
                64+8*(first-1),published,8*available);
            if(sr!=STORE_OK)return fail(s,result(sr));
        }
        if(first<=oldprogress){
            const size_t available=(oldprogress-first+1)<count?oldprogress-first+1:count;
            const StoreResult sr=s->backend.read_file(s->backend.context,bank(s,true),
                64+8*(first-1),pending,8*available);
            if(sr!=STORE_OK)return fail(s,result(sr));
        }
        for(size_t j=0;j<count;++j){
            const uint32_t at=first+uint32_t(j);
            const uint64_t id=ids[offset+j];
            const uint64_t p=get(published+8*j,8),t=get(pending+8*j,8);
            if((p&&p!=id)||(t&&t!=id))return NAMES_CONFLICT;
            if(at==get(s->ctl+104,2)&&get(s->ctl+96,8)&&id!=get(s->ctl+96,8))return NAMES_CONFLICT;
            if(at==v->reserve_index&&v->reserve_device&&id!=v->reserve_device)return NAMES_CONFLICT;
            put(bytes,id,8);if(!verify_hash(v)->add(bytes,8))return NAMES_IO;
            if(id)++v->assigned;
        }
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
    if(!v||!v->complete||!members_valid(peers,n,controller)||!epoch||
       bool(v->reserve_device)!=bool(v->reserve_index)||
       v->reserve_index>get(v->target+64,4))return NAMES_STALE;
    if(v->controller&&v->controller!=controller)return NAMES_STALE;
    if(v->epoch&&v->epoch!=epoch)return NAMES_STALE;
    if(v->barrier_count&&v->barrier_count!=n)return NAMES_STALE;
    for(size_t i=0;i<v->barrier_count;++i)if(v->barrier[i]!=peers[i])return NAMES_STALE;
    if(v->anchor_flags!=s->ctl[51]||v->anchor_phase!=s->ctl[48]||
       v->anchor_progress!=get(s->ctl+52,4)||std::memcmp(v->anchor_progress_hash,s->ctl+56,32)||
       v->anchor_reserve_device!=get(s->ctl+96,8)||v->anchor_reserve_index!=get(s->ctl+104,2)||
       v->anchor_view!=s->visible_index||v->anchor_controller!=get(s->ctl+336,8)||
       v->anchor_epoch!=get(s->ctl+88,8)||std::memcmp(v->anchor_p,desc(s,false),80)||
       std::memcmp(v->anchor_t,desc(s,true),80))return NAMES_STALE;
    if(v->reserve_device&&get(s->ctl+96,8)&&
       (get(s->ctl+96,8)!=v->reserve_device||get(s->ctl+104,2)!=v->reserve_index))return NAMES_CONFLICT;
    const unsigned candidate=(s->ctl[51]&PV)?1-bank(s,false):bank(s,true);
    if(observe_serial(s,v->target)!=NAMES_OK)return NAMES_INDEX_EXHAUSTED;
    names_cancel_scan(s);
    std::memcpy(desc(s,true),v->target,80);std::memcpy(s->ctl+352,v->target,80);
    s->target_validated=false;
    put(s->ctl+432,controller,8);put(s->ctl+336,controller,8);put(s->ctl+88,epoch,8);
    s->ctl[48]=NAMES_RX_REPLACE;s->ctl[49]=3;s->ctl[50]=uint8_t(n);
    s->ctl[51]=uint8_t((s->ctl[51]&(PB|PV))|TV|REPLACE|(candidate?TB:0));
    put(s->ctl+52,0,4);std::memset(s->ctl+56,0,32);std::memset(s->ctl+272,0,64);
    if(v->reserve_device){put(s->ctl+96,v->reserve_device,8);put(s->ctl+104,v->reserve_index,2);}
    for(size_t i=0;i<n;++i)put(s->ctl+272+8*i,peers[i],8);
    NamesResult r=save(s); // Only known-new intent authorizes candidate reset.
    if(r!=NAMES_OK)return r;
    std::memset(v,0,sizeof(*v));
    r=start_file(s);return r==NAMES_OK?progress(s,0):r;
}

extern "C" NamesResult names_reopen(NamesStore *s){
    if(!s||!s->opened)return NAMES_IO;
    if(s->opening)return finish_open(s);
    names_cancel_scan(s);
    const bool limited=s->scan_limited;const uint16_t budget=s->scan_budget;
    NamesBackend backend=s->backend;uint8_t group[16];std::memcpy(group,s->expected_group,16);
    uint64_t device=s->local_device;
    if(backend.reopen){StoreResult r=backend.reopen(backend.context);if(r!=STORE_OK)return fail(s,result(r));}
    return open_impl(s,backend,device,group,limited,budget);
}

namespace {
Hash *merge_source_hash(NamesMerge *m){return reinterpret_cast<Hash *>(m->hashes);}
Hash *merge_target_hash(NamesMerge *m){return reinterpret_cast<Hash *>(m->hashes+32);}
NamesResult merge_start_hashes(NamesStore *s,NamesMerge *m){
    new(m->hashes)Hash();new(m->hashes+32)Hash();m->active=true;m->hashes_active=true;m->stage=0;
    uint8_t h[64];header(s,m->source,h);
    if(!merge_source_hash(m)->add(h,64))return NAMES_IO;
    header(s,m->target,h);return merge_target_hash(m)->add(h,64)?NAMES_OK:NAMES_IO;
}
}
extern "C" void names_merge_cancel(NamesMerge *m){
    if(m&&m->hashes_active){merge_source_hash(m)->~Hash();merge_target_hash(m)->~Hash();}
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
    if(observed>=UINT64_MAX-1)return NAMES_INDEX_EXHAUSTED;
    if(serial<=observed)serial=observed+1;
    if(serial==UINT64_MAX||get(s->ctl+88,8)==UINT64_MAX)return NAMES_INDEX_EXHAUSTED;
    put(t,s->local_device,8);put(t+8,serial,8);put(t+16,s->local_device,8);put(t+24,serial,8);
    const uint32_t a=uint32_t(get(source+64,4)),b=uint32_t(get(desc(s,false)+64,4));
    put(t+64,a>b?a:b,4);put(t+68,(a>b?a:b)+1,4);put(t+76,1,2);
    names_cancel_scan(s);
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
extern "C" NamesResult names_merge_advance(NamesStore *s,NamesMerge *m,const uint64_t *peers,size_t count){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!m||!m->active)return NAMES_STALE;
    const uint32_t source_slots=uint32_t(get(m->source+64,4));
    const uint32_t pslots=uint32_t(get(desc(s,false)+64,4));
    if(m->stage==5){
        const uint32_t durable=uint32_t(get(s->ctl+52,4));
        if(m->progress>durable){
            NamesResult r=progress(s,m->progress);
            if(r!=NAMES_OK)return r;
        }
        m->stage=m->progress==source_slots?1:0;
    }
    if(m->stage==1){
        uint8_t bytes[8];
        while(m->progress<pslots){
            const uint32_t at=m->progress+1;
            const bool durable=m->writing&&at<=get(s->ctl+52,4);
            if(s->scan_limited){
                const uint16_t cost=durable?2:1;
                if(s->scan_budget<cost)return NAMES_BUSY;
                s->scan_budget-=cost;
            }
            uint64_t id=0;NamesResult nr=slot(s,bank(s,false),at,&id);
            if(nr!=NAMES_OK)return fail(s,nr);
            put(bytes,id,8);
            if(!merge_target_hash(m)->add(bytes,8))return NAMES_IO;
            if(id)++m->assigned;
            if(m->writing){
                if(durable){uint64_t prior=0;nr=slot(s,bank(s,true),at,&prior);
                    if(nr!=NAMES_OK)return fail(s,nr);
                    if(prior!=id)return NAMES_CONFLICT;
                }else{
                    const StoreResult sr=s->backend.write_file(s->backend.context,bank(s,true),64+8*(at-1),bytes,8);
                    if(sr!=STORE_OK)return fail(s,result(sr));
                }
            }
            m->progress=at;
        }
        m->stage=2;
    }
    if(m->stage==2){
        uint8_t sh[32],th[32];
        if(!merge_source_hash(m)->finish(sh)||!merge_target_hash(m)->finish(th))return NAMES_IO;
        if(std::memcmp(sh,m->source+32,32)||m->source_assigned!=get(m->source+72,4))return NAMES_CORRUPT;
        merge_source_hash(m)->~Hash();merge_target_hash(m)->~Hash();m->hashes_active=false;
        if(m->writing){
            if(std::memcmp(th,m->target+32,32)||m->assigned!=get(m->target+72,4))return NAMES_CORRUPT;
            m->stage=3;
        }else{
            m->active=false;
            std::memcpy(m->target+32,th,32);put(m->target+72,m->assigned,4);
            std::memcpy(desc(s,true),m->target,80);std::memcpy(s->ctl+352,m->source,80);
            put(s->ctl+432,m->source_device,8);s->target_validated=false;
            put(s->ctl+40,get(m->target+8,8)+1,8);put(s->ctl+96,0,8);put(s->ctl+104,0,2);
            s->ctl[48]=NAMES_LOCAL_PREPARE;s->ctl[49]=4;
            s->ctl[51]=uint8_t((s->ctl[51]&(PB|PV))|TV|(bank(s,false)?0:TB));
            put(s->ctl+52,0,4);std::memset(s->ctl+56,0,32);
            NamesResult nr=names_set_barrier(s,peers,count,s->local_device);
            if(nr!=NAMES_OK)return nr;
            nr=start_file(s);if(nr!=NAMES_OK)return nr;
            nr=progress(s,0);return nr==NAMES_OK?names_merge_resume(s,m):nr;
        }
    }
    if(m->stage==3){
        StoreResult sr=s->backend.sync(s->backend.context,bank(s,true));
        if(sr!=STORE_OK)return fail(s,result(sr));
        NamesResult nr=progress(s,uint32_t(get(m->target+64,4)));
        if(nr!=NAMES_OK)return nr;
        m->stage=4;
    }
    if(m->stage==4){
        const NamesResult nr=prepared(s);
        if(nr==NAMES_OK)m->active=false;
        return nr;
    }
    return NAMES_OK;
}
extern "C" NamesResult names_merge_page(NamesStore *s,NamesMerge *m,uint32_t start,const uint64_t *ids,size_t n,const uint64_t *peers,size_t count){
    if(guard(s)!=NAMES_OK)return guard(s);
    if(!m||!m->active||!ids||!n||n>89)return NAMES_STALE;
    if(m->stage!=0)return NAMES_BUSY;
    if(start!=m->progress||
       start>get(m->source+64,4)||n>get(m->source+64,4)-start)return NAMES_STALE;
    const unsigned chunks=unsigned((n+31)/32);
    if(s->scan_limited){
        if(s->scan_budget<2*chunks)return NAMES_BUSY;
        s->scan_budget=uint16_t(s->scan_budget-2*chunks);
    }
    const uint32_t pslots=uint32_t(get(desc(s,false)+64,4));
    const uint32_t durable=m->writing?uint32_t(get(s->ctl+52,4)):0;
    for(size_t offset=0;offset<n;offset+=32){
        const size_t count=(n-offset)>32?32:n-offset;
        const uint32_t first=start+uint32_t(offset)+1;
        uint8_t old[256]{},prior[256]{},encoded[8];
        if(first<=pslots){
            const size_t available=(pslots-first+1)<count?pslots-first+1:count;
            const StoreResult sr=s->backend.read_file(s->backend.context,bank(s,false),
                64+8*(first-1),old,8*available);
            if(sr!=STORE_OK)return fail(s,result(sr));
        }
        if(m->writing&&first<=durable){
            const size_t available=(durable-first+1)<count?durable-first+1:count;
            const StoreResult sr=s->backend.read_file(s->backend.context,bank(s,true),
                64+8*(first-1),prior,8*available);
            if(sr!=STORE_OK)return fail(s,result(sr));
        }
        for(size_t j=0;j<count;++j){
            const uint32_t at=first+uint32_t(j);
            const uint64_t old_id=get(old+8*j,8);
            const uint64_t source_id=ids[offset+j];
            if(old_id&&source_id&&old_id!=source_id)return NAMES_CONFLICT;
            if(source_id)++m->source_assigned;
            put(encoded,source_id,8);
            if(!merge_source_hash(m)->add(encoded,8))return NAMES_IO;
            const uint64_t merged=old_id?old_id:source_id;
            put(encoded,merged,8);
            if(!merge_target_hash(m)->add(encoded,8))return NAMES_IO;
            if(merged)++m->assigned;
            if(m->writing){
                if(at<=durable){if(get(prior+8*j,8)!=merged)return NAMES_CONFLICT;}
                else{
                    const StoreResult sr=s->backend.write_file(s->backend.context,bank(s,true),
                        64+8*(at-1),encoded,8);
                    if(sr!=STORE_OK)return fail(s,result(sr));
                }
            }
        }
    }
    m->progress+=uint32_t(n);
    if(m->writing){
        const StoreResult sr=s->backend.sync(s->backend.context,bank(s,true));
        if(sr!=STORE_OK)return fail(s,result(sr));
        m->stage=5;
        return names_merge_advance(s,m,peers,count);
    }
    if(m->progress==get(m->source+64,4)){
        m->stage=1;
        return names_merge_advance(s,m,peers,count);
    }
    return NAMES_OK;
}
