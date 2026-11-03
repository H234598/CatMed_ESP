#define main prior_submit_test
#include "host_store/check_main_submit.c"
#undef main
#include "check_ui_refinement_main.h"

void refinement_init(const uint8_t *catalog, uint32_t count)
{
    committed_length=0; fail_commit=false; writes=0; fake_microseconds=1000000;
    memset(&runtime,0,sizeof(runtime));
    StoreRecovery recovery={0};
    StoreBackend backend={NULL,test_read,test_write};
    assert(store_open(&runtime.store,backend,UINT64_C(0xAC276E46BC2C),catalog,count,&recovery)==STORE_OK);
    runtime.store_ready=true; runtime.boot=recovery.boot_identity;
    runtime.boot_domain=new_boot_domain(runtime.boot); runtime.local=recovery.local;
    katmed_time_init(&runtime.time,runtime.boot_domain);
    assert(katmed_time_accept_utc(&runtime.time,1000,INT64_C(1792393200000),CLOCK_SYNCHRONIZED));
    ClockSample now=clock_sample(runtime.boot_domain);
    assert(app_init(&runtime.state,&recovery.shared,&now,runtime.boot,count,catalog)==APP_OK);
    runtime.mesh_started=true; fake_mesh_coordinator=true; fake_mesh_connected=false;
    fake_mesh_result=MESH_ACCEPTED;
    assert(submit_due_confirm()==UI_SUBMIT_PENDING);
    publish_ui_owner(&runtime);
}
/* This single-threaded fixture executes the owner synchronously. Publish its
 * committed model before the real UI callback consumes read_ui_model. */
static UiSubmitResult refinement_submit(void *context,const UiRequest *request)
{
    UiSubmitResult result=submit_ui(context,request);
    publish_ui_owner(context);
    return result;
}
UiServices refinement_services(void) { return (UiServices){.context=&runtime,.submit=refinement_submit,.read_model=read_ui_model}; }
void refinement_wifi_prepare(void)
{
    StoreWifiProfileUpdate profile={.priority=1,.ssid_length=17,
        .credential_action=STORE_WIFI_CREDENTIAL_OPEN};
    memcpy(profile.ssid,"synthetic-network",profile.ssid_length);
    assert(store_wifi_profiles_save_atomic(&runtime.store,&profile,1)==STORE_OK);
    assert(katmed_config_bridge_start());
}
void refinement_language_commit(uint8_t language)
{
    const KatmedConfigRequest request={.operation=KATMED_CONFIG_LANGUAGE_SAVE,
                                       .language=language};
    KatmedConfigResponse response={0};
    config_execute(&runtime,&request,&response);
    assert(response.status==KATMED_CONFIG_OK && response.language==language);
}
KatmedWebWifiServices refinement_wifi_services(void)
{
    return (KatmedWebWifiServices){.context=&runtime,
        .configured=web_wifi_configured,
        .recovery_available=web_wifi_recovery_available,
        .profile_limit=web_wifi_profile_limit,
        .list_profiles=web_wifi_list_profiles,
        .save_profiles_atomic=web_wifi_save_profiles,
        .delete_profile_atomic=web_wifi_delete_profile,
        .schedule_reconnect=web_wifi_schedule_reconnect,
#ifdef KATMED_WEB_WIFI_RENDER_SNAPSHOT
        .render_snapshot=web_wifi_render_snapshot,
#endif
    };
}
void refinement_render_owner(void)
{
    ClockSample now=refinement_clock();
#ifdef KATMED_WEB_WIFI_RENDER_SNAPSHOT
    publish_render_owner(&runtime);
#else
    publish_ui_owner(&runtime);
#endif
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_RENDER_CALL);
    assert(render_ui(&runtime.state,&runtime.local,&now,NULL,false,true));
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_OTHER);
#ifdef KATMED_WEB_WIFI_RENDER_SNAPSHOT
    finish_render_owner();
#endif
}
void refinement_poll_owner(void) { katmed_config_poll(config_execute,&runtime); }
void refinement_wifi_snapshot_contract(void)
{
#ifdef KATMED_WEB_WIFI_RENDER_SNAPSHOT
    KatmedWebWifiRenderSnapshot view={0};
    assert(!web_wifi_render_snapshot(&runtime,&view));
    publish_render_owner(&runtime);
    assert(web_wifi_render_snapshot(&runtime,&view) && view.count==1 &&
           !strcmp(view.profiles[0].ssid,"synthetic-network"));
    const uint32_t id=view.profiles[0].id;
    finish_render_owner();
    assert(!web_wifi_render_snapshot(&runtime,&view));

    KatmedConfigRequest rename={.operation=KATMED_CONFIG_WIFI_SAVE};
    rename.wifi.count=1;
    rename.wifi.profiles[0].id=id;
    rename.wifi.profiles[0].priority=1;
    rename.wifi.profiles[0].credential_action=KATMED_WEB_WIFI_CREDENTIAL_KEEP;
    strcpy(rename.wifi.profiles[0].ssid,"synthetic-renamed");
    KatmedConfigResponse response={0};
    config_execute(&runtime,&rename,&response);
    assert(response.status==KATMED_CONFIG_OK);
    publish_render_owner(&runtime);
    assert(web_wifi_render_snapshot(&runtime,&view) && view.count==1 &&
           !strcmp(view.profiles[0].ssid,"synthetic-renamed") &&
           strcmp(view.profiles[0].ssid,"synthetic-network"));
    finish_render_owner();

    const KatmedConfigRequest remove={.operation=KATMED_CONFIG_WIFI_DELETE,.wifi.id=id};
    response=(KatmedConfigResponse){0};
    config_execute(&runtime,&remove,&response);
    assert(response.status==KATMED_CONFIG_OK);
    publish_render_owner(&runtime);
    assert(web_wifi_render_snapshot(&runtime,&view) && view.count==0);
    finish_render_owner();

    runtime.store.record.wifi_profile_count=KATMED_STORE_WIFI_PROFILE_MAX+1;
    publish_render_owner(&runtime);
    assert(!web_wifi_render_snapshot(&runtime,&view));
    finish_render_owner();
    runtime.store.record.wifi_profile_count=0;

    fail_commit=true;
    rename.wifi.profiles[0].id=0;
    rename.wifi.profiles[0].credential_action=KATMED_WEB_WIFI_CREDENTIAL_OPEN;
    response=(KatmedConfigResponse){0};
    config_execute(&runtime,&rename,&response);
    fail_commit=false;
    assert(response.status==KATMED_CONFIG_COMMIT_UNKNOWN && runtime.store.uncertain);
    publish_render_owner(&runtime);
    assert(!web_wifi_render_snapshot(&runtime,&view));
    finish_render_owner();
#endif
}
const AppState *refinement_state(void) { return &runtime.state; }
const LocalSettings *refinement_local(void) { return &runtime.local; }
ClockSample refinement_clock(void) { return clock_sample(runtime.boot_domain); }
void refinement_advance(uint32_t ms) { fake_microseconds+=(int64_t)ms*1000; }
unsigned refinement_writes(void) { return writes; }
void refinement_fail_commit(bool fail) { fail_commit=fail; }
AppResult refinement_image(uint32_t id)
{
    EventId next;
    assert(store_next_event_id(&runtime.store,&next)==STORE_OK);
    ClockSample now=refinement_clock();
    AppEvent event={.kind=EVENT_SET_IMAGE,.id=next,.sender=next.device,
        .round=runtime.state.round,.parent_revision=runtime.state.revision,
        .occurred_at=now,.coordinator_accepted=true,.value.image_id=id};
    AppEffects effects; AppResult result;
    (void)store_apply_event(&runtime.store,&runtime.state,&event,&now,false,&effects,&result);
    return result;
}

void refinement_index_contract(void)
{
    uint8_t current[KATMED_CATALOG_BYTES];
    memcpy(current,runtime.state.catalog_id,sizeof(current));
    const uint32_t count=runtime.state.image_count;
    const uint32_t old_ids[]={72,73,74};
    for(unsigned i=0;i<3;++i) {
        uint8_t old[KATMED_CATALOG_BYTES];memcpy(old,current,sizeof(old));old[0]^=1;
        refinement_init(old,75);
        assert(refinement_image(old_ids[i])==APP_OK);
        assert(runtime.store.record.history.image_id==old_ids[i]);
        const SharedSettings settings=runtime.store.record.shared;
        StoreRecovery recovery={0};StoreBackend backend={NULL,test_read,test_write};
        assert(store_open(&runtime.store,backend,UINT64_C(0xAC276E46BC2C),current,count,&recovery)==STORE_OK);
        assert(recovery.catalog_mismatch);
        runtime.boot=recovery.boot_identity;
        runtime.boot_domain=new_boot_domain(runtime.boot);
        katmed_time_init(&runtime.time,runtime.boot_domain);
        ClockSample now=refinement_clock();
        assert(app_init(&runtime.state,&recovery.shared,&now,recovery.boot_identity,count,current)==APP_OK);
        assert(runtime.state.image_id==0&&runtime.state.image_count==count);
        assert(settings.medication_seconds==runtime.state.settings.medication_seconds &&
               settings.image_seconds==runtime.state.settings.image_seconds &&
               settings.rotation==runtime.state.settings.rotation);
        assert(submit_due_confirm()==UI_SUBMIT_PENDING);
        assert(refinement_image(old_ids[i])==APP_INVALID);
        assert(refinement_image(UINT16_MAX)==APP_INVALID);
        assert(refinement_image(UINT32_MAX)==APP_INVALID);
        assert(runtime.state.image_id<count);
    }
    refinement_init(current,0);
    assert(runtime.state.image_id==0&&runtime.state.catalog_mismatch);
    assert(refinement_image(0)==APP_INVALID);
    puts("real Store old75 indices72/73/74 reboot to current catalog, invalid65535/max and empty catalog passed");
}

void refinement_tick_contract(void)
{
    StoreProvision *p=&runtime.store.record.provision;
    for(unsigned i=0;i<16;++i)p->group[i]=(uint8_t)(i+1);
    for(unsigned i=0;i<32;++i)p->secret[i]=(uint8_t)(i+30);
    memcpy(p->ssid,"testssid",8);p->ssid_length=8;
    memcpy(p->password,"password",8);p->password_length=8;
    memcpy(p->time_source,"pool.ntp.org",12);p->time_source_length=12;
    p->configured=true;
    memcpy(runtime.store.record.bound_group,p->group,16);
    runtime.store.record.group_origin=STORE_GROUP_ORIGIN_FIRST_BOUND;
    UiRequest interval={.kind=UI_REQUEST_SET_IMAGE_SECONDS,.value.seconds=1};
    UiRequest random={.kind=UI_REQUEST_SET_ROTATION_MODE,.value.rotation=ROTATION_RANDOM};
    assert(submit_ui(&runtime,&interval)==UI_SUBMIT_COMMITTED);
    assert(submit_ui(&runtime,&random)==UI_SUBMIT_COMMITTED);
    const uint32_t previous=runtime.state.image_id;
    refinement_advance(1000);ClockSample now=refinement_clock();
    tick_runtime(&runtime,&now);
    printf("actual Main tick: pending=%d changed=%d durable=%d\n",
        runtime.state.image_due_pending,runtime.state.image_id!=previous,
        runtime.store.record.history.image_id==runtime.state.image_id);
    fflush(stdout);
    assert(!runtime.state.image_due_pending&&runtime.state.image_id!=previous);
    const uint32_t medication=runtime.state.settings.medication_seconds;
    const int64_t deadline=runtime.state.deadline.effective_utc.ms;
    for(unsigned i=0;i<150;++i) {
        uint32_t before=runtime.state.image_id;
        refinement_advance(1000);now=refinement_clock();tick_runtime(&runtime,&now);
        assert(runtime.state.image_id<runtime.state.image_count&&runtime.state.image_id!=before);
        assert(!runtime.state.image_due_pending);
        assert(runtime.store.record.history.image_id==runtime.state.image_id);
    }
    assert(runtime.state.settings.medication_seconds==medication&&runtime.state.deadline.effective_utc.ms==deadline);
    const uint32_t before=runtime.state.image_id;
    fake_mesh_coordinator=false;refinement_advance(1000);now=refinement_clock();
    unsigned prior=writes;tick_runtime(&runtime,&now);
    assert(runtime.state.image_id==before&&runtime.state.image_due_pending&&writes==prior);
    fake_mesh_coordinator=true;runtime.mesh_started=false;tick_runtime(&runtime,&now);
    assert(runtime.state.image_id==before&&writes==prior);
    runtime.mesh_started=true;runtime.store.record.provision.configured=false;
    tick_runtime(&runtime,&now);assert(runtime.state.image_id==before&&writes==prior);
    runtime.store.record.provision.configured=true;
    UiRequest mode={.kind=UI_REQUEST_SET_ROTATION_MODE,.value.rotation=ROTATION_NAME_ORDER};
    assert(submit_ui(&runtime,&mode)==UI_SUBMIT_COMMITTED);
    uint32_t start=runtime.state.image_id;
    refinement_advance(1000);now=refinement_clock();tick_runtime(&runtime,&now);
    assert(runtime.state.image_id==(start+1)%runtime.state.image_count);
    mode.value.rotation=ROTATION_OFF;assert(submit_ui(&runtime,&mode)==UI_SUBMIT_COMMITTED);
    start=runtime.state.image_id;prior=writes;
    refinement_advance(1000);now=refinement_clock();tick_runtime(&runtime,&now);
    assert(runtime.state.image_id==start&&writes==prior);
    mode.value.rotation=ROTATION_RANDOM;assert(submit_ui(&runtime,&mode)==UI_SUBMIT_COMMITTED);
    fail_commit=true;prior=writes;
    refinement_advance(1000);now=refinement_clock();tick_runtime(&runtime,&now);
    assert(runtime.state.image_id==start&&writes==prior+1);
    for(unsigned i=0;i<100;++i)tick_runtime(&runtime,&now);
    assert(runtime.state.image_id==start&&writes==prior+1);
    puts("actual Main random150 durable bounds, name-order, off, unavailable and fail-closed/no-spam passed");
}
