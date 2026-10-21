#include "katmed_store_idf.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <cstdio>
std::vector<unsigned char> blob;int stat_error=0,set_error=0,commit_error=0;
unsigned commits=0,stats=0,sets=0;bool opened=false;
extern "C" esp_err_t nvs_flash_init(){return ESP_OK;}
extern "C" esp_err_t katmed_nvs_init(void){return ESP_OK;}
extern "C" bool katmed_source_writer_enter(void){return true;}
extern "C" void katmed_source_writer_leave(void){}
extern "C" esp_err_t nvs_open(const char *s,int mode,nvs_handle_t *h){assert(!std::strcmp(s,"katmed")&&mode==NVS_READWRITE);*h=1;opened=true;return ESP_OK;}
extern "C" void nvs_close(nvs_handle_t h){assert(h==1);opened=false;}
extern "C" esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n){
    assert(h==1&&!std::strcmp(key,"mesh_history"));if(blob.empty())return ESP_ERR_NVS_NOT_FOUND;
    if(out){assert(*n>=blob.size());std::memcpy(out,blob.data(),blob.size());}*n=blob.size();return ESP_OK;
}
extern "C" esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *b,size_t n){
    assert(h==1&&!std::strcmp(key,"mesh_history")&&n==2152);++sets;
    if(set_error)return set_error;
    auto *p=static_cast<const unsigned char *>(b);blob.assign(p,p+n);return ESP_OK;
}
extern "C" esp_err_t nvs_commit(nvs_handle_t h){assert(h==1);++commits;return commit_error;}
extern "C" esp_err_t nvs_get_stats(const char *p,nvs_stats_t *s){assert(!p);++stats;*s={12,400,630,1};return stat_error;}
int main(){
    StoreIdfNvs nvs{};StoreBackend state{};assert(store_idf_open_backend(&nvs,&state)==STORE_OK&&opened);
    StoreBackend history=store_idf_history_backend(&nvs);unsigned char bytes[2152]{};size_t n=sizeof(bytes);
    assert(history.read(history.context,bytes,&n)==STORE_NOT_FOUND);
    assert(history.write_commit(history.context,bytes,sizeof(bytes))==STORE_OK&&stats==1&&commits==1);
    n=sizeof(bytes);assert(history.read(history.context,bytes,&n)==STORE_OK&&n==2152);
    set_error=ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    assert(history.write_commit(history.context,bytes,sizeof(bytes))==STORE_FULL&&commits==1);
    set_error=99;assert(history.write_commit(history.context,bytes,sizeof(bytes))==STORE_IO&&commits==1);
    set_error=0;commit_error=99;assert(history.write_commit(history.context,bytes,sizeof(bytes))==STORE_COMMIT_UNKNOWN);
    commit_error=0;assert(history.write_commit(history.context,bytes,sizeof(bytes))==STORE_OK);
    stat_error=99;unsigned before=sets;assert(history.write_commit(history.context,bytes,sizeof(bytes))==STORE_IO&&sets==before);
    store_idf_close_backend(&nvs);assert(!opened&&history.write_commit(history.context,bytes,sizeof(bytes))==STORE_UNAVAILABLE);
    std::puts("actual NVS history backend key/stats/read/set/commit failures OK; no erase API");
}
