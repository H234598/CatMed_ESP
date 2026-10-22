#include "memory_backend.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <zlib.h>
extern "C" unsigned char *SHA256(const unsigned char *,size_t,unsigned char *);
static const uint8_t group[16]={1},registry[16]={2};
static void put(uint8_t *p,uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)p[i]=uint8_t(v>>(8*i));}
static std::vector<uint8_t> descriptor(const std::vector<uint64_t> &ids,uint64_t writer,uint64_t serial){
    std::vector<uint8_t> file(64+8*ids.size()),d(80);memcpy(file.data(),"KMN2",4);put(file.data()+4,2,2);put(file.data()+6,64,2);
    memcpy(file.data()+8,registry,16);memcpy(file.data()+24,group,16);put(file.data()+40,writer,8);put(file.data()+48,serial,8);put(file.data()+56,8,2);put(file.data()+60,crc32(0,file.data(),60),4);
    uint32_t assigned=0;for(size_t i=0;i<ids.size();++i){put(file.data()+64+8*i,ids[i],8);if(ids[i])++assigned;}
    put(d.data(),writer,8);put(d.data()+8,serial,8);put(d.data()+16,writer,8);put(d.data()+24,serial,8);
    SHA256(file.data(),file.size(),d.data()+32);put(d.data()+64,ids.size(),4);put(d.data()+68,ids.size()+1,4);put(d.data()+72,assigned,4);put(d.data()+76,1,2);return d;
}
static void pages(NamesStore &s,const std::vector<uint64_t> &ids){for(size_t at=0;at<ids.size();){size_t n=ids.size()-at;if(n>89)n=89;assert(names_receive_page(&s,uint32_t(at),ids.data()+at,n)==NAMES_OK);at+=n;}}
static void publish(NamesStore &s){assert(names_publish(&s)==NAMES_OK);assert(names_confirm(&s,names_published(&s))==NAMES_OK);assert(names_finish(&s)==NAMES_OK);}
static void seed(NamesStore &s,NamesMemory &m,uint64_t self,const std::vector<uint64_t> &ids,uint64_t writer=99,uint64_t serial=1){auto d=descriptor(ids,writer,serial);assert(names_open(&s,m.backend(),self,group)==NAMES_OK);assert(names_receive_begin(&s,registry,d.data(),false,99,1,nullptr,0,0,0)==NAMES_OK);pages(s,ids);publish(s);}
int main(){
    for(bool applies:{false,true}){
        NamesMemory b;NamesStore s{};assert(names_open(&s,b.backend(),11,group)==NAMES_OK);
        auto target=descriptor({11},99,1);b.unknown=true;b.applies=applies;
        assert(names_receive_begin(&s,registry,target.data(),false,99,1,nullptr,0,11,1)==NAMES_COMMIT_UNKNOWN);
        assert(!b.exists[0]&&!b.exists[1]); // persistent registry binding precedes the very first file write
        b.unknown=false;NamesStore reopened{};assert(names_open(&reopened,b.backend(),11,group)==NAMES_OK);
        if(applies){assert(!reopened.fresh&&names_phase(&reopened)==NAMES_RX_REPLACE);assert(names_resume(&reopened)==NAMES_OK);}
        else {assert(reopened.fresh);assert(names_receive_begin(&reopened,registry,target.data(),false,99,1,nullptr,0,11,1)==NAMES_OK);}
        uint64_t own=11;assert(names_receive_page(&reopened,0,&own,1)==NAMES_OK);
        const unsigned before=b.commits;assert(names_receive_page(&reopened,0,&own,1)==NAMES_OK);assert(b.commits==before); // duplicate complete page
        char name[32];bool assigned=false;names_visible(&reopened,name,&assigned,nullptr);assert(!assigned&&!strcmp(name,"KatmedESP"));
        assert(names_publish(&reopened)==NAMES_OK);b.unknown=true;
        assert(names_confirm(&reopened,names_published(&reopened))==NAMES_COMMIT_UNKNOWN);
        names_visible(&reopened,name,&assigned,nullptr);assert(!assigned); // RAM never claims the uncertain view
        b.unknown=false;NamesStore confirmed{};assert(names_open(&confirmed,b.backend(),11,group)==NAMES_OK);
        names_visible(&confirmed,name,&assigned,nullptr);assert(assigned==applies);
        assert(names_confirm(&confirmed,names_published(&confirmed))==NAMES_OK);assert(names_finish(&confirmed)==NAMES_OK);
        names_visible(&confirmed,name,&assigned,nullptr);assert(assigned&&!strcmp(name,"KatmedESP-001"));
    }
    puts("first registry intent and first projection: both CommitUnknown outcomes, reopen and no premature visibility passed");
    {NamesStore s{};NamesMemory b;seed(s,b,11,{11,22,0,44});const std::vector<uint64_t> source={11,22,33,44,55};auto sd=descriptor(source,88,1);NamesMerge job{};
     assert(names_merge_begin(&s,&job,55,sd.data())==NAMES_OK);assert(names_merge_begin(&s,&job,44,sd.data())==NAMES_BUSY&&job.source_device==55);unsigned commits=b.commits;
     assert(names_merge_page(&s,&job,0,source.data(),source.size(),nullptr,0)==NAMES_OK);
     // First pass ends by atomically installing the intent, then opens pass two.
     assert(job.active&&job.writing&&names_phase(&s)==NAMES_LOCAL_PREPARE);assert(b.commits>commits);
     assert(names_merge_page(&s,&job,0,source.data(),3,nullptr,0)==NAMES_OK);
     NamesStore reopened{};assert(names_open(&reopened,b.backend(),11,group)==NAMES_OK);names_merge_cancel(&job);
     assert(names_merge_resume(&reopened,&job)==NAMES_OK);
     assert(job.source_device==55&&!memcmp(job.source,sd.data(),80));
     assert(names_merge_page(&reopened,&job,0,source.data(),2,nullptr,0)==NAMES_OK);assert(reopened.ctl[52]==3); // replay never decreases checkpoint
     assert(names_merge_page(&reopened,&job,2,source.data()+2,3,nullptr,0)==NAMES_OK);assert(names_phase(&reopened)==NAMES_PREPARED);
     auto original=b; // byte backend fork models the two actual NVS commit outcomes, not a second allocator
     for(bool applies:{false,true}){NamesMemory branch=original;NamesStore active{};assert(names_open(&active,branch.backend(),11,group)==NAMES_OK);branch.unknown=true;branch.applies=applies;assert(names_publish(&active)==NAMES_COMMIT_UNKNOWN);branch.unknown=false;NamesStore chosen{};assert(names_open(&chosen,branch.backend(),11,group)==NAMES_OK);assert(names_cursor(&chosen)==(applies?6u:5u));assert((chosen.ctl[51]&1)==(applies?1:0));publish(chosen);branch.files[0].resize(20000,42);assert(names_open(&chosen,branch.backend(),11,group)==NAMES_OK&&names_cursor(&chosen)==6);}
     publish(reopened);
     uint16_t i=0,r=0;assert(names_lookup(&reopened,33,&i,&r)==NAMES_OK&&i==3);assert(names_lookup(&reopened,55,&i,&r)==NAMES_OK&&i==5);
     assert(names_cursor(&reopened)==6);names_merge_cancel(&job);
     puts("nonprefix inner-hole MERGE, exact source and restart/source replay passed");}
    for(bool applies:{false,true}){
     NamesStore s{};NamesMemory b;seed(s,b,11,{11,22});auto old=descriptor({11,22,33,44},99,1); // same immutable physical F; new logical G below
     put(old.data()+8,2,8);assert(names_receive_begin(&s,registry,old.data(),true,99,2,nullptr,0,44,4)==NAMES_OK);
     uint64_t id=33;assert(names_receive_page(&s,2,&id,1)==NAMES_OK);
     const auto pbank=b.files[0];auto ctl=b.keys[0];auto all_a=b.files[0],all_b=b.files[1];unsigned count=b.commits;
     auto target=descriptor({11,22,33,44,55},77,5);NamesRetarget verify{};
     assert(names_retarget_begin(&s,&verify,target.data())==NAMES_OK);const uint64_t ids[]={11,22,33,44,55};
     assert(names_retarget_page(&s,&verify,0,ids,5)==NAMES_OK);assert(verify.complete);
     assert(b.commits==count&&b.keys[0]==ctl&&b.files[0]==all_a&&b.files[1]==all_b); // read-only pass, including append-shared P
     b.unknown=true;b.applies=applies;
     assert(names_retarget_accept(&s,&verify,77,3,nullptr,0)==NAMES_COMMIT_UNKNOWN);
     b.unknown=false;NamesStore reopened{};assert(names_open(&reopened,b.backend(),11,group)==NAMES_OK);
     assert(b.files[0]==pbank); // no reset/truncate of shared P bank on either unknown outcome
     names_retarget_cancel(&verify);
     if(!applies){assert(!memcmp(names_target(&reopened),old.data(),80));assert(names_retarget_begin(&reopened,&verify,target.data())==NAMES_OK);assert(names_retarget_page(&reopened,&verify,0,ids,5)==NAMES_OK);assert(names_retarget_accept(&reopened,&verify,77,3,nullptr,0)==NAMES_OK);}
     else {assert(!memcmp(names_target(&reopened),target.data(),80));assert(names_resume(&reopened)==NAMES_OK);}
     assert(names_receive_page(&reopened,0,ids,5)==NAMES_OK);publish(reopened);assert(names_cursor(&reopened)==6);
     uint16_t i=0,r=0;assert(names_lookup(&reopened,33,&i,&r)==NAMES_OK&&i==3);names_retarget_cancel(&verify);
    }
    puts("write-free retarget, shared append-bank protection and both intent CommitUnknown outcomes passed");
    {NamesStore s{};NamesMemory b;seed(s,b,11,{11,22});auto sd=descriptor({11,33},88,1);NamesMerge job{};assert(names_merge_begin(&s,&job,33,sd.data())==NAMES_OK);auto ctl=b.keys[0],file=b.files[0];uint64_t collision[]={11,33};assert(names_merge_page(&s,&job,0,collision,2,nullptr,0)==NAMES_CONFLICT);assert(b.keys[0]==ctl&&b.files[0]==file);names_merge_cancel(&job);}
    {
     NamesStore s{};NamesMemory b;seed(s,b,11,{11});
     const auto old=b.keys[0];const auto view=b.keys[1];
     const std::vector<uint64_t> duplicate={11,22,22};
     auto target=descriptor(duplicate,77,2);
     assert(names_receive_begin(&s,registry,target.data(),false,77,2,nullptr,0,0,0)==NAMES_OK);
     assert(names_receive_page(&s,0,duplicate.data(),duplicate.size())==NAMES_CONFLICT);
     assert(b.keys[1]==view&&names_cursor(&s)==2);
     NamesStore reopened{};assert(names_open(&reopened,b.backend(),11,group)==NAMES_OK);
     assert(names_cursor(&reopened)==2&&reopened.visible_index==1);
     assert(b.keys[0]!=old&&names_phase(&reopened)==NAMES_RX_REPLACE);
     puts("valid-hash duplicate T rejected before PREPARED; P and View survive reopening");
    }
    {
     NamesStore s{};NamesMemory b;seed(s,b,11,{11});
     std::vector<uint64_t> boundary(1000);
     for(size_t i=0;i<boundary.size();++i)boundary[i]=i+11;
     boundary.back()=boundary[15]; // crosses the 16-entry selection boundary
     const auto target=descriptor(boundary,77,4);
     assert(names_receive_begin(&s,registry,target.data(),false,77,2,nullptr,0,0,0)==NAMES_OK);
     for(size_t at=0;at<boundary.size();){size_t n=boundary.size()-at;if(n>89)n=89;
         const auto r=names_receive_page(&s,uint32_t(at),boundary.data()+at,n);
         assert(r==(at+n==boundary.size()?NAMES_CONFLICT:NAMES_OK));at+=n;}
     NamesStore reopened{};assert(names_open(&reopened,b.backend(),11,group)==NAMES_OK);
     assert(reopened.visible_index==1&&names_cursor(&reopened)==2);
     puts("1000-slot duplicate across selection boundary rejected with valid full digest");
    }
    {
     NamesStore s{};NamesMemory b;seed(s,b,11,{11});
     const auto ctl=b.keys[0],file=b.files[0];
     const auto source=descriptor({11,22},88,std::numeric_limits<uint64_t>::max()-1);
     NamesMerge job{};
     assert(names_merge_begin(&s,&job,22,source.data())==NAMES_INDEX_EXHAUSTED);
     assert(b.keys[0]==ctl&&b.files[0]==file&&names_phase(&s)==NAMES_IDLE);
     assert(names_reopen(&s)==NAMES_OK&&s.visible_index==1);
     names_merge_cancel(&job);
     puts("UINT64_MAX-1 merge offer exhausts serial before durable intent or file mutation");
    }
    {
     NamesStore s{};NamesMemory b;seed(s,b,11,{11});
     const auto original_view=b.keys[1];
     const std::vector<uint64_t> duplicate={11,22,22};
     const auto source=descriptor(duplicate,88,3);
     NamesMerge job{};assert(names_merge_begin(&s,&job,22,source.data())==NAMES_OK);
     assert(names_merge_page(&s,&job,0,duplicate.data(),duplicate.size(),nullptr,0)==NAMES_OK);
     assert(names_merge_page(&s,&job,0,duplicate.data(),duplicate.size(),nullptr,0)==NAMES_CONFLICT);
     names_merge_cancel(&job);
     NamesStore reopened{};assert(names_open(&reopened,b.backend(),11,group)==NAMES_OK);
     assert(names_cursor(&reopened)==2&&reopened.visible_index==1&&b.keys[1]==original_view);
     puts("valid-hash duplicate merge source rejected before PREPARED; P and View survive");
    }
    {
     NamesStore s{};NamesMemory b;seed(s,b,11,{11});
     assert(names_reserve(&s,22,nullptr,0)==NAMES_OK);
     const auto target=descriptor({11,22,33},77,std::numeric_limits<uint64_t>::max());
     NamesRetarget verify{};const uint64_t ids[]={11,22,33};
     assert(names_retarget_begin(&s,&verify,target.data())==NAMES_OK);
     assert(names_retarget_page(&s,&verify,0,ids,3)==NAMES_OK&&verify.complete);
     const auto ctl=b.keys[0],file=b.files[0],view=b.keys[1];const auto commits=b.commits;
     assert(names_retarget_accept(&s,&verify,77,3,nullptr,0)==NAMES_INDEX_EXHAUSTED);
     assert(b.commits==commits&&b.keys[0]==ctl&&b.files[0]==file&&b.keys[1]==view);
     assert(names_reopen(&s)==NAMES_OK&&s.visible_index==1);
     names_retarget_cancel(&verify);
     puts("retarget maximum serial rejected before intent or file mutation");
    }
    {NamesStore s{};NamesMemory b;std::vector<uint64_t> ids(65535);for(size_t i=0;i<ids.size();++i)ids[i]=i+1;seed(s,b,65535,ids);assert(names_cursor(&s)==65536);auto ctl=b.keys[0];assert(names_reserve(&s,999999,nullptr,0)==NAMES_INDEX_EXHAUSTED);assert(b.keys[0]==ctl);char value[32];names_visible(&s,value,nullptr,nullptr);assert(!strcmp(value,"KatmedESP-65535"));puts("bounded seeded format/range probe: 65535 name, cursor65536 explicit exhaustion; NOT a natural join run");}    {NamesMemory orphan;orphan.exists[0]=true;orphan.files[0]={1};NamesStore s{};assert(names_open(&s,orphan.backend(),11,group)==NAMES_CORRUPT&&orphan.commits==0);}
    {NamesMemory b;NamesStore s{};seed(s,b,11,{11,22});auto ctl=b.keys[0];b.files[0][0]^=1;unsigned commits=b.commits;assert(names_open(&s,b.backend(),11,group)==NAMES_CORRUPT);assert(b.commits==commits&&b.keys[0]==ctl);}
    {NamesMemory b;NamesStore s{};seed(s,b,11,{11,22});auto target=descriptor({11,22,33},77,2);assert(names_receive_begin(&s,registry,target.data(),false,77,2,nullptr,0,33,3)==NAMES_OK);uint64_t first=11;assert(names_receive_page(&s,0,&first,1)==NAMES_OK);b.files[1][64]^=1;assert(names_open(&s,b.backend(),11,group)==NAMES_CORRUPT);char name[32];bool assigned=false,uncertain=false;names_visible(&s,name,&assigned,&uncertain);assert(assigned&&uncertain&&!strcmp(name,"KatmedESP-001"));}
    puts("exact old/new bank anchor selection, larger unreferenced file ignored, orphan/corrupt objects failclosed passed");

}
