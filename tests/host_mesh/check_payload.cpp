#include "katmed_mesh_payload.h"
#include "katmed_core.h"
#include <cassert>
#include <cstring>
#include <cstdio>

int main(){
    uint8_t catalog[32]{};catalog[0]=44;
    uint8_t hello[100]{},echo[16]{},parsed_echo[16]{};
    uint16_t hello_len=0;bool mismatch=false;uint8_t pending_count=0;
    echo[0]=99;
    assert(katmed_mesh_encode_hello(echo,catalog,"d941fae",8,hello,sizeof(hello),&hello_len));
    assert(hello_len==61);
    assert(katmed_mesh_decode_hello(hello,hello_len,catalog,"d941fae",parsed_echo,&mismatch,&pending_count));
    assert(!mismatch&&parsed_echo[0]==99&&pending_count==8);
    assert(!katmed_mesh_decode_hello(hello,hello_len,catalog,"other",parsed_echo,&mismatch,&pending_count));
    uint8_t foreign_catalog[32]{};foreign_catalog[0]=45;
    assert(katmed_mesh_decode_hello(hello,hello_len,foreign_catalog,"d941fae",parsed_echo,&mismatch,&pending_count));
    assert(mismatch);
    hello[51]=2;assert(!katmed_mesh_decode_hello(hello,hello_len,catalog,"d941fae",parsed_echo,&mismatch,&pending_count));
    hello[51]=KATMED_SCHEMA_VERSION;
    hello[53]=0x01;assert(!katmed_mesh_decode_hello(hello,hello_len,catalog,"d941fae",parsed_echo,&mismatch,&pending_count));
    hello[53]='d';hello[52]=0;
    assert(!katmed_mesh_decode_hello(hello,hello_len,catalog,"d941fae",parsed_echo,&mismatch,&pending_count));
    hello[52]=32;
    assert(!katmed_mesh_decode_hello(hello,hello_len,catalog,"d941fae",parsed_echo,&mismatch,&pending_count));
    hello[52]=7;hello[60]=9;
    assert(!katmed_mesh_decode_hello(hello,hello_len,catalog,"d941fae",parsed_echo,&mismatch,&pending_count));
    char long_version[33];std::memset(long_version,'a',32);long_version[32]=0;
    assert(!katmed_mesh_encode_hello(echo,catalog,long_version,0,hello,sizeof(hello),&hello_len));
    assert(!katmed_mesh_encode_hello(echo,catalog,"",0,hello,sizeof(hello),&hello_len));
    assert(!katmed_mesh_encode_hello(echo,catalog,"bad\x01",0,hello,sizeof(hello),&hello_len));
    ClockSample now{5000000000ULL,81,1760460000000LL,true,CLOCK_SYNCHRONIZED};
    EventId boot{11,81,1};
    SharedSettings settings=app_default_settings();
    AppState state{};
    assert(app_init(&state,&settings,&now,boot,75,catalog)==APP_OK);
    StateSnapshot snap{};
    assert(app_make_snapshot(&state,&now,&snap)==APP_OK);
    uint8_t bytes[KATMED_MESH_PAYLOAD_MAX];uint16_t len=0;
    assert(katmed_mesh_encode_snapshot(&snap,bytes,sizeof(bytes),&len));
    assert(len>800&&len<=KATMED_MESH_PAYLOAD_MAX);
    StateSnapshot output{};
    assert(katmed_mesh_decode_snapshot(bytes,len,&output));
    assert(output.state.round.ordinal==1&&output.state.image_count==75&&
           output.captured_at.monotonic_ms==now.monotonic_ms&&
           std::memcmp(output.catalog_id,catalog,32)==0);
    for(unsigned i=0;i<len;++i)assert(!katmed_mesh_decode_snapshot(bytes,i,&output));
    bytes[0]=2;assert(!katmed_mesh_decode_snapshot(bytes,len,&output));bytes[0]=0;
    StorePendingConfirm pending{};
    pending.id={11,81,2};pending.round=state.round;pending.occurred_at=now;
    assert(katmed_mesh_encode_confirm(&pending,1234,bytes,sizeof(bytes),&len));
    assert(len==66);
    StorePendingConfirm confirm{};
    uint64_t age=0;
    assert(katmed_mesh_decode_confirm(bytes,len,&confirm,&age));
    assert(confirm.round.ordinal==1&&confirm.occurred_at.utc_valid&&age==1234);
    assert(!katmed_mesh_decode_confirm(bytes,len-1,&confirm,&age));
    /* The actual reducer, not a copied reducer in the test, decides whether
     * a foreign snapshot can be adopted and keeps a cold boot alarm when its
     * causal time cannot be proved. */
    AppEvent medication{};medication.kind=EVENT_CONFIRM;
    medication.id={11,81,2};medication.sender=11;medication.round=state.round;
    medication.parent_revision=state.revision;medication.occurred_at=now;
    medication.coordinator_accepted=true;
    AppEffects effects{};
    assert(app_apply_event(&state,&medication,&now,&effects)==APP_OK);
    assert(app_make_snapshot(&state,&now,&snap)==APP_OK);
    assert(katmed_mesh_encode_snapshot(&snap,bytes,sizeof(bytes),&len));
    assert(katmed_mesh_decode_snapshot(bytes,len,&output));
    ClockSample peer_now{5000001000ULL,91,now.utc_ms+1000,true,CLOCK_HOLDOVER};
    AppState peer{};EventId peer_boot{17,91,1};
    assert(app_init(&peer,&settings,&peer_now,peer_boot,75,catalog)==APP_OK);
    AppEvent adopt{};adopt.kind=EVENT_ADOPT_SNAPSHOT;adopt.id={11,81,3};
    adopt.sender=11;adopt.round=peer.round;adopt.parent_revision=peer.revision;
    adopt.occurred_at=peer_now;adopt.coordinator_accepted=true;
    adopt.value.snapshot=&output;
    assert(app_apply_event(&peer,&adopt,&peer_now,&effects)==APP_OK);
    assert(!peer.alarm_active&&peer.round.ordinal==2);
    ClockSample no_utc{5000001000ULL,93,0,false,CLOCK_UNAVAILABLE};
    AppState isolated{};EventId isolated_boot{19,93,1};
    assert(app_init(&isolated,&settings,&no_utc,isolated_boot,75,catalog)==APP_OK);
    adopt.id.sequence=4;adopt.round=isolated.round;
    adopt.parent_revision=isolated.revision;adopt.occurred_at=no_utc;
    assert(app_apply_event(&isolated,&adopt,&no_utc,&effects)==APP_TIME_UNAVAILABLE);
    assert(isolated.alarm_active&&isolated.round.ordinal==1);
    std::puts("mesh payload bounds OK");
}
