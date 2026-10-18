#include "katmed_store.h"
#include <cassert>
#include <cstring>
#include <cstdio>

struct Backend {
    uint8_t bytes[KATMED_STORE_BLOB_MAX]{};
    size_t length=0;
    bool bad_read=false;
    bool bad_write=false;
};
static StoreResult read_blob(void *context,uint8_t *out,size_t *length){
    auto &b=*static_cast<Backend *>(context);
    if(b.bad_read)return STORE_IO;
    if(!b.length)return STORE_NOT_FOUND;
    assert(b.length<=*length);
    std::memcpy(out,b.bytes,b.length);*length=b.length;return STORE_OK;
}
static StoreResult write_blob(void *context,const uint8_t *data,size_t length){
    auto &b=*static_cast<Backend *>(context);
    if(b.bad_write)return STORE_COMMIT_UNKNOWN;
    assert(length<=sizeof(b.bytes));
    std::memcpy(b.bytes,data,length);b.length=length;return STORE_OK;
}
static StoreBackend backend(Backend &b){return {&b,read_blob,write_blob};}

int main(){
    static Store store, unreadable, corrupt;
    Backend b{};
    uint8_t catalog[KATMED_CATALOG_BYTES]{};
    StoreRecovery recovery{};
    assert(store_open(&store,backend(b),11,catalog,75,&recovery)==STORE_OK);
    StoreDiagnostics diag{};
    assert(store_diagnostics(&store,&diag));
    assert(diag.backend_read_failures==0&&diag.decoded_load_failures==0&&
           diag.backend_commit_failures==0);
    LocalSettings active=recovery.local;
    assert(store_set_local(&store,&active,&active)==STORE_DUPLICATE);
    assert(store_diagnostics(&store,&diag)&&diag.backend_commit_failures==0);
    LocalSettings requested=active;requested.orientation=ORIENTATION_90;
    b.bad_write=true;
    assert(store_set_local(&store,&active,&requested)==STORE_COMMIT_UNKNOWN);
    assert(store_diagnostics(&store,&diag)&&diag.backend_commit_failures==1);
    assert(active.orientation==ORIENTATION_0);
    assert(store_get_status(&store,nullptr,nullptr,nullptr)==STORE_UNAVAILABLE);
    b.bad_write=false;b.bad_read=true;
    assert(store_open(&unreadable,backend(b),11,catalog,75,&recovery)==STORE_IO);
    assert(unreadable.backend_read_failures==1&&!store_diagnostics(&unreadable,&diag));
    b.bad_read=false;b.bytes[12]^=1;
    assert(store_open(&corrupt,backend(b),11,catalog,75,&recovery)==STORE_CORRUPT);
    assert(corrupt.decoded_load_failures==1&&!store_diagnostics(&corrupt,&diag));
    std::puts("store backend/load counters passed");
}
