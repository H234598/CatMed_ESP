#include "katmed_names_idf.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>
#include <cerrno>
static std::map<std::string,std::vector<uint8_t>> durable,staged;
static int commit_fail,set_error,file_fault,read_fail;
static size_t fs_free=4*1024*1024;
extern "C" esp_err_t esp_littlefs_info(const char *,size_t *total,size_t *used){*total=4*1024*1024;*used=*total-fs_free;return ESP_OK;}
extern "C" esp_err_t katmed_nvs_init(void){return ESP_OK;}
extern "C" esp_err_t nvs_get_stats(const char *,nvs_stats_t *s){*s={12,400,630,1};return ESP_OK;}
extern "C" ssize_t __real_write(int,const void *,size_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_write(int fd,const void *p,size_t n){
    if(file_fault==1){file_fault=0;errno=ENOSPC;return -1;}
    if(file_fault==2){file_fault=0;return __real_write(fd,p,n/2);}
    return __real_write(fd,p,n);
}
extern "C" int __wrap_fsync(int fd){if(file_fault==3){file_fault=0;errno=EIO;return -1;}return __real_fsync(fd);}
static bool unknown_applies;
extern "C" bool katmed_source_writer_enter(){return true;}
extern "C" void katmed_source_writer_leave(){}
extern "C" esp_err_t nvs_get_blob(nvs_handle_t,const char *key,void *out,size_t *n){if(read_fail&&!strcmp(key,"names_ctl")){--read_fail;return 99;}auto i=durable.find(key);if(i==durable.end())return ESP_ERR_NVS_NOT_FOUND;if(!out){*n=i->second.size();return ESP_OK;}if(*n<i->second.size())return 99;memcpy(out,i->second.data(),i->second.size());*n=i->second.size();return ESP_OK;}
extern "C" esp_err_t nvs_set_blob(nvs_handle_t,const char *key,const void *bytes,size_t n){if(set_error){int e=set_error;set_error=0;return e;}staged[key]=std::vector<uint8_t>((const uint8_t*)bytes,(const uint8_t*)bytes+n);return ESP_OK;}
extern "C" esp_err_t nvs_commit(nvs_handle_t){if(commit_fail){--commit_fail;if(unknown_applies)for(auto &p:staged)durable[p.first]=p.second;staged.clear();return 99;}for(auto &p:staged)durable[p.first]=p.second;staged.clear();return ESP_OK;}
extern "C" void nvs_close(nvs_handle_t){staged.clear();}
extern "C" esp_err_t nvs_open(const char *,int,nvs_handle_t *h){*h=1;return ESP_OK;}
static const uint8_t group[16]={1},registry[16]={2};
static StoreIdfNvs nvs={1,true};
static NamesIdf idf;
static NamesStore store;
static void reopen(){assert(names_reopen(&store)==NAMES_OK);}
static void publish(){assert(names_publish(&store)==NAMES_OK);if(names_cursor(&store)>1)assert(names_confirm(&store,names_published(&store))==NAMES_OK);assert(names_finish(&store)==NAMES_OK);}
int main(){
    StoreBackend app_backend{};assert(store_idf_open_backend(&nvs,&app_backend)==STORE_OK);
    Store app{};StoreRecovery recovery{};uint8_t catalog[32]={8};
    assert(store_open(&app,app_backend,111,catalog,1,&recovery)==STORE_OK);
    assert(names_open(&store,names_idf_backend(&idf,&nvs),111,group)==NAMES_OK);
    assert(names_initialize(&store,registry)==NAMES_OK);publish();
    assert(names_reserve(&store,111,nullptr,0)==NAMES_OK);publish();
    for(uint64_t i=2;i<=1000;++i){assert(names_reserve(&store,100000+i,nullptr,0)==NAMES_OK);assert(names_cursor(&store)==i);publish();assert(names_cursor(&store)==i+1);}
    reopen();assert(names_cursor(&store)==1001);
    for(uint64_t i=2;i<=1000;++i){uint16_t confirmed=0,pending=0;assert(names_lookup(&store,100000+i,&confirmed,&pending)==NAMES_OK);assert(confirmed==i&&!pending);assert(names_reserve(&store,100000+i,nullptr,0)==NAMES_OK);assert(names_cursor(&store)==1001);}
    for(bool applies:{false,true}){
        unknown_applies=applies;commit_fail=1;
        assert(names_reserve(&store,200001+applies,nullptr,0)==NAMES_COMMIT_UNKNOWN);
        assert(store.poisoned);assert(names_publish(&store)==NAMES_COMMIT_UNKNOWN);
        reopen();
        if(applies){assert(names_phase(&store)==NAMES_LOCAL_PREPARE);assert(names_resume(&store)==NAMES_OK);}
        else assert(names_reserve(&store,200001+applies,nullptr,0)==NAMES_OK);
        publish();reopen();
    }
    assert(names_cursor(&store)==1003);
    uint32_t cursor=1003;
    for(int fault:{1,2,3}){
        file_fault=fault;
        assert(names_reserve(&store,300000+fault,nullptr,0)==(fault==1?NAMES_FULL:NAMES_IO));
        reopen();assert(names_cursor(&store)==cursor);assert(names_phase(&store)==NAMES_LOCAL_PREPARE);
        assert(names_resume(&store)==NAMES_OK);publish();++cursor;
    }
    set_error=ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    assert(names_reserve(&store,400000,nullptr,0)==NAMES_FULL);
    reopen();assert(names_cursor(&store)==cursor);
    // Both actual durable outcomes at publication and final-IDLE commit.
    for(bool applies:{false,true}){
        assert(names_reserve(&store,500000+applies,nullptr,0)==NAMES_OK);
        unknown_applies=applies;commit_fail=1;
        assert(names_publish(&store)==NAMES_COMMIT_UNKNOWN);
        reopen();assert(names_phase(&store)==(applies?NAMES_FINALIZING:NAMES_PREPARED));
        assert(names_publish(&store)==NAMES_OK);
        assert(names_confirm(&store,names_published(&store))==NAMES_OK);
        commit_fail=1;assert(names_finish(&store)==NAMES_COMMIT_UNKNOWN);
        reopen();assert(names_phase(&store)==(applies?NAMES_IDLE:NAMES_FINALIZING));
        assert(names_finish(&store)==NAMES_OK);++cursor;
    }
    assert(names_cursor(&store)==cursor);
    char name[32];bool assigned=false,uncertain=true;names_visible(&store,name,&assigned,&uncertain);assert(!strcmp(name,"KatmedESP-001")&&assigned&&!uncertain);
    fs_free=32768;
    assert(names_reserve(&store,600000,nullptr,0)==NAMES_FULL);
    fs_free=4*1024*1024;reopen();assert(names_resume(&store)==NAMES_OK);publish();
    auto ctl=durable["names_ctl"],view=durable["names_view"];
    StoreUsbStatusInfo info{};char reply[128];
    assert(store_usb_handle_line(&app,"KM1 RESET CONFIG\n",17,&info,reply,sizeof(reply))==STORE_OK);
    assert(durable["names_ctl"]==ctl&&durable["names_view"]==view);reopen();
    names_visible(&store,name,&assigned,&uncertain);assert(assigned&&!uncertain&&!strcmp(name,"KatmedESP-001"));
    read_fail=1;NamesStore retry{};assert(names_open(&retry,store.backend,111,group)==NAMES_IO);
    assert(names_reopen(&retry)==NAMES_OK);names_visible(&retry,name,&assigned,&uncertain);assert(assigned&&!uncertain&&!strcmp(name,"KatmedESP-001"));
    names_idf_close(&idf);assert(names_idf_clear_files());assert(names_idf_clear_files());
    for(int fault:{1,2,3}){
        durable.erase("names_ctl");durable.erase("names_view");staged.clear();
        NamesStore initial{};assert(names_open(&initial,names_idf_backend(&idf,&nvs),111,group)==NAMES_OK);
        file_fault=fault;assert(names_initialize(&initial,registry)==(fault==1?NAMES_FULL:NAMES_IO));
        assert(names_reopen(&initial)==NAMES_OK&&!initial.fresh&&names_phase(&initial)==NAMES_LOCAL_PREPARE);
        assert(names_resume(&initial)==NAMES_OK);assert(names_publish(&initial)==NAMES_OK);assert(names_finish(&initial)==NAMES_OK);
        assert(names_reserve(&initial,111,nullptr,0)==NAMES_OK);assert(names_publish(&initial)==NAMES_OK);assert(names_confirm(&initial,names_published(&initial))==NAMES_OK);assert(names_finish(&initial)==NAMES_OK);
        names_idf_close(&idf);assert(names_idf_clear_files());
    }
    for(bool applies:{false,true}){
        durable.erase("names_ctl");durable.erase("names_view");staged.clear();NamesStore initial{};
        assert(names_open(&initial,names_idf_backend(&idf,&nvs),111,group)==NAMES_OK);
        assert(names_initialize(&initial,registry)==NAMES_OK);assert(names_publish(&initial)==NAMES_OK);assert(names_finish(&initial)==NAMES_OK);
        assert(names_reserve(&initial,111,nullptr,0)==NAMES_OK);assert(names_publish(&initial)==NAMES_OK);
        commit_fail=1;unknown_applies=applies;assert(names_confirm(&initial,names_published(&initial))==NAMES_COMMIT_UNKNOWN);
        assert(names_reopen(&initial)==NAMES_OK);names_visible(&initial,name,&assigned,&uncertain);assert(assigned==applies);
        assert(names_confirm(&initial,names_published(&initial))==NAMES_OK);assert(names_finish(&initial)==NAMES_OK);
        names_visible(&initial,name,&assigned,&uncertain);assert(assigned&&!uncertain&&!strcmp(name,"KatmedESP-001"));
        names_idf_close(&idf);assert(names_idf_clear_files());
    }
    puts("real C adapter: first header ENOSPC/short-write/sync recovery, initial view CommitUnknown old/new, config reset retention and idempotent file reset passed");
    printf("names storage: natural 1000, durable reopen, response retry, FS-reserve/ENOSPC/partial-write/sync/NVS-set and both reservation/publication/IDLE CommitUnknown outcomes passed; mesh/session proof NOT covered\n");
}
