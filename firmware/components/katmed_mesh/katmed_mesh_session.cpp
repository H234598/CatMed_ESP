#include "katmed_mesh_session.h"
#include <cstring>

namespace {
bool any(const uint8_t *p,size_t n){unsigned bits=0;for(size_t i=0;i<n;++i)bits|=p[i];return bits!=0;}
bool equal(const uint8_t *a,const uint8_t *b,size_t n){unsigned bits=0;for(size_t i=0;i<n;++i)bits|=a[i]^b[i];return bits==0;}
}
extern "C" bool katmed_mesh_sessions_init(KatmedMeshSessions *s,const uint8_t nonce[16]){
    if(!s||!nonce||!any(nonce,16))return false;
    *s=KatmedMeshSessions{};std::memcpy(s->local_nonce,nonce,16);return true;
}
extern "C" bool katmed_mesh_sessions_accept_hello(KatmedMeshSessions *s,
    uint64_t device,const uint8_t nonce[16],const uint8_t echo[16]){
    if(!s||!nonce||!echo||device==0||!any(nonce,16)||
       !equal(echo,s->local_nonce,16))return false;
    KatmedMeshPeer *slot=nullptr;
    for(auto &p:s->peers)if(p.live&&p.device==device){slot=&p;break;}
    if(!slot)for(auto &p:s->peers)if(!p.live){slot=&p;break;}
    if(!slot)return false;
    if(!slot->live||!equal(slot->nonce,nonce,16)){
        *slot=KatmedMeshPeer{};slot->device=device;
        std::memcpy(slot->nonce,nonce,16);slot->live=true;
    }
    return true;
}
extern "C" bool katmed_mesh_sessions_nonce_changed(const KatmedMeshSessions *s,
    uint64_t device,const uint8_t nonce[16]){
    if(!s||!nonce||device==0)return false;
    for(const auto &p:s->peers)
        if(p.live&&p.device==device)return !equal(p.nonce,nonce,16);
    return false;
}
extern "C" bool katmed_mesh_sessions_check(KatmedMeshSessions *s,uint64_t device,
    const uint8_t nonce[16],uint64_t counter,bool advance){
    if(!s||!nonce||device==0||counter==0)return false;
    for(auto &p:s->peers){
        if(!p.live||p.device!=device||!equal(p.nonce,nonce,16))continue;
        if(counter<=p.highest_counter)return false;
        if(advance)p.highest_counter=counter;
        return true;
    }
    return false;
}
extern "C" void katmed_mesh_sessions_forget(KatmedMeshSessions *s,uint64_t device){
    if(!s)return;
    for(auto &p:s->peers)if(p.live&&p.device==device)p=KatmedMeshPeer{};
}
