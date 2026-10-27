#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <sys/time.h>
static unsigned fake_wall_sets;
static int fake_settimeofday(const struct timeval *value, const struct timezone *zone) {
    (void)value;(void)zone;++fake_wall_sets;return 0;
}
#define settimeofday fake_settimeofday
#define KATMED_TEST_WIFI_CONFIG 1
#define KATMED_FULL_STACK 1
#define KATMED_WITH_CORE 1
#include "../../firmware/main/main.c"

static unsigned setup_config_calls;
int katmed_test_wifi_config(int iface, wifi_config_t *config)
{
    assert(iface==WIFI_IF_AP);
    assert(config->ap.ssid_len==strlen("Katmed_ESP_SETUP"));
    assert(memcmp(config->ap.ssid,"Katmed_ESP_SETUP",config->ap.ssid_len)==0);
    assert(config->ap.authmode==WIFI_AUTH_WPA2_PSK);
    assert(config->ap.max_connection==1 && config->ap.channel==1);
    assert(strlen((const char *)config->ap.password)==16);
    ++setup_config_calls;return ESP_OK;
}

static uint8_t committed[KATMED_STORE_BLOB_MAX];
static size_t committed_length;
static unsigned writes;
static bool fail_commit;
static bool probe_display_write;
static unsigned display_write_probes;
static int64_t fake_microseconds;
static Orientation board_orientation;
static Orientation sensed_orientation;
static bool sensed_valid;
static bool fake_mesh_connected;
static bool fake_mesh_coordinator;
static KatmedMeshResult fake_mesh_result = MESH_UNAVAILABLE;
static unsigned fake_mesh_start_calls, fake_mesh_history_calls, fake_web_policy_calls;
static bool fake_web_enabled;
static unsigned fake_shared_calls;
static unsigned fake_mesh_notifications;
static AppEvent fake_shared_event;
static uint32_t fake_random_counter=UINT32_C(0x12345678);
static bool reset_marker;
static uint8_t applied_brightness;
static unsigned brightness_calls;
static bool fail_brightness;
BoardResult board_set_brightness(uint8_t value){
    assert(value>=1 && value<=100);++brightness_calls;
    if(fail_brightness)return BOARD_IO_ERROR;
    applied_brightness=value;return BOARD_OK;
}
BoardResult board_set_backlight(bool awake){(void)awake;return BOARD_OK;}
bool lvgl_port_lock(uint32_t timeout){(void)timeout;return true;}
void lvgl_port_unlock(void){}
/* Standalone Main bridge only: native UI integration links its strong implementation. */
__attribute__((weak)) void ui_set_display_awake(bool awake){(void)awake;}

static bool boot_mac_error,boot_mac_zero,boot_zero_epoch;
static unsigned boot_legacy_calls;
static const uint8_t boot_factory_mac[6]={0x02,0x13,0x24,0x35,0x46,0x57};
int esp_efuse_mac_get_default(uint8_t *mac)
{
    ++boot_legacy_calls;if(boot_mac_error)return -1;
#if CONFIG_IDF_TARGET_ESP32C6
    /* Existing SDK C6 contract: extension insertion writes eight bytes. */
    const uint8_t eui[8]={0x02,0x13,0x24,0xff,0xfe,0x35,0x46,0x57};
    struct {uint8_t six[6],sentinel[2];} contract={{0},{0xa5,0x5a}};
    memcpy(&contract,eui,sizeof(eui));
    assert(contract.sentinel[0]!=0xa5 && contract.sentinel[1]!=0x5a);
    /* Expose the SDK's eight-byte result/six-byte sentinel violation without
     * invoking undefined host writes into the actual producer's mac[6]. */
    memcpy(mac,contract.six,6);
#else
    memcpy(mac,boot_factory_mac,sizeof(boot_factory_mac));
#endif
    return ESP_OK;
}
int esp_read_mac(uint8_t *mac,esp_mac_type_t type)
{
    assert(type==ESP_MAC_EFUSE_FACTORY);if(boot_mac_error)return -1;
    if(boot_mac_zero)memset(mac,0,6);else memcpy(mac,boot_factory_mac,6);
    return ESP_OK;
}
uint32_t esp_random(void) { return boot_zero_epoch ? 0 : ++fake_random_counter; }
void esp_fill_random(void *out,size_t length)
{
    unsigned char *bytes=out;
    for (size_t i=0;i<length;i++) bytes[i]=(unsigned char)(i+1);
}
bool katmed_reset_idf_prepare(void) { reset_marker=true; return true; }
bool katmed_reset_idf_cancel_preparation(void) { reset_marker=false; return true; }
bool katmed_reset_idf_take_preparation(bool *prepared)
{
    *prepared=reset_marker;
    reset_marker=false;
    return true;
}

static StoreResult test_read(void *context, uint8_t *out, size_t *length)
{
    (void)context;
    if (!committed_length) return STORE_NOT_FOUND;
    if (*length < committed_length) return STORE_CORRUPT;
    memcpy(out, committed, committed_length);
    *length = committed_length;
    return STORE_OK;
}
static StoreResult test_write(void *context, const uint8_t *data, size_t length)
{
    (void)context;
    if(probe_display_write){
        KatmedDisplaySettings snapshot;KatmedNightCopyStatus night;
        display_settings(&runtime,&snapshot);night_copy_status(&runtime,&night);
        assert(snapshot.brightness==100 && !night.local_saved);
        UiRequest other={.kind=UI_REQUEST_COPY_NIGHT};
        assert(submit_ui_dispatch(&runtime,&other)==UI_SUBMIT_UNAVAILABLE);
        ++display_write_probes;
    }
    ++writes;
    if (fail_commit) return STORE_COMMIT_UNKNOWN;
    memcpy(committed, data, length);
    committed_length = length;
    return STORE_OK;
}
int64_t esp_timer_get_time(void) { return fake_microseconds; }
BoardResult board_set_orientation(Orientation o) { board_orientation=o; return BOARD_OK; }
bool board_auto_orientation(Orientation *o) {*o=sensed_orientation;return sensed_valid;}
KatmedMeshResult katmed_mesh_copy_night(uint64_t now) {(void)now;return MESH_UNAVAILABLE;}
void katmed_mesh_night_status(KatmedNightCopyStatus *out) {*out=(KatmedNightCopyStatus){0};}
bool katmed_mesh_connected(void) { return fake_mesh_connected; }
bool katmed_mesh_is_coordinator(void) { return fake_mesh_coordinator; }
StoreBackend store_idf_history_backend(StoreIdfNvs *nvs) {
    (void)nvs;return (StoreBackend){0};
}
void katmed_mesh_history_backend(StoreBackend backend) {
    (void)backend;++fake_mesh_history_calls;
}
KatmedMeshResult katmed_mesh_start(const StoreProvision *provision,
                                   const KatmedMeshServices *services) {
    ++fake_mesh_start_calls;
    assert(provision && services && services->store && services->state);
    assert(provision->configured && provision->network_mode!=STORE_NETWORK_OFFLINE);
    assert(memcmp(provision->group,services->store->record.provision.group,16)==0 &&
           memcmp(provision->secret,services->store->record.provision.secret,32)==0);
    return fake_mesh_result;
}
void katmed_web_set_enabled(bool enabled) {
    ++fake_web_policy_calls;fake_web_enabled=enabled;
}
void katmed_mesh_device_name(char *out,size_t capacity,bool *assigned,bool *uncertain) {
    (void)out;(void)capacity;*assigned=false;*uncertain=false;
}
bool katmed_mesh_radio_channel(uint8_t *channel) {*channel=0;return false;}
void katmed_mesh_counts(KatmedMeshCounts *out) {*out=(KatmedMeshCounts){0};}
uint32_t katmed_time_idf_sync_server_ipv4(void) {return 0;}
void katmed_mesh_notify_local_commit(void) { ++fake_mesh_notifications; }
KatmedMeshResult katmed_mesh_submit_shared(const AppEvent *event,
                                            const ClockSample *now,
                                            AppEffects *effects) {
    ++fake_shared_calls;
    fake_shared_event = *event;
    if (fake_mesh_result == MESH_ACCEPTED && fake_mesh_coordinator) {
        AppEvent accepted = *event;
        accepted.coordinator_accepted = true;
        AppResult core_result;
        if (store_apply_event(&runtime.store, &runtime.state, &accepted,
                              now, false, effects, &core_result) != STORE_OK)
            return MESH_STORE_ERROR;
    }
    return fake_mesh_result;
}

static void initialize_runtime(void)
{
    const StoreBackend backend = {NULL,test_read,test_write};
    uint8_t catalog[KATMED_CATALOG_BYTES] = {1,2,3};
    StoreRecovery recovery = {0};
    memset(&runtime,0,sizeof(runtime));
    assert(store_open(&runtime.store,backend,UINT64_C(0xAC276E46BC2C),catalog,75,&recovery)==STORE_OK);
    runtime.store_ready=true;
    runtime.boot=recovery.boot_identity;
    runtime.boot_domain=new_boot_domain(runtime.boot);
    runtime.local=recovery.local;
    ClockSample now=clock_sample(runtime.boot_domain);
    assert(app_init(&runtime.state,&recovery.shared,&now,runtime.boot,75,catalog)==APP_OK);
    runtime.state.divergent=store_durable_divergent(&runtime.store);
}

/* Existing tests of genuine due confirmations must no longer use START_BOOT
 * as their fixture: that notice has no medication deadline. */
static UiSubmitResult submit_due_confirm(void)
{
    if (runtime.state.start_reason == START_BOOT) {
        const ClockSample due = clock_sample(runtime.boot_domain);
        runtime.state.start_reason = START_SETTINGS;
        runtime.state.alarm_active = true;
        runtime.state.clock = due;
        runtime.state.countdown_started = due;
        runtime.state.deadline.regular_monotonic_ms = due.monotonic_ms;
        runtime.state.deadline.effective_monotonic_ms = due.monotonic_ms;
        if (due.utc_valid) {
            runtime.state.deadline.regular_utc = (UtcInstant){true,due.utc_ms};
            runtime.state.deadline.effective_utc = (UtcInstant){true,due.utc_ms};
        }
    }
    const UiRequest confirm = {.kind=UI_REQUEST_CONFIRM};
    return submit_ui(&runtime,&confirm);
}

static void test_runtime_setup_and_emergency(void) {
    committed_length=0;fake_microseconds=1000000;fake_wall_sets=0;
    initialize_runtime();katmed_time_init(&runtime.time,runtime.boot_domain);
    UiRequest dismiss={.kind=UI_REQUEST_DISMISS_BOOT};
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_DISMISSED);
    const unsigned first_ap=setup_config_calls, first_mesh=fake_mesh_start_calls;
    assert(start_runtime_network(&runtime) && runtime.setup_ap &&
           setup_config_calls==first_ap+1 && fake_mesh_start_calls==first_mesh && fake_web_enabled);
    KatmedConfigRequest request={.operation=KATMED_CONFIG_SETUP_GET};KatmedConfigResponse response={0};
    unsigned before=writes;config_execute(&runtime,&request,&response);
    assert(response.status==KATMED_CONFIG_OK && !response.setup.configured &&
           response.setup.setup_open && writes==before);
    assert(setup_exit_after_physical_boot(&runtime.store,true,false,ESP_RST_POWERON)==STORE_OK &&
           !runtime.store.record.provision.configured && writes==before);
    request.operation=KATMED_CONFIG_SETUP_EXIT;
    snprintf(request.revision,sizeof(request.revision),"%s",response.snapshot.revision);
    config_execute(&runtime,&request,&response);
    assert(response.status==KATMED_CONFIG_OK && runtime.store.record.setup_dismissed &&
           !runtime.store.record.setup_requested && runtime.reboot_required && writes==before+1);
    runtime.setup_ap=false;
    runtime.reboot_required=false;
    const unsigned offline_ap=setup_config_calls, offline_mesh=fake_mesh_start_calls;
    assert(!start_runtime_network(&runtime) && !runtime.setup_ap && !fake_web_enabled &&
           setup_config_calls==offline_ap && fake_mesh_start_calls==offline_mesh);
    UiNetworkStatus offline_status={0};network_status(&runtime,&offline_status);
    assert(offline_status.offline_setup_exited && offline_status.manual_clock_required &&
           !offline_status.configured && !offline_status.mesh_active);
    assert(store_request_setup(&runtime.store)==STORE_OK &&
           runtime.store.record.setup_requested && !runtime.store.record.setup_dismissed);
    assert(setup_exit_after_physical_boot(&runtime.store,true,false,ESP_RST_SW)==STORE_OK &&
           runtime.store.record.setup_requested);
    assert(start_runtime_network(&runtime) && runtime.setup_ap && fake_web_enabled);
    assert(setup_exit_after_physical_boot(&runtime.store,true,true,ESP_RST_EXT)==STORE_OK &&
           runtime.store.record.setup_requested); /* physical reset warning has priority */
    assert(setup_exit_after_physical_boot(&runtime.store,true,false,ESP_RST_EXT)==STORE_OK &&
           runtime.store.record.setup_dismissed && !runtime.store.record.setup_requested);
    runtime.setup_ap=false;
    assert(!start_runtime_network(&runtime) && !runtime.setup_ap);
    assert(store_request_setup(&runtime.store)==STORE_OK);
    assert(start_runtime_network(&runtime) && runtime.setup_ap);
    request=(KatmedConfigRequest){.operation=KATMED_CONFIG_SETUP_GET};
    config_execute(&runtime,&request,&response);
    request.operation=KATMED_CONFIG_SETUP_SAVE;
    snprintf(request.revision,sizeof(request.revision),"%s",response.snapshot.revision);
    request.setup.mode=STORE_NETWORK_OFFLINE;
    before=writes;
    snprintf(request.setup.local_datetime,sizeof(request.setup.local_datetime),"2026-03-29T02:30");
    config_execute(&runtime,&request,&response);
    assert(response.status==KATMED_CONFIG_INVALID && writes==before && !runtime.store.record.provision.configured);
    snprintf(request.setup.local_datetime,sizeof(request.setup.local_datetime),"2026-10-23T14:25");
    config_execute(&runtime,&request,&response);
    assert(response.status==KATMED_CONFIG_OK && runtime.store.record.provision.network_mode==STORE_NETWORK_OFFLINE && runtime.reboot_required);
    assert(runtime.utc_source==3 && clock_sample(runtime.boot_domain).quality==CLOCK_HOLDOVER && !time_shareable(&runtime));
    assert(!runtime.store.record.last_confirmation.valid && !runtime.store.record.has_history && runtime.store.record.pending_count==0);
    // Simulate completion of the separately scheduled restart, never a real host reset.
    runtime.reboot_required=false;config_execute(&runtime,&request,&response);
    assert(response.status==KATMED_CONFIG_STALE);
    const StoreProvision saved_profile=runtime.store.record.provision;
    UiRequest open_setup={.kind=UI_REQUEST_OPEN_SETUP};
    assert(submit_ui(&runtime,&open_setup)==UI_SUBMIT_COMMITTED &&
           runtime.store.record.setup_requested && runtime.reboot_required);
    assert(setup_exit_after_physical_boot(&runtime.store,true,false,ESP_RST_SW)==STORE_OK &&
           runtime.store.record.setup_requested); /* scheduled reboot retains setup */
    runtime.reboot_required=false; /* now running after that software reboot */
    runtime.setup_ap=true;
    request=(KatmedConfigRequest){.operation=KATMED_CONFIG_SETUP_GET};
    config_execute(&runtime,&request,&response);
    assert(response.status==KATMED_CONFIG_OK && response.setup.setup_open &&
           response.setup.configured);
    request.operation=KATMED_CONFIG_SETUP_EXIT;
    snprintf(request.revision,sizeof(request.revision),"%s",response.snapshot.revision);
    config_execute(&runtime,&request,&response);
    assert(response.status==KATMED_CONFIG_OK);
    assert(!runtime.store.record.setup_requested &&
           memcmp(&runtime.store.record.provision,&saved_profile,sizeof(saved_profile))==0);
    assert(store_request_setup(&runtime.store)==STORE_OK);
    assert(setup_exit_after_physical_boot(&runtime.store,true,true,ESP_RST_EXT)==STORE_OK &&
           runtime.store.record.setup_requested); /* reset warning remains independent */
    assert(setup_exit_after_physical_boot(&runtime.store,true,false,ESP_RST_EXT)==STORE_OK &&
           !runtime.store.record.setup_requested);
    assert(memcmp(&runtime.store.record.provision,&saved_profile,sizeof(saved_profile))==0);
    runtime.setup_ap=false;
    runtime.reboot_required=false;
    AppState state=runtime.state;StoreRecord record=runtime.store.record;before=writes;
    UiRequest emergency={.kind=UI_REQUEST_EMERGENCY};
    assert(submit_ui(&runtime,&emergency)==UI_SUBMIT_COMMITTED && runtime.emergency_button_sequence==1);
    assert(!memcmp(&state,&runtime.state,sizeof(state)) && !memcmp(&record,&runtime.store.record,sizeof(record)) && writes==before);
    request=(KatmedConfigRequest){.operation=KATMED_CONFIG_HA_GET};config_execute(&runtime,&request,&response);
    assert(response.status==KATMED_CONFIG_OK && response.ha.emergency_button_sequence==1 && !response.ha.last_dose_known && !response.ha.alarm_active);
    puts("runtime setup/manual clock/CAS and independent emergency snapshot passed");
    committed_length=0;runtime=(Runtime){0};
}

static void test_mesh_only_without_station_profile(void) {
    committed_length=0;fake_microseconds=1000000;
    initialize_runtime();katmed_time_init(&runtime.time,runtime.boot_domain);
    StoreProvision mesh={0};mesh.configured=true;mesh.network_mode=STORE_NETWORK_MESH_ONLY;
    mesh.group[0]=1;mesh.secret[0]=2; /* synthetic trusted group, no WLAN secret */
    assert(store_save_setup(&runtime.store,&mesh)==STORE_OK &&
           runtime.store.record.wifi_profile_count==0);
    const unsigned before_ap=setup_config_calls,before_mesh=fake_mesh_start_calls;
    fake_mesh_result=MESH_ACCEPTED;
    assert(!start_runtime_network(&runtime) && !runtime.setup_ap &&
           runtime.mesh_started && !fake_web_enabled &&
           setup_config_calls==before_ap && fake_mesh_start_calls==before_mesh+1 &&
           fake_mesh_history_calls>0);
    fake_mesh_result=MESH_UNAVAILABLE;
    puts("mesh-only startup: trusted group starts without station profile; no setup AP");
}

static void test_offline_exit_clock_policy(void) {
    committed_length=0;fake_microseconds=1000000;
    initialize_runtime();katmed_time_init(&runtime.time,runtime.boot_domain);
    assert(store_exit_setup(&runtime.store)==STORE_OK);
    UiNetworkStatus status={0};network_status(&runtime,&status);
    assert(status.offline_setup_exited && status.manual_clock_required);
    assert(katmed_time_accept_utc(&runtime.time,monotonic_ms(),INT64_C(1792400400000),CLOCK_HOLDOVER));
    network_status(&runtime,&status);
    assert(status.offline_setup_exited && !status.manual_clock_required);
    puts("offline exit clock: missing prompts, valid local time skips");
}

static void initialize_offline_runtime(void)
{
    committed_length=0;fail_commit=false;fake_microseconds=1000000;
    fake_mesh_coordinator=false;fake_mesh_connected=false;
    initialize_runtime();katmed_time_init(&runtime.time,runtime.boot_domain);
    const UiRequest dismiss={.kind=UI_REQUEST_DISMISS_BOOT};
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_DISMISSED);
    UiRequest images={.kind=UI_REQUEST_SET_IMAGE_SECONDS};images.value.seconds=28800;
    assert(submit_ui(&runtime,&images)==UI_SUBMIT_COMMITTED);
    StoreProvision offline={0};offline.configured=true;offline.network_mode=STORE_NETWORK_OFFLINE;
    assert(store_save_setup(&runtime.store,&offline)==STORE_OK);
    assert(accept_manual_clock(&runtime,"2026-10-23T14:25"));
    assert(runtime.utc_source==3 && !runtime.mesh_started &&
           clock_sample(runtime.boot_domain).quality==CLOCK_HOLDOVER);
}

static void test_offline_random_commits_without_mesh(void)
{
    initialize_offline_runtime();
    UiRequest rotation={.kind=UI_REQUEST_SET_ROTATION_MODE};rotation.value.rotation=ROTATION_RANDOM;
    assert(submit_ui(&runtime,&rotation)==UI_SUBMIT_COMMITTED);
    const unsigned before=writes,calls=fake_shared_calls;
    const uint32_t image=runtime.state.image_id;
    const StoreRecord original=runtime.store.record;
    fake_microseconds+=INT64_C(28800000000); /* Actual eight-hour image deadline. */
    ClockSample now=clock_sample(runtime.boot_domain);
    tick_runtime(&runtime,&now);
    assert(!runtime.state.image_due_pending && runtime.state.image_id!=image);
    assert(writes>before && fake_shared_calls==calls &&
           runtime.state.image_remaining_ms==UINT64_C(28800000));
    assert(runtime.store.record.shared.image_seconds==28800 &&
           !memcmp(&runtime.store.record.provision,&original.provision,sizeof(original.provision)) &&
           !memcmp(&runtime.store.record.last_confirmation,&original.last_confirmation,
                   sizeof(original.last_confirmation)) &&
           runtime.store.record.has_history && !runtime.store.record.history.has_last_confirmation &&
           runtime.store.record.pending_count==0 &&
           runtime.state.settings.medication_seconds==original.shared.medication_seconds);
    const uint32_t selected=runtime.state.image_id;
    initialize_runtime();
    assert(runtime.store.record.history.image_id==selected &&
           runtime.state.settings.image_seconds==28800 &&
           runtime.store.record.provision.network_mode==STORE_NETWORK_OFFLINE);
    puts("S-M1 offline RANDOM: elapsed interval selects and durably reopens without Mesh/dose");
    committed_length=0;runtime=(Runtime){0};
}

static void test_offline_manual_deadline_preserves_guards(void)
{
    initialize_offline_runtime();
    ClockSample now=clock_sample(runtime.boot_domain);ManualTimePreview preview;
    assert(app_preview_local_time(&now,14,30,&preview)==APP_OK);
    UiRequest next={.kind=UI_REQUEST_SET_NEXT_LOCAL_TIME};
    next.value.local_time.hour=14;next.value.local_time.minute=30;next.value.local_time.preview=preview;
    const unsigned before=writes,calls=fake_shared_calls;
    assert(submit_ui(&runtime,&next)==UI_SUBMIT_COMMITTED);
    assert(runtime.state.start_reason==START_MANUAL &&
           runtime.state.deadline.regular_utc.ms==preview.regular_utc.ms &&
           runtime.store.record.manual_intent.pending && runtime.store.record.has_history &&
           runtime.store.record.history.start_reason==START_MANUAL &&
           runtime.store.record.history.deadline.regular_utc.ms==preview.regular_utc.ms &&
           writes>before && fake_shared_calls==calls);
    assert(!runtime.store.record.last_confirmation.valid && runtime.store.record.pending_count==0 &&
           runtime.store.record.shared.image_seconds==28800);
    const AppState committed_state=runtime.state;
    const StoreManualIntent committed_intent=runtime.store.record.manual_intent;
    next.value.local_time.preview.regular_utc.ms++;
    assert(submit_ui(&runtime,&next)!=UI_SUBMIT_COMMITTED);
    assert(!memcmp(&runtime.state,&committed_state,sizeof(committed_state)) &&
           !memcmp(&runtime.store.record.manual_intent,&committed_intent,sizeof(committed_intent)));
    /* Existing durable ID reservation precedes Core rejection; it is not a saved deadline. */
    const unsigned committed_writes=writes;
    next.value.local_time.preview=preview;
    runtime.shared_pending=true;
    assert(submit_ui(&runtime,&next)==UI_SUBMIT_UNAVAILABLE && writes==committed_writes);
    runtime.shared_pending=false;runtime.reboot_required=true;
    assert(submit_ui(&runtime,&next)==UI_SUBMIT_UNAVAILABLE && writes==committed_writes);
    runtime.reboot_required=false;runtime.store_ready=false;
    assert(submit_ui(&runtime,&next)==UI_SUBMIT_UNAVAILABLE && writes==committed_writes);
    runtime.store_ready=true;runtime.state.alarm_active=true;
    assert(submit_ui(&runtime,&next)!=UI_SUBMIT_COMMITTED && runtime.state.alarm_active &&
           runtime.store.record.history.deadline.regular_utc.ms==preview.regular_utc.ms);
    runtime.state.alarm_active=false;
    katmed_time_init(&runtime.time,runtime.boot_domain); /* Loss of UTC is not an authority bypass. */
    assert(submit_ui(&runtime,&next)!=UI_SUBMIT_COMMITTED &&
           runtime.store.record.history.deadline.regular_utc.ms==preview.regular_utc.ms &&
           !runtime.store.record.last_confirmation.valid);
    initialize_runtime();
    assert(runtime.store.record.manual_intent.pending &&
           runtime.store.record.history.deadline.regular_utc.ms==preview.regular_utc.ms &&
           runtime.state.settings.image_seconds==28800 &&
           !runtime.store.record.last_confirmation.valid &&
           runtime.store.record.provision.network_mode==STORE_NETWORK_OFFLINE);
    puts("S-M2 offline manual deadline: durable, no Mesh/dose; preview/UTC/alarm/store/reboot/pending guards");
    committed_length=0;runtime=(Runtime){0};
}

static void schedule_manual_due_test(void)
{
    ClockSample now=clock_sample(runtime.boot_domain);ManualTimePreview preview;
    assert(app_preview_local_time(&now,14,30,&preview)==APP_OK);
    UiRequest next={.kind=UI_REQUEST_SET_NEXT_LOCAL_TIME};
    next.value.local_time.hour=14;next.value.local_time.minute=30;next.value.local_time.preview=preview;
    assert(submit_ui(&runtime,&next)==UI_SUBMIT_COMMITTED);
}

static void start_offline_due_test(void)
{
    initialize_offline_runtime();schedule_manual_due_test();
}

static void advance_to_real_due(void)
{
    assert(!runtime.state.alarm_active && runtime.state.start_reason!=START_BOOT);
    assert(runtime.state.deadline.effective_monotonic_ms>(uint64_t)(fake_microseconds/1000));
    fake_microseconds=(int64_t)runtime.state.deadline.effective_monotonic_ms*1000;
    const ClockSample now=clock_sample(runtime.boot_domain);
    assert(now.utc_valid);tick_runtime(&runtime,&now);
    assert(runtime.state.alarm_active && runtime.state.start_reason!=START_BOOT);
}

static void test_offline_confirmations_do_not_fill_mesh_queue(void)
{
    start_offline_due_test();
    const UiRequest confirm={.kind=UI_REQUEST_CONFIRM};
    const unsigned notifications=fake_mesh_notifications;
    bool all_local=true;
    for(unsigned i=0;i<10;++i) {
        advance_to_real_due();
        const UiSubmitResult result=submit_ui(&runtime,&confirm);
        assert((result==UI_SUBMIT_COMMITTED || result==UI_SUBMIT_PENDING) && !runtime.state.alarm_active);
        all_local=all_local && result==UI_SUBMIT_COMMITTED &&
                  !runtime.state.confirmation_pending_sync && runtime.store.record.pending_count==0;
        assert(runtime.state.has_last_confirmation &&
               runtime.store.record.last_confirmation.valid &&
               runtime.store.record.last_confirmation.id.sequence==runtime.state.last_confirmation_id.sequence &&
               runtime.store.record.history.last_confirmation_id.sequence==runtime.state.last_confirmation_id.sequence &&
               runtime.store.record.shared.image_seconds==28800);
        const uint64_t dose_sequence=runtime.state.last_confirmation_id.sequence;
        const UiSubmitResult duplicate=submit_ui(&runtime,&confirm);
        assert(duplicate!=UI_SUBMIT_COMMITTED && duplicate!=UI_SUBMIT_PENDING &&
               !runtime.state.alarm_active && runtime.state.last_confirmation_id.sequence==dose_sequence);
    }
    assert(all_local && runtime.store.record.pending_count==0 && fake_mesh_notifications==notifications);
    const StoreConfirmationHistory last=runtime.store.record.last_confirmation;
    initialize_runtime();
    const unsigned before_boot=writes;
    assert(runtime.state.start_reason==START_BOOT);
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_STALE && writes==before_boot &&
           runtime.store.record.pending_count==0);
    assert(runtime.store.record.last_confirmation.valid==last.valid &&
           same_id(runtime.store.record.last_confirmation.id,last.id) &&
           runtime.store.record.last_confirmation.occurred_at.utc_ms==last.occurred_at.utc_ms);

    /* Real disconnected Mesh follower confirmations retain their existing outbox. */
    initialize_offline_runtime();
    StoreProvision mesh={0};mesh.configured=true;mesh.network_mode=STORE_NETWORK_MESH_ONLY;
    memset(mesh.group,1,sizeof(mesh.group));memset(mesh.secret,2,sizeof(mesh.secret));
    assert(store_save_setup(&runtime.store,&mesh)==STORE_OK);
    StoreProvision local={0};local.configured=true;local.network_mode=STORE_NETWORK_OFFLINE;
    assert(store_save_setup(&runtime.store,&local)==STORE_OK);
    schedule_manual_due_test(); /* Bind first; never carry an unbound intent into a different group. */
    assert(store_save_setup(&runtime.store,&mesh)==STORE_OK);
    runtime.mesh_started=true;fake_mesh_connected=false;fake_mesh_coordinator=false;
    for(unsigned i=0;i<KATMED_STORE_PENDING_MAX;++i) {
        advance_to_real_due();
        assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_PENDING &&
               runtime.store.record.pending_count==i+1 && runtime.state.confirmation_pending_sync);
    }
    advance_to_real_due();
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_UNAVAILABLE && runtime.state.alarm_active);
    const StoreRecord prior=runtime.store.record;
    StoreProvision offline={0};offline.configured=true;offline.network_mode=STORE_NETWORK_OFFLINE;
    assert(store_save_setup(&runtime.store,&offline)==STORE_OK);runtime.mesh_started=false;
    runtime.shared_pending=true;unsigned before=writes;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_UNAVAILABLE && writes==before && runtime.state.alarm_active);
    runtime.shared_pending=false;runtime.reboot_required=true;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_UNAVAILABLE && writes==before);
    runtime.reboot_required=false;runtime.store_ready=false;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_UNAVAILABLE && writes==before);
    runtime.store_ready=true;
    const uint64_t coordinator=runtime.state.revision.coordinator;
    runtime.state.revision.coordinator=coordinator+1;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_UNAVAILABLE && writes==before);
    runtime.state.revision.coordinator=coordinator;runtime.mesh_started=true;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_UNAVAILABLE && writes==before);
    runtime.mesh_started=false;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_COMMITTED && !runtime.state.alarm_active &&
           runtime.store.record.pending_count==KATMED_STORE_PENDING_MAX &&
           !memcmp(runtime.store.record.pending,prior.pending,sizeof(prior.pending)) &&
           !memcmp(runtime.store.record.bound_group,prior.bound_group,sizeof(prior.bound_group)));
    puts("S-M3: ten real Offline due confirmations, Boot/duplicate guards, follower queue and prior pending preserved");
    committed_length=0;runtime=(Runtime){0};
    fake_mesh_notifications=0;fake_shared_calls=0;
    fake_mesh_connected=false;fake_mesh_coordinator=false;
}

static void test_orientation_mode(void)
{
    initialize_runtime();
    const unsigned before=writes;
    const bool c6=orientation_auto_supported();
    assert(orientation_auto_for(&runtime)==c6);
    const AppState state=runtime.state;
    sensed_valid=true;
    for(unsigned i=0;i<10;++i){sensed_orientation=(Orientation)(i%4);poll_auto_orientation(&runtime);}
    assert(writes==before && memcmp(&state,&runtime.state,sizeof(state))==0);
    if(c6)assert(runtime.local.orientation==ORIENTATION_90);
    sensed_valid=false;
    UiRequest manual={.kind=UI_REQUEST_SET_ORIENTATION};
    manual.value.orientation=runtime.local.orientation;
    assert(submit_ui(&runtime,&manual)==UI_SUBMIT_COMMITTED);
    assert(writes==before+(c6 ? 1u : 0u));
    assert(!orientation_auto_for(&runtime));
    sensed_valid=true;sensed_orientation=ORIENTATION_180;poll_auto_orientation(&runtime);
    assert(runtime.local.orientation==manual.value.orientation);
    const unsigned selected=writes;
    assert(submit_ui(&runtime,&manual)==UI_SUBMIT_COMMITTED && writes==selected);
    assert(memcmp(&state,&runtime.state,sizeof(state))==0);
    initialize_runtime();
    assert(!orientation_auto_for(&runtime));
    UiRequest automatic={.kind=UI_REQUEST_SET_ORIENTATION_AUTOMATIC};
    assert(submit_ui(&runtime,&automatic)==(c6 ? UI_SUBMIT_COMMITTED : UI_SUBMIT_REJECTED));
    assert(orientation_auto_for(&runtime)==c6);
    KatmedConfigSnapshot snapshot={0};config_snapshot(&runtime,&snapshot);
    assert(snapshot.orientation_auto_supported==c6 && snapshot.orientation_automatic==c6);
    const unsigned pose_writes=writes;
    const Orientation initial=runtime.local.orientation;
    if(c6) {
        runtime.local.orientation=(Orientation)((initial+1)%4);
        KatmedConfigSnapshot moved={0};config_snapshot(&runtime,&moved);
        assert(strcmp(snapshot.revision,moved.revision)==0 && writes==pose_writes);
        runtime.local.orientation=initial;
    }
    // Web and display share the same policy command and CAS revision.
    KatmedConfigRequest request={.operation=KATMED_CONFIG_SAVE,.field=KATMED_CONFIG_ORIENTATION};
    snprintf(request.revision,sizeof(request.revision),"%s",snapshot.revision);
    snprintf(request.value,sizeof(request.value),"auto");
    KatmedConfigResponse reply={0};config_execute(&runtime,&request,&reply);
    assert(reply.status==(c6 ? KATMED_CONFIG_OK : KATMED_CONFIG_INVALID));
    // The LVGL callback only queues. The app loop owns mode, angle and Store.
    display_ui_publish(&runtime);
    manual.value.orientation=runtime.local.orientation;
    const unsigned queued_writes=writes;
    assert(submit_ui_dispatch(&runtime,&manual)==UI_SUBMIT_QUEUED);
    assert(writes==queued_writes && orientation_auto_for(&runtime)==c6);
    assert(submit_ui_dispatch(&runtime,&automatic)==UI_SUBMIT_UNAVAILABLE);
    display_ui_poll(&runtime);
    assert(!orientation_auto_for(&runtime) && !orientation_automatic(&runtime));
    assert(runtime.local.orientation==manual.value.orientation);
    UiRequest done;UiSubmitResult completion;
    assert(display_mb_result(&display_mailbox,&done,&completion));
    assert(done.kind==manual.kind && done.value.orientation==manual.value.orientation &&
           completion==UI_SUBMIT_COMMITTED);
    sensed_valid=true;sensed_orientation=(Orientation)((manual.value.orientation+1)%4);
    poll_auto_orientation(&runtime);
    assert(runtime.local.orientation==manual.value.orientation);
    // UI getters read only the last published mode, not live owner state.
    runtime.store.record.orientation_manual=false;
    assert(!orientation_automatic(&runtime));
    runtime.store.record.orientation_manual=c6;
    assert(submit_ui_dispatch(&runtime,&automatic)==UI_SUBMIT_QUEUED);
    fail_commit=true;display_ui_poll(&runtime);fail_commit=false;
    assert(display_mb_result(&display_mailbox,&done,&completion));
    assert(done.kind==automatic.kind && completion==(c6 ? UI_SUBMIT_UNAVAILABLE : UI_SUBMIT_REJECTED));
    assert(!orientation_automatic(&runtime));
    // An uncertain commit faults the Store. Recover from durable state before
    // a later attempt; never treat the failed transition as an auto-mode pass.
    initialize_runtime();display_ui_publish(&runtime);
    assert(submit_ui_dispatch(&runtime,&automatic)==UI_SUBMIT_QUEUED);
    display_ui_poll(&runtime);
    assert(display_mb_result(&display_mailbox,&done,&completion));
    assert(done.kind==automatic.kind && completion==(c6 ? UI_SUBMIT_COMMITTED : UI_SUBMIT_REJECTED));
    assert(orientation_automatic(&runtime)==c6);
    printf("orientation main C%d: same-angle persistence, reboot, auto and Web capability PASS\n",c6 ? 6 : 3);
}

static void test_boot_factory_identity(void)
{
    boot_mac_error=boot_mac_zero=boot_zero_epoch=false;boot_legacy_calls=0;
    EventId id={0};assert(boot_identity(&id));
    assert(id.device==UINT64_C(0x021324354657) && id.epoch!=0 && id.sequence==1);
    assert(boot_legacy_calls==0);
    struct {uint8_t mac[6],sentinel[2];} out={{0},{0xa5,0x5a}};
    assert(esp_read_mac(out.mac,ESP_MAC_EFUSE_FACTORY)==ESP_OK);
    assert(out.sentinel[0]==0xa5 && out.sentinel[1]==0x5a);
    EventId unchanged=id;boot_mac_error=true;assert(!boot_identity(&id));
    assert(memcmp(&id,&unchanged,sizeof(id))==0);boot_mac_error=false;
    boot_mac_zero=true;assert(!boot_identity(&id));boot_mac_zero=false;
    assert(memcmp(&id,&unchanged,sizeof(id))==0);
    boot_zero_epoch=true;assert(boot_identity(&id) && id.epoch==1 && id.sequence==1);
    boot_zero_epoch=false;fake_random_counter=UINT32_C(0x12345678);
    // Do not conceal the pre-existing Store identity boundary with a migration/bypass.
    committed_length=0;Store store;StoreRecovery recovery;uint8_t catalog[KATMED_CATALOG_BYTES]={3};
    const StoreBackend backend={NULL,test_read,test_write};
    assert(store_open(&store,backend,UINT64_C(0x021324fffe35),catalog,75,&recovery)==STORE_OK);
    const unsigned before=writes;
    assert(store_open(&store,backend,UINT64_C(0x021324354657),catalog,75,&recovery)==STORE_CORRUPT);
    assert(writes==before);committed_length=0;
    puts("boot identity: actual producer fixed factoryMAC48, sentinel6/error/zero/epoch/C3contract; mismatched persisted Store identity stays failclosed PASS");
}

static void test_display_app_loop_bridge(void) {
    initialize_runtime();display_ui_publish(&runtime);
    unsigned before=writes;
    UiRequest request={.kind=UI_REQUEST_SET_BRIGHTNESS};request.value.display_value=25;
    assert(submit_ui_dispatch(&runtime,&request)==UI_SUBMIT_QUEUED && writes==before);
    KatmedDisplaySettings snapshot;display_settings(&runtime,&snapshot);assert(snapshot.brightness==100);
    probe_display_write=true;display_ui_poll(&runtime);probe_display_write=false;
    assert(display_write_probes && runtime.store.record.display.brightness==25);
    display_settings(&runtime,&snapshot);assert(snapshot.brightness==25);
    UiRequest done;UiSubmitResult result;
    assert(display_mb_result(&display_mailbox,&done,&result) && done.kind==request.kind && result==UI_SUBMIT_COMMITTED);
    request.kind=UI_REQUEST_COPY_NIGHT;assert(submit_ui_dispatch(&runtime,&request)==UI_SUBMIT_QUEUED);
    display_ui_poll(&runtime);
    assert(display_mb_result(&display_mailbox,&done,&result) && done.kind==UI_REQUEST_COPY_NIGHT && result==UI_SUBMIT_UNAVAILABLE);
    assert(!runtime.display_policy.wake_until_ms);
    puts("N-M1 real Main bridge: callback no Store/Mesh; actual Store interleave getter nonblocking/coherent; exact result ownership PASS");
}

static void test_brightness_preview_bridge(void)
{
    committed_length=0;initialize_runtime();display_ui_publish(&runtime);
    ClockSample now=clock_sample(runtime.boot_domain);
    runtime.brightness_applied=100;applied_brightness=100;
    unsigned before=writes,board_before=brightness_calls;
    for(unsigned i=1;i<=70;++i)assert(brightness_preview(&runtime,(uint8_t)i));
    assert(writes==before && brightness_calls==board_before && !runtime.brightness_preview);
    display_ui_poll(&runtime);assert(runtime.brightness_preview==70);
    display_policy_step(&runtime,&now);
    assert(applied_brightness==70 && brightness_calls==board_before+1 && writes==before);
    assert(runtime.store.record.display.brightness==100);
    assert(brightness_preview(&runtime,0));display_ui_poll(&runtime);display_policy_step(&runtime,&now);
    assert(applied_brightness==100 && writes==before);
    assert(!brightness_preview(&runtime,101));
    assert(brightness_preview(&runtime,45));display_ui_poll(&runtime);display_policy_step(&runtime,&now);
    UiRequest request={.kind=UI_REQUEST_SET_BRIGHTNESS,.value.display_value=45};
    assert(submit_ui_dispatch(&runtime,&request)==UI_SUBMIT_QUEUED);
    assert(!brightness_preview(&runtime,50)); // pending commit must not accept a newer preview
    display_ui_poll(&runtime);display_policy_step(&runtime,&now);
    assert(writes==before+1 && applied_brightness==45 && !runtime.brightness_preview);
    UiRequest done;UiSubmitResult result;
    assert(display_mb_result(&display_mailbox,&done,&result) && result==UI_SUBMIT_COMMITTED);
    initialize_runtime();assert(runtime.store.record.display.brightness==45); // durable reopen
    display_ui_publish(&runtime);runtime.brightness_applied=45;before=writes;
    assert(brightness_preview(&runtime,60));display_ui_poll(&runtime);display_policy_step(&runtime,&now);
    assert(applied_brightness==60 && writes==before);
    request.value.display_value=60;assert(submit_ui_dispatch(&runtime,&request)==UI_SUBMIT_QUEUED);
    fail_commit=true;display_ui_poll(&runtime);fail_commit=false;display_policy_step(&runtime,&now);
    assert(display_mb_result(&display_mailbox,&done,&result) && result==UI_SUBMIT_UNAVAILABLE);
    assert(!runtime.brightness_preview && applied_brightness==45 && runtime.store.record.display.brightness==45);
    // COMMIT_UNKNOWN does not become success through a preview or a matching value.
    assert(brightness_preview(&runtime,80));display_ui_poll(&runtime);display_policy_step(&runtime,&now);
    assert(!runtime.brightness_preview && applied_brightness==45);
    request.value.display_value=45;assert(submit_ui_dispatch(&runtime,&request)==UI_SUBMIT_QUEUED);
    display_ui_poll(&runtime);assert(display_mb_result(&display_mailbox,&done,&result) && result==UI_SUBMIT_UNAVAILABLE);
    initialize_runtime();display_ui_publish(&runtime);before=writes;runtime.brightness_applied=45;
    fail_brightness=true;request.value.display_value=80;
    assert(submit_ui_dispatch(&runtime,&request)==UI_SUBMIT_QUEUED);display_ui_poll(&runtime);
    assert(display_mb_result(&display_mailbox,&done,&result) && result==UI_SUBMIT_UNAVAILABLE && writes==before);
    fail_brightness=false;
    assert(!runtime.display_policy.wake_until_ms && !fake_mesh_notifications);
    puts("brightness preview: app-loop sole Board/Store owner, coalescing, zero preview writes, one release commit, cancel/reopen/unknown/Board failure rollback PASS");
}

static void test_local_display_settings(void) {
    initialize_runtime();
    UiRequest dismiss={.kind=UI_REQUEST_DISMISS_BOOT};
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_DISMISSED);
    const unsigned no_navigation_writes=writes;
    KatmedConfigSnapshot current={0};config_snapshot(&runtime,&current);
    assert(writes==no_navigation_writes && current.display.brightness==100 && !current.display.night_enabled);
    const bool last_dose=runtime.state.has_last_confirmation;
    const uint32_t medication=runtime.state.settings.medication_seconds;
    UiRequest bright={.kind=UI_REQUEST_SET_BRIGHTNESS};bright.value.display_value=25;
    assert(submit_ui(&runtime,&bright)==UI_SUBMIT_COMMITTED);
    UiRequest enable={.kind=UI_REQUEST_SET_NIGHT_ENABLED};enable.value.display_value=1;
    assert(submit_ui(&runtime,&enable)==UI_SUBMIT_COMMITTED);
    UiRequest start={.kind=UI_REQUEST_SET_NIGHT_START};start.value.display_value=420;
    unsigned before=writes;
    assert(submit_ui(&runtime,&start)==UI_SUBMIT_REJECTED && writes==before); // equal end rejected
    config_snapshot(&runtime,&current);
    KatmedConfigRequest request={.operation=KATMED_CONFIG_SAVE,.field=KATMED_CONFIG_NIGHT_START};
    snprintf(request.revision,sizeof(request.revision),"%s",current.revision);
    snprintf(request.value,sizeof(request.value),"21:30");
    KatmedConfigResponse response={0};config_execute(&runtime,&request,&response);
    assert(response.status==KATMED_CONFIG_OK && runtime.store.record.display.night_start==1290);
    config_execute(&runtime,&request,&response);assert(response.status==KATMED_CONFIG_STALE);
    assert(runtime.state.has_last_confirmation==last_dose && runtime.state.settings.medication_seconds==medication);
    assert(!runtime.display_policy.wake_until_ms && !runtime.reboot_required);
    before=writes;UiRequest copy={.kind=UI_REQUEST_COPY_NIGHT};
    assert(submit_ui(&runtime,&copy)==UI_SUBMIT_UNAVAILABLE && writes==before);
    puts("night Main/UI/Web: local offline save, CAS, no medication/wake mutation and unavailable Mesh gate PASS");
}

int main(int argc, char **argv)
{
    if(argc==2 && !strcmp(argv[1],"boot_identity")){test_boot_factory_identity();return 0;}
    if(argc==2 && !strcmp(argv[1],"night_bridge")){test_display_app_loop_bridge();test_brightness_preview_bridge();test_boot_factory_identity();return 0;}
    if(argc==2 && !strcmp(argv[1],"night")){test_local_display_settings();return 0;}
    if (argc==2 && strcmp(argv[1],"orientation")==0) {test_orientation_mode();return 0;}
    if (argc==2 && strcmp(argv[1],"offline_confirm")==0) {
        test_offline_confirmations_do_not_fill_mesh_queue();return 0;
    }
    if (argc==2 && strcmp(argv[1],"offline_random")==0) {
        test_offline_random_commits_without_mesh();return 0;
    }
    if (argc==2 && strcmp(argv[1],"offline_deadline")==0) {
        test_offline_manual_deadline_preserves_guards();return 0;
    }
    assert(argc==1);
    test_offline_confirmations_do_not_fill_mesh_queue();
    test_offline_random_commits_without_mesh();
    test_offline_manual_deadline_preserves_guards();
    Runtime setup={0};
    assert(start_setup_ap(&setup) && setup.setup_ap && setup_config_calls==1);
    test_runtime_setup_and_emergency();
    test_mesh_only_without_station_profile();
    test_offline_exit_clock_policy();
    fake_microseconds=1000000;
    initialize_runtime();
    assert(runtime.state.alarm_active && !runtime.state.has_last_confirmation);
    const unsigned before=writes;
    UiRequest shared = {.kind=UI_REQUEST_SET_MEDICATION_SECONDS};
    shared.value.seconds=3600;
    assert(submit_ui(&runtime,&shared)==UI_SUBMIT_UNAVAILABLE && writes==before);
    UiRequest confirm = {.kind=UI_REQUEST_CONFIRM};
    fake_microseconds=2000000;
    const UiRequest dismiss = {.kind=UI_REQUEST_DISMISS_BOOT};
    const size_t boot_blob_length=committed_length;
    uint8_t boot_blob[KATMED_STORE_BLOB_MAX];
    memcpy(boot_blob,committed,boot_blob_length);
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_DISMISSED);
    assert(writes==before && committed_length==boot_blob_length &&
           memcmp(committed,boot_blob,boot_blob_length)==0 &&
           !runtime.state.has_last_confirmation &&
           runtime.state.deadline.effective_monotonic_ms==0 &&
           runtime.store.record.pending_count==0);
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_STALE);
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_STALE);
    assert(writes==before);
    assert(submit_due_confirm()==UI_SUBMIT_PENDING);
    assert(!runtime.state.alarm_active && runtime.state.has_last_confirmation &&
           runtime.state.confirmation_pending_sync && runtime.store.record.pending_count==1);
    const EventId first=runtime.state.last_confirmation_id;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_STALE);
    assert(runtime.store.record.pending_count==1 &&
           runtime.state.last_confirmation_id.sequence==first.sequence);
    UiRequest clock_style = {.kind=UI_REQUEST_SET_CLOCK_STYLE};
    clock_style.value.clock_style=ANALOG;
    assert(submit_ui(&runtime,&clock_style)==UI_SUBMIT_COMMITTED);
    assert(runtime.local.clock_style==ANALOG);
    UiRequest orientation = {.kind=UI_REQUEST_SET_ORIENTATION};
    orientation.value.orientation=ORIENTATION_270;
    assert(submit_ui(&runtime,&orientation)==UI_SUBMIT_COMMITTED);
    assert(runtime.local.orientation==ORIENTATION_270 && board_orientation==ORIENTATION_270);
    const unsigned after=writes;
    fake_microseconds+=25000;
    AppEffects effects={0};
    ClockSample now=clock_sample(runtime.boot_domain);
    assert(app_tick(&runtime.state,&now,&effects)==APP_OK);
    assert(!effects.persistence_required && writes==after);
    assert(ui_next_view(BACKGROUND,SWIPE_RIGHT,false)==CLOCK);
    assert(ui_next_view(CLOCK,SWIPE_LEFT,false)==BACKGROUND);
    assert(writes==after); // Transient clock visibility never reaches Store.
    initialize_runtime(); // Actual decode and fresh boot identity, no reused timer.
    assert(runtime.state.alarm_active && runtime.state.start_reason==START_BOOT);
    assert(runtime.local.orientation==ORIENTATION_270 && runtime.local.clock_style==ANALOG);
    assert(pending_confirmations(&runtime)==1 && storage_ready(&runtime));
    ClockSample historical={0};
    assert(historical_confirmation(&runtime,&historical));
    assert(!historical.utc_valid && historical.domain!=runtime.state.clock.domain);
    assert(runtime.state.deadline.effective_monotonic_ms==0);
    const StoreRecord preserved=runtime.store.record;
    const size_t preserved_length=committed_length;
    uint8_t preserved_blob[KATMED_STORE_BLOB_MAX];
    memcpy(preserved_blob,committed,preserved_length);
    const unsigned before_reboot_dismiss=writes;
    runtime.mesh_started=true;
    fake_mesh_connected=true; /* Group transport is available on this boot. */
    fake_mesh_coordinator=false;
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_DISMISSED);
    assert(writes==before_reboot_dismiss && fake_mesh_notifications==0 &&
           committed_length==preserved_length &&
           memcmp(committed,preserved_blob,preserved_length)==0 &&
           memcmp(&runtime.store.record,&preserved,sizeof(preserved))==0);
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_STALE);
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_STALE);
    assert(store_ack_pending(&runtime.store,&runtime.state,
           (EventId){runtime.boot.device,runtime.boot.epoch,2},true)==STORE_NOT_FOUND);
    assert(writes==before_reboot_dismiss && fake_mesh_notifications==0 &&
           runtime.store.record.pending_count==1 &&
           runtime.store.record.last_confirmation.valid &&
           runtime.store.record.history.deadline.effective_monotonic_ms==
               preserved.history.deadline.effective_monotonic_ms);
    initialize_runtime();
    assert(runtime.state.alarm_active && runtime.state.start_reason==START_BOOT &&
           runtime.store.record.pending_count==1 &&
           runtime.store.record.last_confirmation.valid &&
           runtime.store.record.history.deadline.effective_monotonic_ms==
               preserved.history.deadline.effective_monotonic_ms);
    fail_commit=true;
    const AppState before_state=runtime.state;
    fake_microseconds+=1000000;
    assert(submit_due_confirm()==UI_SUBMIT_UNAVAILABLE);
    assert(runtime.state.alarm_active==before_state.alarm_active &&
           runtime.state.round.ordinal==before_state.round.ordinal && !storage_ready(&runtime));
    /* Real board callback + Core + durable Store, with no authenticated peer.
     * The Mesh transport double below still commits through store_apply_event. */
    committed_length=0;
    fail_commit=false;
    initialize_runtime();
    runtime.mesh_started=true;
    fake_mesh_connected=false;
    fake_mesh_coordinator=true;
    fake_mesh_result=MESH_ACCEPTED;
    UiRequest image={.kind=UI_REQUEST_SET_IMAGE_SECONDS};
    image.value.seconds=7200;
    assert(submit_ui(&runtime,&image)==UI_SUBMIT_COMMITTED);
    assert(runtime.state.settings.image_seconds==7200 &&
           runtime.store.record.shared.image_seconds==7200);
    initialize_runtime();
    assert(runtime.state.settings.image_seconds==7200); /* Actual blob decode. */
    runtime.mesh_started=true;
    katmed_time_init(&runtime.time,runtime.boot_domain);
    assert(katmed_time_accept_utc(&runtime.time,(uint64_t)(fake_microseconds/1000),INT64_C(1792400400000),CLOCK_SYNCHRONIZED));
    fake_microseconds+=1000000;
    assert(submit_due_confirm()==UI_SUBMIT_PENDING);
    KatmedConfigRequest web={.operation=KATMED_CONFIG_GET};
    KatmedConfigResponse web_reply={0};
    config_execute(&runtime,&web,&web_reply);
    assert(web_reply.status==KATMED_CONFIG_OK &&
           strcmp(web_reply.snapshot.image_hours,"2")==0);
    web.operation=KATMED_CONFIG_SAVE;
    web.field=KATMED_CONFIG_MEDICATION_HOURS;
    strcpy(web.revision,"stale");
    strcpy(web.value,"1.5");
    config_execute(&runtime,&web,&web_reply);
    assert(web_reply.status==KATMED_CONFIG_STALE);
    web.operation=KATMED_CONFIG_GET;
    config_execute(&runtime,&web,&web_reply);
    strcpy(web.revision,web_reply.snapshot.revision);
    web.operation=KATMED_CONFIG_SAVE;
    strcpy(web.value,"0");
    config_execute(&runtime,&web,&web_reply);
    assert(web_reply.status==KATMED_CONFIG_INVALID);
    web.field=KATMED_CONFIG_ORIENTATION;
    strcpy(web.value,"180");
    config_execute(&runtime,&web,&web_reply);
    assert(web_reply.status==KATMED_CONFIG_OK &&
           web_reply.snapshot.orientation==ORIENTATION_180 &&
           board_orientation==ORIENTATION_180);
    web.operation=KATMED_CONFIG_PREVIEW_TIME;
    strcpy(web.revision,web_reply.snapshot.revision);
    strcpy(web.value,"14:30");
    config_execute(&runtime,&web,&web_reply);
    assert(web_reply.status==KATMED_CONFIG_OK &&
           strcmp(web_reply.preview.time,"14:30")==0);
    web.operation=KATMED_CONFIG_SAVE;
    web.field=KATMED_CONFIG_NEXT_DUE_HHMM;
    strcpy(web.preview_token,"invalid");
    const unsigned before_bad_token=writes;
    config_execute(&runtime,&web,&web_reply);
    assert(web_reply.status==KATMED_CONFIG_STALE && writes==before_bad_token);
    web.operation=KATMED_CONFIG_PREVIEW_TIME;
    config_execute(&runtime,&web,&web_reply);
    assert(web_reply.status==KATMED_CONFIG_OK);
    strcpy(web.preview_token,web_reply.preview.preview_token);
    web.operation=KATMED_CONFIG_SAVE;
    ClockSample manual_now=clock_sample(runtime.boot_domain);
    ManualTimePreview preview;
    assert(app_preview_local_time(&manual_now,14,30,&preview)==APP_OK);
    config_execute(&runtime,&web,&web_reply);
    assert(web_reply.status==KATMED_CONFIG_OK &&
           web_reply.snapshot.next_due_state==KATMED_DUE_ACTIVE);
    assert(runtime.state.start_reason==START_MANUAL &&
           runtime.state.deadline.regular_utc.ms==preview.regular_utc.ms &&
           runtime.store.record.history.deadline.regular_utc.ms==preview.regular_utc.ms);
    const uint8_t pending_saved=runtime.store.record.pending_count;
    const EventId confirmation_saved=runtime.state.last_confirmation_id;
    initialize_runtime(); /* Reopen/decode the actual manual-time history. */
    assert(runtime.store.record.history.start_reason==START_MANUAL &&
           runtime.store.record.manual_intent.pending &&
           runtime.store.record.history.deadline.regular_utc.ms==preview.regular_utc.ms &&
           runtime.store.record.pending_count==pending_saved &&
           runtime.store.record.last_confirmation.id.sequence==confirmation_saved.sequence);
    web=(KatmedConfigRequest){.operation=KATMED_CONFIG_GET};
    config_execute(&runtime,&web,&web_reply);
    assert(web_reply.status==KATMED_CONFIG_OK &&
           web_reply.snapshot.next_due_state==KATMED_DUE_PAUSED_BOOT &&
           strcmp(web_reply.snapshot.next_due_hhmm,"14:30")==0);
    runtime.mesh_started=true;
    fake_mesh_connected=true;
    katmed_time_init(&runtime.time,runtime.boot_domain);
    const int64_t earlier=(preview.effective_utc.ms<preview.regular_utc.ms ?
        preview.effective_utc.ms:preview.regular_utc.ms)-600000;
    assert(katmed_time_accept_utc(&runtime.time,(uint64_t)(fake_microseconds/1000),
                                  earlier,CLOCK_SYNCHRONIZED));
    ClockSample restore_now=clock_sample(runtime.boot_domain);
    assert(apply_time_sample(&runtime,&restore_now));
    /* Feed two authenticated Boot snapshots through the same Core/Store
     * entry used by the Mesh adapter, one on each side of local dismissal. */
    AppState peer_boot={0};
    ClockSample peer_captured={1000,99,restore_now.utc_ms-1000,true,CLOCK_SYNCHRONIZED};
    assert(app_init(&peer_boot,&runtime.state.settings,&peer_captured,
                    (EventId){99,5,1},75,runtime.state.catalog_id)==APP_OK);
    StateSnapshot peer_snapshot={0};
    assert(app_make_snapshot(&peer_boot,&peer_captured,&peer_snapshot)==APP_OK);
    AppEvent peer_event={0};
    peer_event.kind=EVENT_ADOPT_SNAPSHOT;
    peer_event.id=(EventId){99,5,2};
    peer_event.sender=99;
    peer_event.round=runtime.state.round;
    peer_event.parent_revision=runtime.state.revision;
    peer_event.occurred_at=restore_now;
    peer_event.coordinator_accepted=true;
    peer_event.value.snapshot=&peer_snapshot;
    const StoreRecord before_peer_boot=runtime.store.record;
    const unsigned before_peer_writes=writes;
    const unsigned before_peer_notifications=fake_mesh_notifications;
    AppEffects peer_effects={0};
    AppResult peer_core=APP_INVALID;
    assert(store_apply_event(&runtime.store,&runtime.state,&peer_event,&restore_now,
                             false,&peer_effects,&peer_core)==STORE_OK && peer_core==APP_OK);
    assert(runtime.state.alarm_active && runtime.state.start_reason==START_BOOT &&
           !runtime.state.has_last_confirmation &&
           memcmp(&runtime.store.record,&before_peer_boot,sizeof(before_peer_boot))==0 &&
           writes==before_peer_writes &&
           fake_mesh_notifications==before_peer_notifications);
    restore_manual_if_ready(&runtime,&restore_now);
    assert(runtime.state.alarm_active && runtime.store.record.manual_intent.pending);
    fake_microseconds+=1000000;
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_DISMISSED);
    restore_now=clock_sample(runtime.boot_domain);
    peer_captured.monotonic_ms+=1000;
    peer_captured.utc_ms+=1000;
    assert(app_make_snapshot(&peer_boot,&peer_captured,&peer_snapshot)==APP_OK);
    peer_event.id.sequence=3;
    peer_event.round=runtime.state.round;
    peer_event.parent_revision=runtime.state.revision;
    peer_event.occurred_at=restore_now;
    assert(store_apply_event(&runtime.store,&runtime.state,&peer_event,&restore_now,
                             false,&peer_effects,&peer_core)==STORE_OK && peer_core==APP_OK);
    assert(!runtime.state.alarm_active && runtime.state.start_reason==START_BOOT &&
           !runtime.state.has_last_confirmation &&
           !peer_effects.alarm_started && !peer_effects.persistence_required &&
           !peer_effects.broadcast_required &&
           memcmp(&runtime.store.record,&before_peer_boot,sizeof(before_peer_boot))==0 &&
           writes==before_peer_writes &&
           fake_mesh_notifications==before_peer_notifications);
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_STALE &&
           writes==before_peer_writes &&
           runtime.store.record.manual_intent.pending);
    restore_manual_if_ready(&runtime,&restore_now);
    assert(runtime.state.start_reason==START_MANUAL &&
           runtime.state.deadline.regular_utc.ms==preview.regular_utc.ms &&
           runtime.store.record.manual_intent.pending);
    uint8_t saved_before_expiry[KATMED_STORE_BLOB_MAX];
    const size_t saved_before_expiry_length=committed_length;
    memcpy(saved_before_expiry,committed,committed_length);
    initialize_runtime();
    runtime.mesh_started=true;
    katmed_time_init(&runtime.time,runtime.boot_domain);
    assert(katmed_time_accept_utc(&runtime.time,(uint64_t)(fake_microseconds/1000),
                                  earlier,CLOCK_SYNCHRONIZED));
    ClockSample rejected_now=clock_sample(runtime.boot_domain);
    assert(apply_time_sample(&runtime,&rejected_now));
    assert(runtime.state.alarm_active && runtime.store.record.manual_intent.pending);
    fake_microseconds+=1000000;
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_DISMISSED);
    fake_mesh_result=MESH_REJECTED;
    rejected_now=clock_sample(runtime.boot_domain);
    restore_manual_if_ready(&runtime,&rejected_now);
    assert(runtime.manual_restore_attempted &&
           runtime.manual_restore_status==KATMED_RESTORE_CONFLICT &&
           !runtime.store.record.manual_intent.pending);
    web=(KatmedConfigRequest){.operation=KATMED_CONFIG_GET};
    config_execute(&runtime,&web,&web_reply);
    assert(web_reply.status==KATMED_CONFIG_OK &&
           web_reply.snapshot.next_due_state!=KATMED_DUE_PAUSED_BOOT &&
           web_reply.snapshot.restore_status==KATMED_RESTORE_CONFLICT);
    initialize_runtime();
    assert(!runtime.store.record.manual_intent.pending); /* Durable rejection. */
    memcpy(committed,saved_before_expiry,saved_before_expiry_length);
    committed_length=saved_before_expiry_length;
    initialize_runtime();
    runtime.mesh_started=true;
    katmed_time_init(&runtime.time,runtime.boot_domain);
    assert(katmed_time_accept_utc(&runtime.time,(uint64_t)(fake_microseconds/1000),
                                  earlier,CLOCK_SYNCHRONIZED));
    ClockSample delayed_now=clock_sample(runtime.boot_domain);
    assert(apply_time_sample(&runtime,&delayed_now));
    fake_mesh_coordinator=false;
    fake_mesh_connected=true;
    fake_mesh_result=MESH_PENDING;
    fake_microseconds+=1000000;
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_DISMISSED);
    delayed_now=clock_sample(runtime.boot_domain);
    restore_manual_if_ready(&runtime,&delayed_now);
    assert(runtime.manual_restore_waiting && runtime.shared_pending &&
           runtime.store.record.manual_intent.pending);
    mesh_shared_result(&runtime,fake_shared_event.id,MESH_SHARED_REJECTED);
    restore_manual_if_ready(&runtime,&delayed_now);
    assert(!runtime.manual_restore_waiting && !runtime.manual_restore_rejected &&
           runtime.manual_restore_status==KATMED_RESTORE_CONFLICT &&
           !runtime.store.record.manual_intent.pending);
    initialize_runtime();
    assert(!runtime.store.record.manual_intent.pending); /* Delayed rejection durable. */
    memcpy(committed,saved_before_expiry,saved_before_expiry_length);
    committed_length=saved_before_expiry_length;
    initialize_runtime();
    runtime.mesh_started=true;
    katmed_time_init(&runtime.time,runtime.boot_domain);
    assert(katmed_time_accept_utc(&runtime.time,(uint64_t)(fake_microseconds/1000),
                                  earlier,CLOCK_SYNCHRONIZED));
    ClockSample timeout_now=clock_sample(runtime.boot_domain);
    assert(apply_time_sample(&runtime,&timeout_now));
    fake_mesh_result=MESH_PENDING;
    fake_microseconds+=1000000;
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_DISMISSED);
    timeout_now=clock_sample(runtime.boot_domain);
    restore_manual_if_ready(&runtime,&timeout_now);
    const EventId timed_out_id=runtime.shared_id;
    const uint64_t reserved_sequence=runtime.store.record.next_sequence;
    mesh_shared_result(&runtime,timed_out_id,MESH_SHARED_TIMEOUT);
    assert(!runtime.shared_pending && runtime.manual_restore_retry &&
           !runtime.manual_restore_attempted &&
           runtime.store.record.manual_intent.pending &&
           runtime.manual_restore_status==KATMED_RESTORE_NONE);
    fake_mesh_result=MESH_UNAVAILABLE;
    restore_manual_if_ready(&runtime,&timeout_now);
    assert(runtime.manual_restore_retry && runtime.store.record.manual_intent.pending);
    fake_mesh_result=MESH_PENDING;
    restore_manual_if_ready(&runtime,&timeout_now);
    assert(runtime.shared_pending && runtime.manual_restore_waiting &&
           runtime.shared_id.sequence==timed_out_id.sequence &&
           runtime.store.record.next_sequence==reserved_sequence &&
           runtime.store.record.manual_intent.pending);
    /* Lost ACK: the coordinator state arrives durably after the timeout.
     * Main must recognize it before issuing a second request. */
    mesh_shared_result(&runtime,timed_out_id,MESH_SHARED_TIMEOUT);
    Store reopened_timeout={0};StoreRecovery timeout_recovery={0};
    const StoreBackend timeout_backend={NULL,test_read,test_write};
    uint8_t timeout_catalog[KATMED_CATALOG_BYTES]={1,2,3};
    assert(store_open(&reopened_timeout,timeout_backend,UINT64_C(0xAC276E46BC2C),
                      timeout_catalog,75,&timeout_recovery)==STORE_OK);
    assert(reopened_timeout.record.manual_intent.pending);
    AppEvent adopted=fake_shared_event;adopted.coordinator_accepted=true;
    AppEffects adopted_effects={0};AppResult adopted_result;
    assert(store_apply_event(&runtime.store,&runtime.state,&adopted,&timeout_now,
                            false,&adopted_effects,&adopted_result)==STORE_OK);
    const unsigned calls_before_adopted=fake_shared_calls;
    restore_manual_if_ready(&runtime,&timeout_now);
    assert(runtime.manual_restore_attempted && !runtime.manual_restore_retry &&
           fake_shared_calls==calls_before_adopted && runtime.state.start_reason==START_MANUAL);
    /* An unrelated or late callback cannot turn an adopted result into conflict. */
    mesh_shared_result(&runtime,timed_out_id,MESH_SHARED_REJECTED);
    restore_manual_if_ready(&runtime,&timeout_now);
    assert(!runtime.manual_restore_rejected && runtime.state.start_reason==START_MANUAL);
    initialize_runtime();
    assert(runtime.store.record.manual_intent.pending);
    /* Restore the original boot image for definitive retry rejection. */
    memcpy(committed,saved_before_expiry,saved_before_expiry_length);
    committed_length=saved_before_expiry_length;
    initialize_runtime();
    runtime.manual_restore_rejected=true;
    restore_manual_if_ready(&runtime,&timeout_now);
    assert(!runtime.store.record.manual_intent.pending &&
           runtime.manual_restore_status==KATMED_RESTORE_CONFLICT);
    initialize_runtime();
    assert(!runtime.store.record.manual_intent.pending);
    fake_mesh_coordinator=true;
    fake_mesh_result=MESH_ACCEPTED;
    memcpy(committed,saved_before_expiry,saved_before_expiry_length);
    committed_length=saved_before_expiry_length;
    initialize_runtime();
    katmed_time_init(&runtime.time,runtime.boot_domain);
    assert(katmed_time_accept_utc(&runtime.time,(uint64_t)(fake_microseconds/1000),
        preview.regular_utc.ms+600000,CLOCK_SYNCHRONIZED));
    ClockSample expired_now=clock_sample(runtime.boot_domain);
    restore_manual_if_ready(&runtime,&expired_now);
    assert(!runtime.store.record.manual_intent.pending &&
           runtime.manual_restore_status==KATMED_RESTORE_EXPIRED &&
           runtime.state.alarm_active);
    web=(KatmedConfigRequest){.operation=KATMED_CONFIG_GET};
    config_execute(&runtime,&web,&web_reply);
    assert(web_reply.status==KATMED_CONFIG_OK &&
           web_reply.snapshot.restore_status==KATMED_RESTORE_EXPIRED);

    memcpy(committed,saved_before_expiry,saved_before_expiry_length);
    committed_length=saved_before_expiry_length;
    initialize_runtime();
    katmed_time_init(&runtime.time,runtime.boot_domain);
    assert(katmed_time_accept_utc(&runtime.time,(uint64_t)(fake_microseconds/1000),
                                  earlier,CLOCK_SYNCHRONIZED));
    runtime.state.settings.revision.number++; /* Conflicting live group state. */
    ClockSample conflict_now=clock_sample(runtime.boot_domain);
    restore_manual_if_ready(&runtime,&conflict_now);
    assert(!runtime.store.record.manual_intent.pending &&
           runtime.manual_restore_status==KATMED_RESTORE_CONFLICT);
    initialize_runtime();
    runtime.mesh_started=true;
    /* This guard fixture represents an already-bound group, not the newly
     * supported fresh-unbound local-settings case. */
    runtime.store.record.group_origin=STORE_GROUP_ORIGIN_FIRST_BOUND;
    runtime.store.record.bound_group[0]=1;
    const unsigned committed_settings=writes;
    fake_mesh_connected=false;
    fake_mesh_coordinator=false; /* Disconnected follower must not self-authorize. */
    image.value.seconds=3600;
    assert(submit_ui(&runtime,&image)==UI_SUBMIT_UNAVAILABLE && writes==committed_settings);
    fake_mesh_coordinator=true;
    runtime.mesh_started=false; /* A stopped bound runtime stays blocked. */
    assert(submit_ui(&runtime,&image)==UI_SUBMIT_UNAVAILABLE && writes==committed_settings);
    runtime.mesh_started=true;
    fail_commit=true;
    const AppState stable=runtime.state;
    assert(submit_ui(&runtime,&image)==UI_SUBMIT_UNAVAILABLE);
    assert(runtime.state.settings.image_seconds==stable.settings.image_seconds &&
           runtime.state.revision.number==stable.revision.number);

    /* A synthetic persisted group identity without a clock still has only a
     * boot notice. No credential, network, or physical device is involved. */
    fail_commit=false;
    committed_length=0;
    initialize_runtime();
    runtime.store.record.group_origin=STORE_GROUP_ORIGIN_FIRST_BOUND;
    runtime.store.record.bound_group[0]=1;
    LocalSettings bound_local=runtime.local;
    bound_local.clock_style=ANALOG;
    assert(store_set_local(&runtime.store,&runtime.local,&bound_local)==STORE_OK);
    initialize_runtime();
    assert(runtime.store.record.group_origin==STORE_GROUP_ORIGIN_FIRST_BOUND &&
           runtime.store.record.bound_group[0]==1 &&
           !runtime.state.clock.utc_valid && !runtime.state.has_last_confirmation);
    runtime.mesh_started=true;
    fake_mesh_connected=true;
    fake_mesh_coordinator=true;
    const StoreRecord bound_before=runtime.store.record;
    const unsigned bound_writes=writes;
    const unsigned bound_notifications=fake_mesh_notifications;
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_DISMISSED);
    assert(writes==bound_writes && fake_mesh_notifications==bound_notifications &&
           memcmp(&runtime.store.record,&bound_before,sizeof(bound_before))==0 &&
           runtime.store.record.pending_count==0 &&
           runtime.state.deadline.effective_monotonic_ms==0);
    /* Neither an HTTP offer nor a prepared reboot can enter the journal.
     * Only a new display token after the one-boot marker may request it. */
    memset(&runtime,0,sizeof(runtime));
    reset_mutex=xSemaphoreCreateMutexStatic(&reset_mutex_storage);
    fake_microseconds=1000000;
    char reset_token[33]={0};size_t reset_length=0;
    assert(!katmed_runtime_reset_prepare(&runtime,reset_token,sizeof(reset_token),&reset_length));
    assert(web_reset_offer(&runtime) && runtime.reset_web_offer && !reset_marker);
    assert(web_reset_status(&runtime)==KATMED_WEB_RESET_PREPARING);
    assert(katmed_runtime_reset_arm(&runtime,true) && reset_marker);
    assert(web_reset_status(&runtime)==KATMED_WEB_RESET_PREPARED);
    assert(katmed_runtime_reset_confirm(&runtime,"stale",5)==1 && !runtime.reset_requested);
    assert(katmed_runtime_reset_cancel(&runtime) && !reset_marker);
    assert(katmed_reset_idf_take_preparation(&runtime.reset_warning_ready) &&
           !runtime.reset_warning_ready);
    assert(katmed_runtime_reset_arm(&runtime,false) && reset_marker);
    memset(&runtime,0,sizeof(runtime)); /* next boot */
    assert(katmed_reset_idf_take_preparation(&runtime.reset_warning_ready) &&
           runtime.reset_warning_ready && !reset_marker);
    assert(katmed_reset_idf_take_preparation(&reset_marker) && !reset_marker);
    assert(!reset_reason_allows_warning(ESP_RST_SW));
    assert(!reset_reason_allows_warning(ESP_RST_WDT));
    assert(!reset_reason_allows_warning(ESP_RST_BROWNOUT));
    assert(!reset_reason_allows_warning(ESP_RST_UNKNOWN));
    assert(reset_reason_allows_warning(ESP_RST_EXT));
    assert(reset_reason_allows_warning(ESP_RST_POWERON));
    assert(katmed_runtime_reset_prepare(&runtime,reset_token,sizeof(reset_token),&reset_length));
    assert(reset_length==32 && !runtime.reset_requested);
    assert(katmed_runtime_reset_confirm(&runtime,"stale",5)==1 && !runtime.reset_requested);
    assert(katmed_runtime_reset_confirm(&runtime,reset_token,reset_length)==0 &&
           runtime.reset_requested && !runtime.reset_warning_ready);
    assert(katmed_runtime_reset_confirm(&runtime,reset_token,reset_length)==1);
    assert(web_reset_status(&runtime)==KATMED_WEB_RESET_STOP_PENDING);
    assert(web_reset_cancel(&runtime)==KATMED_WEB_RESET_CANCEL_TOO_LATE);
    assert(runtime.reset_requested && !runtime.reset_web_close);
    runtime.reset_requested=false;runtime.reset_active=true;
    assert(web_reset_cancel(&runtime)==KATMED_WEB_RESET_CANCEL_TOO_LATE);
    assert(web_reset_status(&runtime)==KATMED_WEB_RESET_COMMITTED);
    runtime.reset_active=false;

    /* A queued close belongs to its old tile, even if local preparation
     * writes a fresh NVS marker before the main loop can close it. */
    assert(web_reset_offer(&runtime));
    assert(katmed_runtime_reset_arm(&runtime,true) && reset_marker);
    assert(web_reset_cancel(&runtime)==KATMED_WEB_RESET_CANCELLED);
    const uint64_t old_close=runtime.reset_web_close_generation;
    assert(reset_close_matches_locked(&runtime,old_close));
    assert(katmed_runtime_reset_arm(&runtime,false) && reset_marker);
    assert(!reset_close_matches_locked(&runtime,old_close));
    assert(web_reset_status(&runtime)==KATMED_WEB_RESET_PREPARED);
    assert(web_reset_cancel(&runtime)==KATMED_WEB_RESET_CANCELLED);
    assert(!reset_marker);

    assert(web_reset_offer(&runtime));
    const uint64_t rejected_epoch=runtime.reset_web_offer_epoch;
    reset_web_offer_complete(&runtime,rejected_epoch,false); /* Alarm/UI refusal. */
    assert(web_reset_status(&runtime)==KATMED_WEB_RESET_UNAVAILABLE);
    assert(!reset_marker && !runtime.reset_web_offer);
    assert(web_reset_offer(&runtime));
    reset_web_offer_complete(&runtime,rejected_epoch,false); /* stale result */
    assert(web_reset_status(&runtime)==KATMED_WEB_RESET_PREPARING);
    assert(web_reset_cancel(&runtime)==KATMED_WEB_RESET_CANCELLED);
    /* BOOT may cancel the durable preparation and then observe IO9 Low.
     * Its abandoned restart must still retire the stale display tile. */
    runtime.reset_web_close=false;
    assert(katmed_runtime_reset_arm(&runtime,false) && reset_marker);
    const unsigned boot_writes=writes, boot_notifications=fake_mesh_notifications;
    assert(boot_cancel_preparation(&runtime) && !reset_marker);
    assert(runtime.reset_web_close &&
           reset_close_matches_locked(&runtime,runtime.reset_web_close_generation));
    assert(writes==boot_writes && fake_mesh_notifications==boot_notifications);
    const uint64_t boot_close=runtime.reset_web_close_generation;
    assert(katmed_runtime_reset_arm(&runtime,false) && reset_marker);
    assert(!reset_close_matches_locked(&runtime,boot_close));
    assert(katmed_runtime_reset_cancel(&runtime) && !reset_marker);
    puts("main solo-root settings: image persistence, next-time history, follower/unstarted and commit failure OK");
    puts("physical reset runtime: offer, cancel, one-boot marker, warning, fresh one-use confirmation OK");
    puts("main submit/reboot regression passed");
    return 0;
}
