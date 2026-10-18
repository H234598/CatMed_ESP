#include <assert.h>
#include <stdio.h>
#include <string.h>

#define main original_store_main
#include "../host_store/check_main_submit.c"
#undef main

static KatmedUsbLine incoming_line;
static bool line_ready;
static char last_reply[256];
static unsigned mesh_stops;
static unsigned restarts;
static const esp_app_desc_t app_desc={"testbuild"};

const esp_app_desc_t *esp_app_get_description(void) { return &app_desc; }
int esp_app_get_elf_sha256(char *out,size_t capacity) {
    if (capacity<9) return -1;
    memcpy(out,"01234567",9); return 8;
}
const uint8_t *katmed_images_catalog_sha256(void) {
    static uint8_t catalog[KATMED_CATALOG_BYTES]={1,2,3};
    return catalog;
}
bool katmed_usb_console_receive(KatmedUsbLine *line) {
    if (!line_ready) return false;
    *line=incoming_line; line_ready=false; return true;
}
void katmed_usb_console_reply(const char *reply) {
    snprintf(last_reply,sizeof(last_reply),"%s",reply);
}
void katmed_mesh_stop(void) { ++mesh_stops; }
void esp_restart(void) { ++restarts; }
esp_err_t katmed_time_idf_set_root(KatmedTime *time,bool is_root,bool has_ip,
                                    const uint8_t *server,size_t length) {
    (void)time;(void)is_root;(void)has_ip;(void)server;(void)length;
    return ESP_OK;
}

static void queue_provision(unsigned char group,unsigned char secret)
{
    static const char digits[]="0123456789abcdef";
    char group_hex[33],secret_hex[65];
    for (unsigned i=0;i<16;++i) {
        group_hex[i*2]=digits[group>>4]; group_hex[i*2+1]=digits[group&15];
    }
    for (unsigned i=0;i<32;++i) {
        secret_hex[i*2]=digits[secret>>4]; secret_hex[i*2+1]=digits[secret&15];
    }
    group_hex[32]=0; secret_hex[64]=0;
    const int length=snprintf(incoming_line.data,sizeof(incoming_line.data),
        "KM1 PROVISION group=%s secret=%s ssid=486f6d65 "
        "password=3132333435363738 time=706f6f6c2e6e74702e6f7267\n",
        group_hex,secret_hex);
    assert(length>0 && length<(int)sizeof(incoming_line.data));
    incoming_line.length=(size_t)length;
    incoming_line.overlong=false;
    line_ready=true;
}

static void reset_backend(void)
{
    committed_length=0;
    writes=0;
    fail_commit=false;
    fake_microseconds=1000000;
    fake_mesh_connected=false;
    fake_mesh_coordinator=false;
    fake_mesh_result=MESH_UNAVAILABLE;
    fake_shared_calls=0;
    mesh_stops=0;
    restarts=0;
    initialize_runtime();
}

int main(void)
{
    reset_backend();
    const uint64_t first_boot_domain=runtime.boot_domain;
    assert(first_boot_domain!=0 && first_boot_domain!=runtime.boot.epoch);
    initialize_runtime();
    assert(runtime.boot_domain!=0 && runtime.boot_domain!=first_boot_domain);
    const uint32_t saved_rng=fake_random_counter;
    fake_random_counter=UINT32_C(0x1234);
    const uint64_t device11_domain=new_boot_domain((EventId){11,1,1});
    fake_random_counter=UINT32_C(0x1234);
    const uint64_t device99_domain=new_boot_domain((EventId){99,1,1});
    fake_random_counter=saved_rng;
    assert(device11_domain!=device99_domain);
    reset_backend();
    /* A foreign device can have the same persisted boot epoch number. Its
     * milliseconds are not this device's monotonic clock. */
    fake_microseconds=2000000;
    const ClockSample domain_probe=clock_sample(runtime.boot_domain);
    AppState foreign_probe=runtime.state;
    AppEvent foreign_confirm={0};
    foreign_confirm.kind=EVENT_CONFIRM;
    foreign_confirm.id=(EventId){99,5,2};
    foreign_confirm.sender=99;
    foreign_confirm.round=foreign_probe.round;
    foreign_confirm.parent_revision=foreign_probe.revision;
    foreign_confirm.occurred_at=(ClockSample){1500,runtime.boot.epoch,0,false,CLOCK_UNAVAILABLE};
    foreign_confirm.coordinator_accepted=true;
    AppEffects domain_effects={0};
    assert(app_apply_event(&foreign_probe,&foreign_confirm,&domain_probe,
                           &domain_effects)==APP_TIME_UNAVAILABLE);
    UiRequest shared = {.kind=UI_REQUEST_SET_IMAGE_SECONDS};
    shared.value.seconds=3600;
    const unsigned offline_writes=writes;
    assert(submit_ui(&runtime,&shared)==UI_SUBMIT_UNAVAILABLE);
    assert(writes==offline_writes && fake_shared_calls==0);

    UiRequest confirm={.kind=UI_REQUEST_CONFIRM};
    fake_microseconds=2000000;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_PENDING);
    assert(!runtime.state.alarm_active && runtime.store.record.pending_count==1);

    runtime.mesh_started=true;
    fake_mesh_connected=true;
    fake_mesh_result=MESH_PENDING;
    const uint32_t old_seconds=runtime.state.settings.image_seconds;
    const unsigned before_peer=writes;
    assert(submit_ui(&runtime,&shared)==UI_SUBMIT_PENDING_SHARED);
    assert(runtime.shared_pending && fake_shared_calls==1);
    assert(runtime.state.settings.image_seconds==old_seconds);
    assert(writes==before_peer+1); /* Durable ID reservation, no settings commit. */
    assert(submit_ui(&runtime,&shared)==UI_SUBMIT_UNAVAILABLE);
    EventId wrong=runtime.shared_id; ++wrong.sequence;
    mesh_shared_result(&runtime,wrong,true);
    assert(runtime.shared_pending && runtime.shared_feedback==0);
    mesh_shared_result(&runtime,runtime.shared_id,false);
    assert(!runtime.shared_pending && runtime.shared_feedback==-1);
    assert(runtime.state.settings.image_seconds==old_seconds);
    runtime.shared_feedback=0;

    fake_mesh_coordinator=true;
    fake_mesh_result=MESH_ACCEPTED;
    assert(submit_ui(&runtime,&shared)==UI_SUBMIT_COMMITTED);
    assert(runtime.state.settings.image_seconds==3600);
    assert(runtime.store.record.shared.image_seconds==3600);
    assert(runtime.store.record.pending_count==1);

    const KatmedTime prior=runtime.time;
    const uint64_t old_revision=runtime.state.revision.number;
    mesh_utc_received(&runtime,UINT64_C(0),INT64_C(1791973800000),CLOCK_HOLDOVER);
    assert(runtime.time.domain==0 || !runtime.time.utc_known);
    katmed_time_init(&runtime.time,runtime.boot_domain);
    fake_microseconds=4000000;
    mesh_utc_received(&runtime,UINT64_C(3000),INT64_C(1791973800000),CLOCK_HOLDOVER);
    assert(runtime.time.utc_known && runtime.time.source_quality==CLOCK_HOLDOVER);
    assert(runtime.state.has_time_sync && runtime.state.revision.number==old_revision);
    assert(runtime.state.last_time_sync.quality==CLOCK_HOLDOVER);
    assert(runtime.time_sequence_end-runtime.time_sequence==1023);
    (void)prior;

    reset_backend();
    fake_microseconds=2000000;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_PENDING);
    assert(runtime.store.record.pending_count==1);
    assert(runtime.store.record.group_origin==STORE_GROUP_ORIGIN_FRESH_UNBOUND);
    const StorePendingConfirm local_event=runtime.store.record.pending[0];
    queue_provision(0x33,0x44);
    const unsigned pregroup_writes=writes;
    const ClockSample provision_time=clock_sample(runtime.boot_domain);
    process_usb(&runtime,&provision_time);
    assert(strcmp(last_reply,"KM1 OK PROVISION\n")==0);
    assert(writes==pregroup_writes+1 && mesh_stops==1 &&
           runtime.reboot_required && runtime.store.record.pending_count==1 &&
           runtime.store.record.group_origin==STORE_GROUP_ORIGIN_FIRST_BOUND &&
           runtime.store.record.pending[0].id.device==local_event.id.device &&
           runtime.store.record.pending[0].id.epoch==local_event.id.epoch &&
           runtime.store.record.pending[0].id.sequence==local_event.id.sequence &&
           runtime.store.record.pending[0].round.ordinal==local_event.round.ordinal &&
           runtime.store.record.pending[0].parent_revision.number==
               local_event.parent_revision.number &&
           runtime.store.record.pending[0].occurred_at.domain==
               local_event.occurred_at.domain &&
           runtime.state.alarm_active && runtime.state.start_reason==START_BOOT);

    reset_backend();
    queue_provision(0x33,0x44);
    const ClockSample first_setup=clock_sample(runtime.boot_domain);
    process_usb(&runtime,&first_setup);
    assert(strcmp(last_reply,"KM1 OK PROVISION\n")==0);
    assert(mesh_stops==1 && runtime.reboot_required && !runtime.mesh_started);
    reboot_if_due(&runtime,runtime.reboot_due_ms-1);
    assert(restarts==0);
    reboot_if_due(&runtime,runtime.reboot_due_ms);
    assert(restarts==1);

    initialize_runtime();
    katmed_time_init(&runtime.time,runtime.boot_domain);
    assert(katmed_time_accept_utc(&runtime.time,1000,INT64_C(1791973800000),
                                  CLOCK_SYNCHRONIZED));
    runtime.mesh_started=true;
    fake_mesh_connected=true;
    fake_mesh_coordinator=false;
    fake_microseconds=3000000;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_PENDING);
    assert(runtime.store.record.pending_count==1 && !runtime.state.alarm_active);
    queue_provision(0x55,0x66);
    const unsigned old_group_writes=writes;
    const ClockSample switch_time=clock_sample(runtime.boot_domain);
    process_usb(&runtime,&switch_time);
    assert(strcmp(last_reply,"KM1 ERR CONFIG\n")==0);
    assert(writes==old_group_writes && mesh_stops==1 &&
           !runtime.reboot_required && runtime.mesh_started &&
           runtime.store.record.pending_count==1 && !runtime.state.alarm_active);

    memcpy(incoming_line.data,"KM1 RESET CONFIG\n",17);
    incoming_line.length=17;
    incoming_line.overlong=false;
    line_ready=true;
    const ClockSample reset_time=clock_sample(runtime.boot_domain);
    process_usb(&runtime,&reset_time);
    assert(strcmp(last_reply,"KM1 OK RESET_CONFIG\n")==0);
    assert(mesh_stops==2 && runtime.reboot_required && !runtime.mesh_started &&
           !runtime.store.record.provision.configured &&
           runtime.store.record.pending_count==1 &&
           runtime.store.record.last_confirmation.valid &&
           runtime.state.alarm_active && runtime.state.start_reason==START_BOOT &&
           !runtime.time.utc_known);
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_UNAVAILABLE);

    initialize_runtime();
    queue_provision(0x33,0x77); /* Same group after reset; secret rotation. */
    const ClockSample same_group=clock_sample(runtime.boot_domain);
    process_usb(&runtime,&same_group);
    assert(strcmp(last_reply,"KM1 OK PROVISION\n")==0);
    assert(mesh_stops==3 && runtime.reboot_required &&
           runtime.store.record.pending_count==1 &&
           runtime.store.record.last_confirmation.valid);

    reset_backend();
    queue_provision(0x33,0x44);
    const ClockSample empty_first=clock_sample(runtime.boot_domain);
    process_usb(&runtime,&empty_first);
    assert(strcmp(last_reply,"KM1 OK PROVISION\n")==0);
    initialize_runtime();
    queue_provision(0x55,0x66); /* Empty group can switch without data loss. */
    const ClockSample empty_switch=clock_sample(runtime.boot_domain);
    process_usb(&runtime,&empty_switch);
    assert(strcmp(last_reply,"KM1 OK PROVISION\n")==0);
    assert(mesh_stops==2 && runtime.reboot_required &&
           runtime.state.alarm_active && runtime.state.start_reason==START_BOOT &&
           runtime.store.record.pending_count==0);

    reset_backend();
    const AlarmRoundId old_round=runtime.state.round;
    const StateRevision root_revision=runtime.state.revision;
    fake_microseconds=2000000;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_PENDING);
    fake_microseconds=3000000;
    ClockSample conflict_now=clock_sample(runtime.boot_domain);
    StorePendingConfirm rejected={{99,5,2},99,old_round,root_revision,
                                  {1500,runtime.boot.epoch,0,false,CLOCK_UNAVAILABLE}};
    assert(store_record_conflict(&runtime.store,&runtime.state,&rejected,
                                 &conflict_now)==STORE_OK);
    assert(durable_divergence(&runtime) && conflict_count(&runtime)==1);
    UiConflict visible={0};
    assert(conflict_at(&runtime,0,&visible) && visible.id.device==99 &&
           visible.round.ordinal==old_round.ordinal &&
           visible.occurred_at.monotonic_ms==1500);
    puts("main mesh/service integration passed");
}
