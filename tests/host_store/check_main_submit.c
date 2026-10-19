#include <assert.h>
#include <stdio.h>
#include <string.h>

#define KATMED_FULL_STACK 1
#define KATMED_WITH_CORE 1
#include "../../firmware/main/main.c"

static uint8_t committed[KATMED_STORE_BLOB_MAX];
static size_t committed_length;
static unsigned writes;
static bool fail_commit;
static int64_t fake_microseconds;
static Orientation board_orientation;
static bool fake_mesh_connected;
static bool fake_mesh_coordinator;
static KatmedMeshResult fake_mesh_result = MESH_UNAVAILABLE;
static unsigned fake_shared_calls;
static AppEvent fake_shared_event;
static uint32_t fake_random_counter=UINT32_C(0x12345678);

uint32_t esp_random(void) { return ++fake_random_counter; }

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
    ++writes;
    if (fail_commit) return STORE_COMMIT_UNKNOWN;
    memcpy(committed, data, length);
    committed_length = length;
    return STORE_OK;
}
int64_t esp_timer_get_time(void) { return fake_microseconds; }
BoardResult board_set_orientation(Orientation o) { board_orientation=o; return BOARD_OK; }
bool katmed_mesh_connected(void) { return fake_mesh_connected; }
bool katmed_mesh_is_coordinator(void) { return fake_mesh_coordinator; }
void katmed_mesh_notify_local_commit(void) {}
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

int main(void)
{
    fake_microseconds=1000000;
    initialize_runtime();
    assert(runtime.state.alarm_active && !runtime.state.has_last_confirmation);
    const unsigned before=writes;
    UiRequest shared = {.kind=UI_REQUEST_SET_MEDICATION_SECONDS};
    shared.value.seconds=3600;
    assert(submit_ui(&runtime,&shared)==UI_SUBMIT_UNAVAILABLE && writes==before);
    UiRequest confirm = {.kind=UI_REQUEST_CONFIRM};
    fake_microseconds=2000000;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_PENDING);
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
    fail_commit=true;
    const AppState before_state=runtime.state;
    fake_microseconds+=1000000;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_UNAVAILABLE);
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
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_PENDING);
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
    restore_manual_if_ready(&runtime,&restore_now);
    assert(runtime.state.alarm_active && runtime.store.record.manual_intent.pending);
    fake_microseconds+=1000000;
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_COMMITTED);
    restore_now=clock_sample(runtime.boot_domain);
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
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_COMMITTED);
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
    assert(submit_ui(&runtime,&confirm)==UI_SUBMIT_PENDING);
    delayed_now=clock_sample(runtime.boot_domain);
    restore_manual_if_ready(&runtime,&delayed_now);
    assert(runtime.manual_restore_waiting && runtime.shared_pending &&
           runtime.store.record.manual_intent.pending);
    mesh_shared_result(&runtime,fake_shared_event.id,false);
    restore_manual_if_ready(&runtime,&delayed_now);
    assert(!runtime.manual_restore_waiting && !runtime.manual_restore_rejected &&
           runtime.manual_restore_status==KATMED_RESTORE_CONFLICT &&
           !runtime.store.record.manual_intent.pending);
    initialize_runtime();
    assert(!runtime.store.record.manual_intent.pending); /* Delayed rejection durable. */
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
    const unsigned committed_settings=writes;
    fake_mesh_connected=false;
    fake_mesh_coordinator=false; /* Disconnected follower must not self-authorize. */
    image.value.seconds=3600;
    assert(submit_ui(&runtime,&image)==UI_SUBMIT_UNAVAILABLE && writes==committed_settings);
    fake_mesh_coordinator=true;
    runtime.mesh_started=false; /* Unstarted/unconfigured runtime stays blocked. */
    assert(submit_ui(&runtime,&image)==UI_SUBMIT_UNAVAILABLE && writes==committed_settings);
    runtime.mesh_started=true;
    fail_commit=true;
    const AppState stable=runtime.state;
    assert(submit_ui(&runtime,&image)==UI_SUBMIT_UNAVAILABLE);
    assert(runtime.state.settings.image_seconds==stable.settings.image_seconds &&
           runtime.state.revision.number==stable.revision.number);
    puts("main solo-root settings: image persistence, next-time history, follower/unstarted and commit failure OK");
    puts("main submit/reboot regression passed");
    return 0;
}
