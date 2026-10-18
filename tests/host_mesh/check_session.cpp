#include "katmed_mesh_session.h"
#include <cassert>
#include <cstring>
#include <cstdio>

int main(){
    uint8_t local[16],peer[16],fresh[16],old_echo[16]{};
    for(unsigned i=0;i<16;++i){local[i]=uint8_t(i+1);peer[i]=uint8_t(i+40);fresh[i]=uint8_t(i+80);}
    KatmedMeshSessions session{};
    assert(katmed_mesh_sessions_init(&session,local));
    assert(!katmed_mesh_sessions_accept_hello(&session,42,peer,old_echo));
    assert(!katmed_mesh_sessions_check(&session,42,peer,1,false));
    assert(katmed_mesh_sessions_accept_hello(&session,42,peer,local));
    assert(!katmed_mesh_sessions_nonce_changed(&session,42,peer));
    assert(katmed_mesh_sessions_nonce_changed(&session,42,fresh));
    assert(katmed_mesh_sessions_check(&session,42,peer,1,true));
    assert(!katmed_mesh_sessions_check(&session,42,peer,1,true));
    assert(katmed_mesh_sessions_check(&session,42,peer,2,true));
    assert(!katmed_mesh_sessions_check(&session,42,peer,1,true));
    assert(!katmed_mesh_sessions_check(&session,42,fresh,3,true));
    /* A new boot's local challenge invalidates every recorded STATE/TIME/ACK
     * even if the old group HMAC and old sender epoch are still valid. */
    uint8_t new_local[16];std::memcpy(new_local,fresh,16);
    assert(katmed_mesh_sessions_init(&session,new_local));
    assert(!katmed_mesh_sessions_check(&session,42,peer,999,true));
    assert(!katmed_mesh_sessions_accept_hello(&session,42,peer,local));
    assert(!katmed_mesh_sessions_check(&session,42,peer,1000,true));
    assert(katmed_mesh_sessions_accept_hello(&session,42,peer,new_local));
    assert(katmed_mesh_sessions_check(&session,42,peer,1001,true));
    katmed_mesh_sessions_forget(&session,42);
    assert(!katmed_mesh_sessions_check(&session,42,peer,1002,true));
    std::puts("mesh fresh-session replay gate OK");
}
