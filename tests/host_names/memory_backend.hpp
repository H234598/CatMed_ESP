#ifndef HOST_NAMES_MEMORY_BACKEND_HPP
#define HOST_NAMES_MEMORY_BACKEND_HPP
#include "katmed_names.h"
#include <vector>
#include <cstring>
struct NamesMemory {
    std::vector<uint8_t> keys[2],files[2];
    bool exists[2]{};
    bool unknown=false,applies=false;
    unsigned commits=0,unknown_on_commit=0;
    static StoreResult read(void *c,NamesKey k,uint8_t *p,size_t *n){auto &v=((NamesMemory*)c)->keys[k];if(v.empty())return STORE_NOT_FOUND;if(*n<v.size())return STORE_CORRUPT;std::memcpy(p,v.data(),v.size());*n=v.size();return STORE_OK;}
    static StoreResult commit(void *c,NamesKey k,const uint8_t *p,size_t n){auto &m=*(NamesMemory*)c;++m.commits;const bool fault=m.unknown||(m.unknown_on_commit&&m.commits==m.unknown_on_commit);if(!fault||m.applies)m.keys[k].assign(p,p+n);return fault?STORE_COMMIT_UNKNOWN:STORE_OK;}
    static StoreResult size(void *c,unsigned b,uint32_t *n){auto &m=*(NamesMemory*)c;if(!m.exists[b])return STORE_NOT_FOUND;*n=uint32_t(m.files[b].size());return STORE_OK;}
    static StoreResult rd(void *c,unsigned b,uint32_t at,uint8_t *p,size_t n){auto &v=((NamesMemory*)c)->files[b];if(at>v.size()||n>v.size()-at)return STORE_CORRUPT;std::memcpy(p,v.data()+at,n);return STORE_OK;}
    static StoreResult wr(void *c,unsigned b,uint32_t at,const uint8_t *p,size_t n){auto &m=*(NamesMemory*)c;m.exists[b]=true;auto &v=m.files[b];if(v.size()<at+n)v.resize(at+n);std::memcpy(v.data()+at,p,n);return STORE_OK;}
    static StoreResult truncate(void *c,unsigned b,uint32_t n){auto &m=*(NamesMemory*)c;m.exists[b]=true;m.files[b].resize(n);return STORE_OK;}
    static StoreResult sync(void *,unsigned){return STORE_OK;}
    NamesBackend backend(){return {this,read,commit,size,rd,wr,truncate,sync,nullptr};}
};
#endif
