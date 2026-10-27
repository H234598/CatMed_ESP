#define main mesh_engine_existing_main
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include "../host_mesh/check_engine.cpp"
#pragma GCC diagnostic pop
#undef main

int main(){
    uint8_t catalog[32]{};catalog[0]=43;
    StoreRecovery recovery{};
    StoreBackend backend{&memory,read_blob,write_blob};
    assert(store_open(&store,backend,11,catalog,75,&recovery)==STORE_OK);
    for(unsigned i=0;i<16;++i)provision.group[i]=uint8_t(i+1);
    for(unsigned i=0;i<32;++i)provision.secret[i]=uint8_t(i+30);
    std::memcpy(provision.ssid,"testssid",8);provision.ssid_length=8;
    std::memcpy(provision.password,"password",8);provision.password_length=8;
    std::memcpy(provision.time_source,"pool.ntp.org",12);provision.time_source_length=12;
    provision.configured=true;store.record.provision=provision;
    std::memcpy(store.record.bound_group,provision.group,16);
    assert(app_init(&state,&recovery.shared,&now,recovery.boot_identity,75,catalog)==APP_OK);
    KatmedMeshServices services{&store,&state,nullptr,shared_result,utc_received,nullptr,nullptr};
    assert(katmed_mesh_start(&provision,&services)==MESH_ACCEPTED);
    level=ROOT;katmed_mesh_poll(&now);
    auto challenge=latest_kind(MESH_HELLO);
    std::memcpy(current_local_nonce,challenge.session_nonce,16);
    auto hello=peer_frame(MESH_HELLO,9);
    assert(katmed_mesh_encode_hello(challenge.session_nonce,catalog,"d941fae",0,
        hello.payload,sizeof(hello.payload),&hello.payload_length));
    inject(hello);katmed_mesh_poll(&now);
    assert(katmed_mesh_connected());
    const auto recorded_hello=hello;
    hello.session_counter=10;
    assert(katmed_mesh_encode_hello(challenge.session_nonce,catalog,"d941fae",2,
        hello.payload,sizeof(hello.payload),&hello.payload_length));
    inject(hello);katmed_mesh_poll(&now);

    advance_now(10000);
    AppEffects effects{};
    assert(app_tick(&state,&now,&effects)==APP_OK);
    StorePendingConfirm pending{};
    pending.id={99,5,11};pending.sender=99;pending.round=state.round;
    pending.parent_revision=state.revision;
    pending.occurred_at={8000,55,now.utc_ms-2000,true,CLOCK_HOLDOVER};
    auto confirm=peer_frame(MESH_CONFIRM,11);
    assert(katmed_mesh_encode_confirm(&pending,2000,confirm.payload,
        sizeof(confirm.payload),&confirm.payload_length));
    inject(confirm);katmed_mesh_poll(&now);
    assert(!state.has_last_confirmation);
    advance_now(16000);katmed_mesh_poll(&now);
    assert(!state.has_last_confirmation);

    /* A captured, lower-counter HELLO must not erase the peer's announcement
     * that a second pending confirmation is still outstanding. */
    inject(recorded_hello);katmed_mesh_poll(&now);
    assert(!state.has_last_confirmation);
    std::puts("old signed HELLO cannot reduce pending advertisement");
}
