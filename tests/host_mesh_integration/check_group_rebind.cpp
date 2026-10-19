#include "katmed_store.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <zlib.h>

struct Backend {
    uint8_t bytes[KATMED_STORE_BLOB_MAX]{};
    size_t length{};
    unsigned commits{};
    bool fail{};
};
static StoreResult read_record(void *context, uint8_t *bytes, size_t *length) {
    auto &b=*static_cast<Backend *>(context);
    if (!b.length) return STORE_NOT_FOUND;
    if (*length<b.length) return STORE_CORRUPT;
    std::memcpy(bytes,b.bytes,b.length); *length=b.length; return STORE_OK;
}
static StoreResult write_record(void *context,const uint8_t *bytes,size_t length) {
    auto &b=*static_cast<Backend *>(context); ++b.commits;
    if (b.fail) return STORE_COMMIT_UNKNOWN;
    assert(length<=sizeof(b.bytes));
    std::memcpy(b.bytes,bytes,length); b.length=length; return STORE_OK;
}
static std::string repeated_hex(unsigned char value,size_t count) {
    static const char digits[]="0123456789abcdef";
    std::string result;
    for (size_t i=0;i<count;++i) { result+=digits[value>>4]; result+=digits[value&15]; }
    return result;
}
static std::string text_hex(const char *text) {
    static const char digits[]="0123456789abcdef";
    std::string result;
    for (const unsigned char *p=reinterpret_cast<const unsigned char *>(text);*p;++p) {
        result+=digits[*p>>4]; result+=digits[*p&15];
    }
    return result;
}
static StoreResult provision(Store &store,unsigned char group,unsigned char secret,
                             const char *ssid="Home",std::string *response=nullptr,
                             const char *password="12345678",
                             const char *time_source="pool.ntp.org") {
    std::string command="KM1 PROVISION group="+repeated_hex(group,16)+
        " secret="+repeated_hex(secret,32)+" ssid="+text_hex(ssid)+
        " password="+text_hex(password)+" time="+text_hex(time_source)+"\n";
    char reply[256]{};
    StoreResult result=store_usb_handle_line(&store,command.c_str(),command.size(),nullptr,reply,sizeof(reply));
    if(response)*response=reply;
    return result;
}
static ClockSample sample(uint64_t domain,uint64_t ms) {
    return ClockSample{ms,domain,0,false,CLOCK_UNAVAILABLE};
}
static void downgrade_to_v5(Backend &backend, unsigned wifi_profile_count) {
    constexpr size_t v7_manual_suffix=77;
    constexpr size_t v9_mesh_name_suffix=1+17+2+1+KATMED_MESH_NAME_MAX_ENTRIES*10+4;
    assert(backend.length>99 && backend.bytes[4]==10);
    /* The serializer appends the count and bounded profile rows after mesh. */
    assert(wifi_profile_count<=KATMED_STORE_WIFI_PROFILE_MAX);
    const size_t wifi_suffix=5u+static_cast<size_t>(wifi_profile_count)*103u;
    assert(backend.length>wifi_suffix &&
           backend.bytes[backend.length-wifi_suffix]==wifi_profile_count);
    backend.length-=wifi_suffix;
    backend.bytes[4]=9;
    backend.length-=v9_mesh_name_suffix+v7_manual_suffix;
    assert(backend.length>99 && backend.bytes[4]==9);
    --backend.length; /* v6 appends the durable group-origin byte. */
    backend.bytes[4]=5; backend.bytes[5]=0;
    backend.bytes[6]=static_cast<uint8_t>(backend.length);
    backend.bytes[7]=static_cast<uint8_t>(backend.length>>8);
    const uint32_t crc=static_cast<uint32_t>(crc32(0,backend.bytes+12,backend.length-12));
    for(unsigned i=0;i<4;++i) backend.bytes[8+i]=static_cast<uint8_t>(crc>>(8*i));
}
static void downgrade_to_v4(Backend &backend,unsigned wifi_profile_count) {
    downgrade_to_v5(backend,wifi_profile_count);
    /* v5 inserts a zero conflict count and divergence flag before provision. */
    constexpr size_t provision_size=210;
    const size_t count_at=backend.length-83-16-provision_size-2;
    assert(backend.bytes[count_at]==0 && backend.bytes[count_at+1]==0);
    std::memmove(backend.bytes+count_at,backend.bytes+count_at+2,
                 backend.length-count_at-2);
    backend.length-=2;
    backend.bytes[4]=4;backend.bytes[5]=0;
    backend.bytes[6]=static_cast<uint8_t>(backend.length);
    backend.bytes[7]=static_cast<uint8_t>(backend.length>>8);
    const uint32_t v4_crc=static_cast<uint32_t>(crc32(0,backend.bytes+12,backend.length-12));
    for(unsigned i=0;i<4;++i)backend.bytes[8+i]=static_cast<uint8_t>(v4_crc>>(8*i));
}
static void downgrade_to_v3(Backend &backend,unsigned wifi_profile_count) {
    downgrade_to_v4(backend,wifi_profile_count);
    const size_t bound_at=backend.length-83-16;
    std::memmove(backend.bytes+bound_at,backend.bytes+bound_at+16,83);
    backend.length-=16;
    backend.bytes[4]=3;backend.bytes[5]=0;
    backend.bytes[6]=static_cast<uint8_t>(backend.length);
    backend.bytes[7]=static_cast<uint8_t>(backend.length>>8);
    const uint32_t crc=static_cast<uint32_t>(crc32(0,backend.bytes+12,backend.length-12));
    for(unsigned i=0;i<4;++i)backend.bytes[8+i]=static_cast<uint8_t>(crc>>(8*i));
}
static bool bound_to(const Store &store,unsigned char group) {
    for(uint8_t byte:store.record.bound_group)if(byte!=group)return false;
    return true;
}
static bool same_id(EventId a,EventId b) {
    return a.device==b.device && a.epoch==b.epoch && a.sequence==b.sequence;
}
static bool same_round(AlarmRoundId a,AlarmRoundId b) {
    return same_id(a.origin,b.origin) && a.ordinal==b.ordinal;
}
static bool same_clock(ClockSample a,ClockSample b) {
    return a.monotonic_ms==b.monotonic_ms && a.domain==b.domain &&
           a.utc_ms==b.utc_ms && a.utc_valid==b.utc_valid && a.quality==b.quality;
}
static void check_fresh_current_offline_first_provision() {
    constexpr uint64_t device=UINT64_C(0xABCDEF123456);
    Backend backend{};
    Store store{}; StoreRecovery recovery{};
    uint8_t catalog[KATMED_CATALOG_BYTES]={1,2,3};
    const StoreBackend io{&backend,read_record,write_record};
    assert(store_open(&store,io,device,catalog,75,&recovery)==STORE_OK);
    assert(backend.bytes[4]==10 && backend.commits==1 && !recovery.configured &&
           recovery.pending_count==0 && bound_to(store,0) &&
           store.record.group_origin==STORE_GROUP_ORIGIN_FRESH_UNBOUND);

    ClockSample now=sample(recovery.boot_identity.epoch,1000);
    AppState active{};
    assert(app_init(&active,&recovery.shared,&now,recovery.boot_identity,75,catalog)==APP_OK);
    assert(active.alarm_active && active.start_reason==START_BOOT);
    EventId original{};
    assert(store_next_event_id(&store,&original)==STORE_OK);
    now.monotonic_ms=2000;
    AppEvent confirm{};
    confirm.kind=EVENT_CONFIRM; confirm.id=original; confirm.sender=device;
    confirm.round=active.round; confirm.parent_revision=active.revision;
    confirm.occurred_at=now;
    AppEffects effects{}; AppResult core=APP_INVALID;
    assert(store_apply_event(&store,&active,&confirm,&now,true,&effects,&core)==STORE_OK);
    assert(core==APP_OK && !active.alarm_active && store.record.pending_count==1 &&
           store.record.last_confirmation.valid && bound_to(store,0));

    Store durable{}; StoreRecovery durable_recovery{};
    assert(store_open(&durable,io,device,catalog,75,&durable_recovery)==STORE_OK);
    assert(backend.bytes[4]==10 && durable_recovery.pending_count==1 &&
           durable_recovery.has_last_confirmation && bound_to(durable,0) &&
           durable.record.group_origin==STORE_GROUP_ORIGIN_FRESH_UNBOUND);
    const StorePendingConfirm pending=durable.record.pending[0];
    const StoreConfirmationHistory confirmation_before=durable.record.last_confirmation;
    const AppState history_before=durable.record.history;
    assert(pending.id.device==original.device && pending.id.epoch==original.epoch &&
           pending.id.sequence==original.sequence && pending.sender==original.device &&
           durable_recovery.last_confirmation_id.device==original.device &&
           durable_recovery.last_confirmation_id.epoch==original.epoch &&
           durable_recovery.last_confirmation_id.sequence==original.sequence);

    Backend old_v5=backend;
    downgrade_to_v5(old_v5,0);
    Store old_v5_store{}; StoreRecovery old_v5_recovery{};
    assert(store_open(&old_v5_store,{&old_v5,read_record,write_record},device,
                      catalog,75,&old_v5_recovery)==STORE_OK);
    assert(old_v5_store.record.group_origin==STORE_GROUP_ORIGIN_UNKNOWN &&
           old_v5_store.record.pending_count==1);
    std::string old_reply;
    const unsigned old_writes=old_v5.commits;
    assert(provision(old_v5_store,0x5a,0xa5,"SYNTHETIC_V5",&old_reply)==STORE_STALE &&
           old_reply=="KM1 ERR CONFIG\n" && old_v5.commits==old_writes);
    char old_reset_reply[256]{};
    assert(store_usb_handle_line(&old_v5_store,"KM1 RESET CONFIG\n",17,nullptr,
                                 old_reset_reply,sizeof(old_reset_reply))==STORE_OK);
    Store old_v5_after_reset{}; StoreRecovery old_v5_reset_recovery{};
    assert(store_open(&old_v5_after_reset,{&old_v5,read_record,write_record},device,
                      catalog,75,&old_v5_reset_recovery)==STORE_OK);
    assert(old_v5_after_reset.record.group_origin==STORE_GROUP_ORIGIN_UNKNOWN &&
           old_v5_after_reset.record.pending_count==1);
    const unsigned old_reset_writes=old_v5.commits;
    assert(provision(old_v5_after_reset,0x5a,0xa5,"SYNTHETIC_V5",&old_reply)==STORE_STALE &&
           old_v5.commits==old_reset_writes);

    Backend failed=backend;
    Store failed_store{}; StoreRecovery failed_recovery{};
    assert(store_open(&failed_store,{&failed,read_record,write_record},device,
                      catalog,75,&failed_recovery)==STORE_OK);
    failed.fail=true;
    std::string failed_reply;
    assert(provision(failed_store,0x5a,0xa5,"SYNTHETIC_V5",&failed_reply)==
           STORE_COMMIT_UNKNOWN);
    assert(failed_reply=="KM1 ERR COMMIT\n" && failed_store.uncertain &&
           failed_store.record.group_origin==STORE_GROUP_ORIGIN_FRESH_UNBOUND);
    failed.fail=false;
    Store failed_reopen{}; StoreRecovery failed_after{};
    assert(store_open(&failed_reopen,{&failed,read_record,write_record},device,
                      catalog,75,&failed_after)==STORE_OK);
    assert(failed_reopen.record.group_origin==STORE_GROUP_ORIGIN_FRESH_UNBOUND &&
           failed_reopen.record.pending_count==1 && bound_to(failed_reopen,0));

    std::string reply;
    const unsigned before_provision=backend.commits;
    const StoreResult result=provision(durable,0x5a,0xa5,"SYNTHETIC_V5",&reply,
                                       "synthetic-pass-123","ntp.example.invalid");
    const unsigned after_provision=backend.commits;
    const unsigned provision_commits=backend.commits-before_provision;
    const bool after_bound_to_zero=bound_to(durable,0);
    const bool after_bound_to_requested=bound_to(durable,0x5a);
    assert(durable.record.pending_count==1 &&
           durable.record.pending[0].id.device==original.device &&
           durable.record.pending[0].id.epoch==original.epoch &&
           durable.record.pending[0].id.sequence==original.sequence);
    Store after_attempt{}; StoreRecovery after_attempt_recovery{};
    assert(store_open(&after_attempt,io,device,catalog,75,&after_attempt_recovery)==STORE_OK);
    assert(after_attempt_recovery.pending_count==1 &&
           after_attempt.record.pending[0].id.device==original.device &&
           after_attempt.record.pending[0].id.epoch==original.epoch &&
           after_attempt.record.pending[0].id.sequence==original.sequence);
    std::fprintf(stderr,
                 "fresh v10 offline first PROVISION: result=%d reply=%s"
                 "commits_before=%u commits_after=%u commits_after_reopen=%u "
                 "commits_during_provision=%u pending=%u id=%llx/%llu/%llu "
                 "bound_before=zero bound_after_zero=%d bound_after_requested=%d "
                 "last_error=%d\n",
                 static_cast<int>(result),reply.c_str(),before_provision,
                 after_provision,backend.commits,provision_commits,
                 static_cast<unsigned>(after_attempt.record.pending_count),
                 static_cast<unsigned long long>(original.device),
                 static_cast<unsigned long long>(original.epoch),
                 static_cast<unsigned long long>(original.sequence),
                 after_bound_to_zero,after_bound_to_requested,
                 static_cast<int>(durable.last_error));
    assert(result==STORE_OK && reply=="KM1 OK PROVISION\n" &&
           provision_commits==1 && after_bound_to_requested &&
           !after_bound_to_zero && bound_to(after_attempt,0x5a) &&
           after_attempt.record.provision.configured &&
           durable.record.group_origin==STORE_GROUP_ORIGIN_FIRST_BOUND &&
           after_attempt.record.group_origin==STORE_GROUP_ORIGIN_FIRST_BOUND &&
           same_id(pending.id,after_attempt.record.pending[0].id) &&
           pending.sender==after_attempt.record.pending[0].sender &&
           same_round(pending.round,after_attempt.record.pending[0].round) &&
           pending.parent_revision.number==after_attempt.record.pending[0].parent_revision.number &&
           pending.parent_revision.coordinator==after_attempt.record.pending[0].parent_revision.coordinator &&
           same_clock(pending.occurred_at,after_attempt.record.pending[0].occurred_at) &&
           confirmation_before.valid && after_attempt.record.last_confirmation.valid &&
           same_id(confirmation_before.id,after_attempt.record.last_confirmation.id) &&
           same_round(confirmation_before.round,after_attempt.record.last_confirmation.round) &&
           same_clock(confirmation_before.occurred_at,
                      after_attempt.record.last_confirmation.occurred_at) &&
           same_id(history_before.last_confirmation_id,
                   after_attempt.record.history.last_confirmation_id) &&
           same_round(history_before.last_confirmed_round,
                      after_attempt.record.history.last_confirmed_round) &&
           same_clock(history_before.last_confirmation,
                      after_attempt.record.history.last_confirmation) &&
           history_before.recent_event_count==after_attempt.record.history.recent_event_count &&
           history_before.recent_event_next==after_attempt.record.history.recent_event_next);
    char reset_reply[256]{};
    assert(store_usb_handle_line(&after_attempt,"KM1 RESET CONFIG\n",17,nullptr,
                                 reset_reply,sizeof(reset_reply))==STORE_OK);
    Store after_reset{}; StoreRecovery reset_recovery{};
    assert(store_open(&after_reset,io,device,catalog,75,&reset_recovery)==STORE_OK);
    assert(after_reset.record.group_origin==STORE_GROUP_ORIGIN_FIRST_BOUND &&
           !after_reset.record.provision.configured && bound_to(after_reset,0x5a) &&
           after_reset.record.pending_count==1);
    const unsigned before_wrong_group=backend.commits;
    assert(provision(after_reset,0x33,0x44,"Other",&reply)==STORE_STALE &&
           reply=="KM1 ERR CONFIG\n" && backend.commits==before_wrong_group);
    assert(provision(after_reset,0x5a,0x66,"Again",&reply)==STORE_OK &&
           after_reset.record.pending_count==1 && bound_to(after_reset,0x5a));
}
int main() {
    constexpr uint64_t device=UINT64_C(0x112233445566);
    Backend backend{};
    Store store{};
    StoreRecovery recovery{};
    uint8_t catalog[KATMED_CATALOG_BYTES]={1,2,3};
    StoreBackend io{&backend,read_record,write_record};
    assert(store_open(&store,io,device,catalog,75,&recovery)==STORE_OK);
    assert(provision(store,0x11,0x22)==STORE_OK);
    assert(bound_to(store,0x11));
    EventId first_block{};
    assert(store_reserve_event_ids(&store,1024,&first_block)==STORE_OK);
    assert(first_block.device==device && first_block.sequence==2);
    assert(store_reserve_event_ids(&store,0,&first_block)==STORE_INVALID);
    LocalSettings local=recovery.local;
    LocalSettings desired=local; desired.orientation=ORIENTATION_270;
    desired.clock_style=ANALOG;
    assert(store_set_local(&store,&local,&desired)==STORE_OK);
    ClockSample now=sample(recovery.boot_identity.epoch,1000);
    AppState active{};
    assert(app_init(&active,&recovery.shared,&now,recovery.boot_identity,75,catalog)==APP_OK);
    EventId id{};
    assert(store_next_event_id(&store,&id)==STORE_OK);
    assert(id.sequence==1026);
    now.monotonic_ms=2000;
    AppEvent confirm{};
    confirm.kind=EVENT_CONFIRM; confirm.id=id; confirm.sender=id.device;
    confirm.round=active.round; confirm.parent_revision=active.revision;
    confirm.occurred_at=now;
    AppEffects effects{}; AppResult core=APP_INVALID;
    assert(store_apply_event(&store,&active,&confirm,&now,true,&effects,&core)==STORE_OK);
    assert(store.record.pending_count==1 && store.record.has_history &&
           store.record.last_confirmation.valid && !active.alarm_active);
    const StateRevision old_revision=store.record.shared.revision;
    assert(provision(store,0x11,0x22,"Other")==STORE_OK);
    assert(store.record.pending_count==1 && store.record.has_history &&
           store.record.shared.revision.number==old_revision.number);
    std::string reply;
    const unsigned blocked_commits=backend.commits;
    assert(provision(store,0x33,0x44,"Home",&reply)==STORE_STALE);
    assert(reply=="KM1 ERR CONFIG\n" && backend.commits==blocked_commits &&
           bound_to(store,0x11) && store.record.pending_count==1 &&
           store.record.last_confirmation.valid);
    backend.fail=true;
    assert(provision(store,0x11,0x22,"Again")==STORE_COMMIT_UNKNOWN);
    assert(store.record.pending_count==1 && store.record.has_history);
    backend.fail=false;
    Store reopened{}; StoreRecovery after_uncertain{};
    assert(store_open(&reopened,io,device,catalog,75,&after_uncertain)==STORE_OK);

    Backend v4_configured=backend;
    downgrade_to_v4(v4_configured,1);
    Store v4_migrated{};StoreRecovery v4_recovery{};
    assert(store_open(&v4_migrated,{&v4_configured,read_record,write_record},device,
                      catalog,75,&v4_recovery)==STORE_OK);
    assert(bound_to(v4_migrated,0x11) && v4_migrated.record.pending_count==1 &&
           v4_migrated.record.conflict_count==0 && !v4_migrated.record.group_divergent);

    /* A configured v3 record identifies its real group during v4 migration. */
    Backend v3_configured=backend;
    downgrade_to_v3(v3_configured,1);
    Store migrated{};StoreRecovery migrated_recovery{};
    assert(store_open(&migrated,{&v3_configured,read_record,write_record},device,
                      catalog,75,&migrated_recovery)==STORE_OK);
    assert(bound_to(migrated,0x11) && migrated.record.pending_count==1 &&
           migrated.record.last_confirmation.valid);
    assert(provision(migrated,0x11,0x55)==STORE_OK); /* Same group's key rotation. */
    assert(bound_to(migrated,0x11) && migrated.record.pending_count==1);

    /* Reset removes credentials only. A later same-group setup keeps history. */
    char reply_buffer[256]{};
    assert(store_usb_handle_line(&reopened,"KM1 RESET CONFIG\n",17,nullptr,
                                 reply_buffer,sizeof(reply_buffer))==STORE_OK);
    assert(!reopened.record.provision.configured && bound_to(reopened,0x11) &&
           reopened.record.pending_count==1 && reopened.record.has_history &&
           reopened.record.last_confirmation.valid &&
           reopened.record.local.orientation==ORIENTATION_270 &&
           reopened.record.local.clock_style==ANALOG);
    Backend v3_after_reset=backend;
    downgrade_to_v3(v3_after_reset,1);
    Store legacy_unknown{};StoreRecovery legacy_recovery{};
    assert(store_open(&legacy_unknown,{&v3_after_reset,read_record,write_record},device,
                      catalog,75,&legacy_recovery)==STORE_OK);
    assert(!bound_to(legacy_unknown,0x11) && legacy_unknown.record.pending_count==1);
    const unsigned legacy_commits=v3_after_reset.commits;
    assert(provision(legacy_unknown,0x11,0x22,"Home",&reply)==STORE_STALE);
    assert(reply=="KM1 ERR CONFIG\n" && v3_after_reset.commits==legacy_commits);

    assert(provision(reopened,0x11,0x66)==STORE_OK);
    assert(reopened.record.pending_count==1 && reopened.record.has_history &&
           reopened.record.last_confirmation.valid && bound_to(reopened,0x11));
    const unsigned switched_commits=backend.commits;
    assert(provision(reopened,0x33,0x44,"Home",&reply)==STORE_STALE);
    assert(reply=="KM1 ERR CONFIG\n" && backend.commits==switched_commits);
    Store restart{}; StoreRecovery fresh{};
    assert(store_open(&restart,io,device,catalog,75,&fresh)==STORE_OK);
    assert(fresh.pending_count==1 && fresh.has_last_confirmation &&
           fresh.local.orientation==ORIENTATION_270 && fresh.local.clock_style==ANALOG);
    now=sample(fresh.boot_identity.epoch,3000);
    assert(app_init(&active,&fresh.shared,&now,fresh.boot_identity,75,catalog)==APP_OK);
    assert(active.alarm_active && active.start_reason==START_BOOT);

    /* All fixed-size fields are present here; the v10 mesh registry plus
     * bounded Wi-Fi profile header are included in the storage regression. */
    Backend maximal=backend;
    Store max_store{};StoreRecovery max_recovery{};
    assert(store_open(&max_store,{&maximal,read_record,write_record},device,
                      catalog,75,&max_recovery)==STORE_OK);
    for(unsigned i=1;i<KATMED_STORE_PENDING_MAX;++i){
        max_store.record.pending[i]=max_store.record.pending[0];
        max_store.record.pending[i].id.sequence+=i;
    }
    max_store.record.pending_count=KATMED_STORE_PENDING_MAX;
    EventId max_id{};
    assert(store_next_event_id(&max_store,&max_id)==STORE_OK);
    assert(maximal.length<=KATMED_STORE_BLOB_MAX && maximal.length==2907);
    max_store.record.history.divergent=true;
    max_store.record.group_divergent=true;
    for(unsigned i=0;i<KATMED_STORE_CONFLICT_MAX;++i) {
        max_store.record.conflicts[i]=max_store.record.pending[i];
        max_store.record.conflicts[i].id.device=99;
        max_store.record.conflicts[i].sender=99;
    }
    max_store.record.conflict_count=KATMED_STORE_CONFLICT_MAX;
    assert(store_next_event_id(&max_store,&max_id)==STORE_OK);
    assert(maximal.length==3755 && maximal.length<KATMED_STORE_BLOB_MAX);
    Store full_restart{};StoreRecovery full_recovery{};
    assert(store_open(&full_restart,{&maximal,read_record,write_record},device,
                      catalog,75,&full_recovery)==STORE_OK);
    assert(full_restart.record.pending_count==8 && full_restart.record.conflict_count==8 &&
           full_restart.record.history.divergent && full_restart.record.group_divergent);
    AppState full_active{};
    ClockSample full_now=sample(full_recovery.boot_identity.epoch,3000);
    assert(app_init(&full_active,&full_recovery.shared,&full_now,
                    full_recovery.boot_identity,75,catalog)==APP_OK);
    const unsigned full_writes=maximal.commits;
    assert(store_reject_pending(&full_restart,&full_active,
                                full_restart.record.pending[0].id)==STORE_FULL);
    assert(full_restart.record.pending_count==8 && maximal.commits==full_writes);

    /* Two real confirmations of the same alarm: the second is durable,
     * explicitly nonapplicable history, not a new medication transition. */
    Backend conflict_backend{};Store conflict_store{};StoreRecovery conflict_recovery{};
    assert(store_open(&conflict_store,{&conflict_backend,read_record,write_record},device,
                      catalog,75,&conflict_recovery)==STORE_OK);
    assert(provision(conflict_store,0x11,0x22)==STORE_OK);
    ClockSample conflict_now=sample(conflict_recovery.boot_identity.epoch,1000);
    AppState root{};
    assert(app_init(&root,&conflict_recovery.shared,&conflict_now,
                    conflict_recovery.boot_identity,75,catalog)==APP_OK);
    const AlarmRoundId original_round=root.round;
    const StateRevision original_revision=root.revision;
    AppEvent winner{};
    winner.kind=EVENT_CONFIRM;winner.id={99,5,2};winner.sender=99;
    winner.round=original_round;winner.parent_revision=original_revision;
    winner.occurred_at=sample(conflict_now.domain,2000);
    winner.coordinator_accepted=true;
    conflict_now.monotonic_ms=3000;
    assert(store_apply_event(&conflict_store,&root,&winner,&conflict_now,false,
                             &effects,&core)==STORE_OK);
    const AlarmRoundId next_round=root.round;
    const uint64_t deadline=root.deadline.effective_monotonic_ms;
    StorePendingConfirm loser{{100,5,2},100,original_round,original_revision,
                              sample(conflict_now.domain,1500)};
    const unsigned before_conflict=conflict_backend.commits;
    assert(store_record_conflict(&conflict_store,&root,&loser,&conflict_now)==STORE_OK);
    assert(conflict_backend.commits==before_conflict+1 && root.divergent &&
           root.round.ordinal==next_round.ordinal &&
           root.deadline.effective_monotonic_ms==deadline &&
           conflict_store.record.conflict_count==1);
    uint8_t conflict_count=0;StorePendingConfirm read_conflict{};
    assert(store_conflict_count(&conflict_store,&conflict_count)==STORE_OK && conflict_count==1);
    assert(store_conflict_at(&conflict_store,0,&read_conflict)==STORE_OK &&
           read_conflict.id.device==100 && read_conflict.occurred_at.monotonic_ms==1500);
    assert(store_record_conflict(&conflict_store,&root,&loser,&conflict_now)==STORE_DUPLICATE &&
           conflict_backend.commits==before_conflict+1);
    loser.occurred_at.monotonic_ms=1600;
    assert(store_record_conflict(&conflict_store,&root,&loser,&conflict_now)==STORE_STALE);
    loser.occurred_at.monotonic_ms=1500;
    assert(store_mark_divergent(&conflict_store,&root)==STORE_DUPLICATE);
    conflict_backend.fail=true;
    loser.id.sequence=3;
    assert(store_record_conflict(&conflict_store,&root,&loser,&conflict_now)==STORE_COMMIT_UNKNOWN);
    assert(conflict_store.record.conflict_count==1 &&
           store_conflict_count(&conflict_store,&conflict_count)==STORE_UNAVAILABLE);
    conflict_backend.fail=false;
    Store persisted{};StoreRecovery persisted_recovery{};
    assert(store_open(&persisted,{&conflict_backend,read_record,write_record},device,
                      catalog,75,&persisted_recovery)==STORE_OK);
    assert(store_conflict_count(&persisted,&conflict_count)==STORE_OK && conflict_count==1 &&
           store_durable_divergent(&persisted));
    const unsigned conflict_commits=conflict_backend.commits;
    assert(store_usb_handle_line(&persisted,"KM1 RESET CONFIG\n",17,nullptr,
                                 reply_buffer,sizeof(reply_buffer))==STORE_OK);
    assert(persisted.record.conflict_count==1 && !persisted.record.provision.configured &&
           store_durable_divergent(&persisted));
    assert(provision(persisted,0x33,0x44,"Home",&reply)==STORE_STALE &&
           reply=="KM1 ERR CONFIG\n" && conflict_backend.commits==conflict_commits+1);

    /* ACK 2 on a restarted leaf keeps its historical confirmation while
     * atomically moving only the named pending event into its own journal. */
    const EventId rejected=reopened.record.pending[0].id;
    const StoreConfirmationHistory retained=reopened.record.last_confirmation;
    Backend reject_uncertain_backend=backend;
    Store reject_uncertain{};StoreRecovery reject_uncertain_recovery{};
    assert(store_open(&reject_uncertain,
                      {&reject_uncertain_backend,read_record,write_record},device,
                      catalog,75,&reject_uncertain_recovery)==STORE_OK);
    ClockSample reject_now=sample(reject_uncertain_recovery.boot_identity.epoch,1000);
    AppState reject_state{};
    assert(app_init(&reject_state,&reject_uncertain_recovery.shared,&reject_now,
                    reject_uncertain_recovery.boot_identity,75,catalog)==APP_OK);
    reject_uncertain_backend.fail=true;
    assert(store_reject_pending(&reject_uncertain,&reject_state,rejected)==STORE_COMMIT_UNKNOWN);
    assert(reject_uncertain.record.pending_count==1 &&
           reject_uncertain.record.conflict_count==0 && !reject_state.divergent);
    reject_uncertain_backend.fail=false;
    Store reject_reopened{};StoreRecovery reject_recovery{};
    assert(store_open(&reject_reopened,
                      {&reject_uncertain_backend,read_record,write_record},device,
                      catalog,75,&reject_recovery)==STORE_OK);
    assert(reject_reopened.record.pending_count==1 &&
           reject_reopened.record.conflict_count==0);
    assert(store_reject_pending(&reopened,&active,rejected)==STORE_OK);
    assert(reopened.record.pending_count==0 && reopened.record.conflict_count==1 &&
           reopened.record.last_confirmation.valid &&
           reopened.record.last_confirmation.id.sequence==retained.id.sequence &&
           reopened.record.history.divergent && active.divergent);
    assert(store_reject_pending(&reopened,&active,rejected)==STORE_DUPLICATE);
    Store leaf_restart{};StoreRecovery leaf_recovery{};
    assert(store_open(&leaf_restart,io,device,catalog,75,&leaf_recovery)==STORE_OK);
    assert(leaf_restart.record.pending_count==0 && leaf_restart.record.conflict_count==1 &&
           leaf_recovery.has_last_confirmation && store_durable_divergent(&leaf_restart));

    /* An incomparable reunion is only a durable warning. No fabricated
     * rejection or ACK is allowed; a cold boot keeps the warning. */
    Backend warning_backend{};Store warning_store{};StoreRecovery warning_recovery{};
    assert(store_open(&warning_store,{&warning_backend,read_record,write_record},device,
                      catalog,75,&warning_recovery)==STORE_OK);
    ClockSample warning_now=sample(warning_recovery.boot_identity.epoch,1000);
    AppState warning_state{};
    assert(app_init(&warning_state,&warning_recovery.shared,&warning_now,
                    warning_recovery.boot_identity,75,catalog)==APP_OK);
    const unsigned warning_writes=warning_backend.commits;
    assert(store_mark_divergent(&warning_store,&warning_state)==STORE_OK);
    assert(warning_backend.commits==warning_writes+1 && warning_state.alarm_active &&
           warning_state.divergent && warning_store.record.conflict_count==0 &&
           warning_store.record.pending_count==0);
    assert(store_mark_divergent(&warning_store,&warning_state)==STORE_DUPLICATE &&
           warning_backend.commits==warning_writes+1);
    Store warning_restart{};StoreRecovery warning_after{};
    assert(store_open(&warning_restart,{&warning_backend,read_record,write_record},device,
                      catalog,75,&warning_after)==STORE_OK);
    assert(store_durable_divergent(&warning_restart) &&
           warning_restart.record.conflict_count==0);

    Backend empty{};Store empty_store{};StoreRecovery empty_recovery{};
    assert(store_open(&empty_store,{&empty,read_record,write_record},device,
                      catalog,75,&empty_recovery)==STORE_OK);
    assert(provision(empty_store,0x11,0x22)==STORE_OK);
    assert(provision(empty_store,0x33,0x44)==STORE_OK);
    assert(bound_to(empty_store,0x33) && !empty_store.record.has_history &&
           empty_store.record.pending_count==0 &&
           !empty_store.record.last_confirmation.valid &&
           empty_store.record.group_origin==STORE_GROUP_ORIGIN_FIRST_BOUND);
    char empty_reset_reply[256]{};
    assert(store_usb_handle_line(&empty_store,"KM1 RESET CONFIG\n",17,nullptr,
                                 empty_reset_reply,sizeof(empty_reset_reply))==STORE_OK);
    Store empty_reopened{}; StoreRecovery empty_after_reset{};
    assert(store_open(&empty_reopened,{&empty,read_record,write_record},device,
                      catalog,75,&empty_after_reset)==STORE_OK);
    const unsigned empty_writes=empty.commits;
    std::string empty_reply;
    assert(provision(empty_reopened,0x55,0x66,"Other",&empty_reply)==STORE_STALE &&
           empty_reply=="KM1 ERR CONFIG\n" && empty.commits==empty_writes);
    assert(provision(empty_reopened,0x33,0x77,"Same",&empty_reply)==STORE_OK);
    check_fresh_current_offline_first_provision();
    std::printf("v10 pending-and-conflict-max encoded blob: %zu / %u bytes\n",
                maximal.length,KATMED_STORE_BLOB_MAX);
    std::puts("group rebind durability matrix passed");
}
