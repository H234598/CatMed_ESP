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
static unsigned fake_mesh_notifications;
static AppEvent fake_shared_event;
static uint32_t fake_random_counter=UINT32_C(0x12345678);
static bool reset_marker;

uint32_t esp_random(void) { return ++fake_random_counter; }
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
    runtime.reset_web_offer=false;
    assert(katmed_runtime_reset_arm(&runtime) && reset_marker);
    assert(katmed_runtime_reset_confirm(&runtime,"stale",5)==1 && !runtime.reset_requested);
    assert(katmed_runtime_reset_cancel(&runtime) && !reset_marker);
    assert(katmed_reset_idf_take_preparation(&runtime.reset_warning_ready) &&
           !runtime.reset_warning_ready);
    assert(katmed_runtime_reset_arm(&runtime) && reset_marker);
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
    puts("main solo-root settings: image persistence, next-time history, follower/unstarted and commit failure OK");
    puts("physical reset runtime: offer, cancel, one-boot marker, warning, fresh one-use confirmation OK");
    puts("main submit/reboot regression passed");
    return 0;
}
