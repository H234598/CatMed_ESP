#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main prior_engine_main
#include "host_mesh/check_engine.cpp"
#undef main
#pragma GCC diagnostic pop
extern "C" void refinement_actual_tick(Store *,AppState *,const ClockSample *);
int main()
{
    const uint8_t catalog[KATMED_CATALOG_BYTES]={1,2,3};
    StoreRecovery recovered{};StoreBackend backend{&memory,read_blob,write_blob};
    assert(store_open(&store,backend,11,catalog,72,&recovered)==STORE_OK);
    for(unsigned i=0;i<16;++i)provision.group[i]=uint8_t(i+1);
    for(unsigned i=0;i<32;++i)provision.secret[i]=uint8_t(i+30);
    std::memcpy(provision.ssid,"testssid",8);provision.ssid_length=8;
    std::memcpy(provision.password,"password",8);provision.password_length=8;
    std::memcpy(provision.time_source,"pool.ntp.org",12);provision.time_source_length=12;
    provision.configured=true;store.record.provision=provision;
    std::memcpy(store.record.bound_group,provision.group,16);
    store.record.group_origin=STORE_GROUP_ORIGIN_FIRST_BOUND;
    assert(app_init(&state,&recovered.shared,&now,recovered.boot_identity,72,catalog)==APP_OK);
    KatmedMeshServices services{&store,&state,nullptr,shared_result,utc_received};
    assert(katmed_mesh_start(&provision,&services)==MESH_ACCEPTED);
    host_station_present=true;
    level=ROOT;katmed_mesh_poll(&now);
    char published_name[32]{};bool assigned=false,uncertain=true;
    katmed_mesh_device_name(published_name,sizeof(published_name),&assigned,&uncertain);
    assert(assigned&&!uncertain&&!std::strcmp(published_name,"KatmedESP-001")&&
           !std::strcmp(host_hostname,published_name));
    const unsigned before_dismiss=memory.commits;
    AppEffects dismiss_effects{};
    assert(state.start_reason==START_BOOT&&state.alarm_active);
    assert(app_dismiss_boot_alarm(&state,&now,&dismiss_effects)==APP_OK);
    assert(!state.alarm_active&&!state.has_last_confirmation&&
           !dismiss_effects.persistence_required&&!dismiss_effects.broadcast_required&&
           memory.commits==before_dismiss);
    for(AppEventKind kind:{EVENT_SET_IMAGE_INTERVAL,EVENT_SET_ROTATION_MODE}) {
        EventId id{};assert(store_next_event_id(&store,&id)==STORE_OK);
        AppEvent event{};event.kind=kind;event.id=id;event.sender=id.device;
        event.round=state.round;event.parent_revision=state.revision;event.occurred_at=now;
        if(kind==EVENT_SET_IMAGE_INTERVAL)event.value.interval_seconds=1;
        if(kind==EVENT_SET_ROTATION_MODE)event.value.rotation=ROTATION_RANDOM;
        AppEffects effects{};
        assert(katmed_mesh_submit_shared(&event,&now,&effects)==MESH_ACCEPTED);
    }
    assert(katmed_mesh_is_coordinator()&&!katmed_mesh_connected());
    const auto previous=state.image_id;
    advance_now(2000);
    level=2;unsigned before=memory.commits;
    refinement_actual_tick(&store,&state,&now);
    assert(state.image_due_pending&&state.image_id==previous&&memory.commits==before);
    level=0;refinement_actual_tick(&store,&state,&now);
    assert(state.image_id==previous&&memory.commits==before);
    level=ROOT;store.record.provision.configured=false;
    refinement_actual_tick(&store,&state,&now);
    assert(state.image_id==previous&&memory.commits==before);
    store.record.provision.configured=true;store.record.provision.group[0]^=1;
    refinement_actual_tick(&store,&state,&now);
    assert(state.image_id==previous&&memory.commits==before);
    store.record.provision.group[0]^=1;store.record.provision.secret[0]^=1;
    refinement_actual_tick(&store,&state,&now);
    assert(state.image_id==previous&&memory.commits==before);
    store.record.provision.secret[0]^=1;
    for(unsigned i=0;i<150;++i) {
        auto prior=state.image_id;refinement_actual_tick(&store,&state,&now);
        if(!(state.image_id<72&&state.image_id!=prior&&!state.image_due_pending))
            std::fprintf(stderr,"tick iteration=%u pending=%d changed=%d coordinator=%d storeHealthy=%d alarm=%d count=%u mismatch=%d error=%d\n",
                i,state.image_due_pending,state.image_id!=prior,katmed_mesh_is_coordinator(),
                store_get_status(&store,nullptr,nullptr,nullptr)==STORE_OK,state.alarm_active,state.image_count,state.catalog_mismatch,(int)store.last_error);
        assert(state.image_id<72&&state.image_id!=prior&&!state.image_due_pending);
        assert(store.record.history.image_id==state.image_id);
        advance_now(now.monotonic_ms+1000);
    }
    Store durable{};StoreRecovery recovery{};
    assert(store_open(&durable,backend,11,catalog,72,&recovery)==STORE_OK);
    assert(durable.record.history.image_id==state.image_id);
    auto stable=state.image_id;memory.fail=true;
    refinement_actual_tick(&store,&state,&now);
    assert(state.image_id==stable&&state.image_due_pending);
    assert(store_get_status(&store,nullptr,nullptr,nullptr)!=STORE_OK);
    for(unsigned i=0;i<100;++i)refinement_actual_tick(&store,&state,&now);
    assert(state.image_id==stable);
    puts("actual Main/Core/Store/Mesh: solo-root150durable, follower/offline/foreign/secret and Store-failure guards passed");
}
