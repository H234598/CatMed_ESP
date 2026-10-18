#include "katmed_mesh_status.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <cstdio>
struct Memory {std::vector<uint8_t> bytes;unsigned writes=0;StoreResult failure=STORE_OK;};
StoreResult read(void *v,uint8_t *out,size_t *n){
    auto &m=*static_cast<Memory *>(v);if(m.bytes.empty())return STORE_NOT_FOUND;
    if(*n<m.bytes.size())return STORE_FULL;
    *n=m.bytes.size();std::memcpy(out,m.bytes.data(),*n);return STORE_OK;
}
StoreResult write(void *v,const uint8_t *b,size_t n){
    auto &m=*static_cast<Memory *>(v);++m.writes;
    if(m.failure!=STORE_OK)return m.failure;
    m.bytes.assign(b,b+n);return STORE_OK;
}
int main(){
    Memory memory;MeshHistory h{};uint8_t group[16]{1},scratch[4096],page[1044];
    h.backend={&memory,read,write};mesh_history_open(&h,group,scratch);
    assert(!h.error&&h.count==0);assert(mesh_history_add(&h,11));
    mesh_history_commit(&h,scratch);assert(memory.writes==1&&memory.bytes.size()==MESH_HISTORY_BLOB);
    assert(!mesh_history_add(&h,11));mesh_history_commit(&h,scratch);assert(memory.writes==1);
    MeshHistory restored{};restored.backend=h.backend;mesh_history_open(&restored,group,scratch);
    assert(restored.count==1&&restored.ids[0]==11&&!restored.error);
    // A secret is intentionally absent from the format. RESET CONFIG keeps group.
    mesh_history_open(&restored,group,scratch);assert(restored.count==1);
    memory.failure=STORE_COMMIT_UNKNOWN;mesh_history_add(&h,22);mesh_history_commit(&h,scratch);
    assert(h.error&&!h.dirty&&memory.writes==2);
    mesh_history_commit(&h,scratch);assert(memory.writes==2&&h.error);
    memory.failure=STORE_OK;mesh_history_add(&h,33);mesh_history_commit(&h,scratch);assert(!h.error);
    memory.bytes[30]^=1;mesh_history_open(&restored,group,scratch);assert(restored.error);
    memory.bytes[30]^=1;uint8_t other[16]{2};mesh_history_open(&restored,other,scratch);
    assert(!restored.error&&restored.count==0&&!std::memcmp(restored.group,other,16));
    for(uint64_t id=1;id<=256;++id)mesh_history_add(&h,id);
    assert(h.count==256);mesh_history_commit(&h,scratch);unsigned commits=memory.writes;
    assert(!mesh_history_add(&h,h.ids[0]));mesh_history_commit(&h,scratch);assert(memory.writes==commits);
    assert(!mesh_history_add(&h,999));mesh_history_commit(&h,scratch);assert(h.saturated&&memory.writes==commits+1);
    MeshHistory unioned{};unioned.generation=1;MeshStatusPeer meta{};MeshStatusPage view{};
    uint64_t current[9]{1,2,3,4,5,6,7,8,9};
    unsigned lengths[3]{};uint64_t t=8000;
    for(unsigned which:{2u,0u,1u}){
        lengths[which]=mesh_status_encode(&h,MS_PREHISTORY|MS_CURRENT|MS_SATURATED,uint8_t(which),1,current,9,page);
        assert(mesh_status_decode(page,lengths[which],true,1,&view));
        assert(mesh_status_merge(&unioned,&meta,&view,t));t+=8000;
    }
    assert(lengths[0]==1004&&lengths[1]==1004&&lengths[2]==608);
    assert(unioned.count==256&&unioned.saturated&&mesh_status_complete(&meta,24000));
    assert(mesh_status_complete(&meta,38999)&&!mesh_status_complete(&meta,39000));
    // HELLO metadata is independent and cannot keep a historical snapshot fresh.
    meta.hello_ms=40000;meta.hello_valid=true;assert(!mesh_status_complete(&meta,40000));
    const uint64_t generation=unioned.generation;
    assert(mesh_status_merge(&unioned,&meta,&view,40000));
    assert(mesh_status_complete(&meta,40000)&&unioned.generation==generation);
    view.generation++;assert(mesh_status_merge(&unioned,&meta,&view,41000));
    assert(!mesh_status_complete(&meta,41000)&&meta.mask==(1u<<view.page));
    MeshStatusPage older=view;--older.generation;
    assert(!mesh_status_merge(&unioned,&meta,&older,41500));
    mesh_history_commit(&h,scratch);mesh_history_open(&restored,group,scratch);
    assert(restored.saturated&&restored.count==256);
    // Nonce/root/group invalidation clears metadata, never IDs.
    meta=MeshStatusPeer{};assert(!mesh_status_complete(&meta,42000)&&unioned.count==256);
    page[18]=1;assert(!mesh_status_decode(page,lengths[1],true,1,&view));page[18]=0;
    assert(!mesh_status_decode(page,lengths[1]-1,true,1,&view));
    page[17]=10;assert(!mesh_status_decode(page,lengths[1],true,1,&view));
    // Root switch unions overlapping full IDs, never received counts.
    MeshHistory a{},b{};a.generation=b.generation=1;
    mesh_history_add(&a,11);mesh_history_add(&a,22);mesh_history_add(&b,22);mesh_history_add(&b,33);
    auto n=mesh_status_encode(&a,MS_PREHISTORY,0,33,nullptr,0,page);
    assert(mesh_status_decode(page,n,false,11,&view));meta=MeshStatusPeer{};
    assert(mesh_status_merge(&b,&meta,&view,1000)&&b.count==3);
    uint64_t gen=b.generation;assert(mesh_status_merge(&b,&meta,&view,2000)&&b.generation==gen);
    assert(mesh_status_fresh(15999,1000,true)&&!mesh_status_fresh(16000,1000,true));
    std::printf("global history codec/union/persistence/frische OK; History=%zu Status=%zu Peer=%zu Total=%zu\n",
        sizeof(MeshHistory),sizeof(MeshStatusState),sizeof(MeshStatusPeer),sizeof(MeshHistory)+sizeof(MeshStatusState));
}
