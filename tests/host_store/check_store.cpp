#include "katmed_store.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <zlib.h>

namespace {
constexpr uint64_t kDevice=UINT64_C(0xAC276E46BC2C);
uint8_t catalog[KATMED_CATALOG_BYTES]{};
enum Fault { NONE, BEFORE, DURING, AFTER };
struct Fake {
    uint8_t bytes[KATMED_STORE_BLOB_MAX]{};
    size_t length=0;
    unsigned writes=0;
    Fault fault=NONE;
};
StoreResult fake_read(void *context,uint8_t *out,size_t *length) {
    Fake &f=*static_cast<Fake *>(context);
    if (!f.length) return STORE_NOT_FOUND;
    if (f.length>*length) return STORE_CORRUPT;
    std::memcpy(out,f.bytes,f.length); *length=f.length; return STORE_OK;
}
StoreResult fake_write(void *context,const uint8_t *data,size_t length) {
    Fake &f=*static_cast<Fake *>(context); ++f.writes;
    if (f.fault==BEFORE) return STORE_IO;
    if (f.fault==DURING) return STORE_COMMIT_UNKNOWN;
    assert(length<=sizeof(f.bytes));
    std::memcpy(f.bytes,data,length); f.length=length;
    if (f.fault==AFTER) return STORE_COMMIT_UNKNOWN;
    return STORE_OK;
}
StoreBackend backend(Fake &f) { return {&f,fake_read,fake_write}; }
void repair_crc(Fake &f) {
    f.bytes[6]=static_cast<uint8_t>(f.length);
    f.bytes[7]=static_cast<uint8_t>(f.length>>8);
    uint32_t crc=static_cast<uint32_t>(crc32(0,f.bytes+12,f.length-12));
    for (unsigned i=0;i<4;++i) f.bytes[8+i]=static_cast<uint8_t>(crc>>(8*i));
}
void remove_v9_names(Fake &f) {
    constexpr size_t mesh_name_suffix=1+17+2+1+KATMED_MESH_NAME_MAX_ENTRIES*10+4;
    assert(f.bytes[4]==9 && f.length>mesh_name_suffix);
    f.length-=mesh_name_suffix; // v9 widens the bounded registry and exhaustion cursor.
    f.bytes[4]=7;
    repair_crc(f);
}
void remove_v10_wifi_profiles(Fake &f);
void remove_v12_mode(Fake &f) {
    if(f.bytes[4]==12) {f.length-=2;f.bytes[4]=11;repair_crc(f);}
}
void remove_v11_language(Fake &f) {
    remove_v12_mode(f);
    assert(f.bytes[4]==11 && f.length>12);
    --f.length; f.bytes[4]=10; repair_crc(f);
}
void downgrade_names_to_v8(Fake &f) {
    constexpr size_t v9_suffix=1+17+2+1+KATMED_MESH_NAME_MAX_ENTRIES*10+4;
    constexpr size_t v8_suffix=1+17+2+1+9*10+2;
    remove_v12_mode(f);
    if (f.bytes[4]==11) remove_v11_language(f);
    if (f.bytes[4]==10) remove_v10_wifi_profiles(f);
    assert(f.bytes[4]==9 && f.length>v9_suffix);
    const size_t suffix=f.length-v9_suffix;
    const size_t old_cursor=suffix+1+17+2+1+9*10;
    const size_t current_cursor=f.length-4;
    std::memmove(f.bytes+old_cursor,f.bytes+current_cursor,2);
    f.length=suffix+v8_suffix;
    f.bytes[4]=8;
    repair_crc(f);
}
void remove_v10_wifi_profiles(Fake &f) {
    remove_v12_mode(f);
    if (f.bytes[4]==11) remove_v11_language(f);
    assert(f.bytes[4]==10 && f.length>=17);
    size_t suffix=0; unsigned matches=0;
    for (unsigned count=0;count<=KATMED_STORE_WIFI_PROFILE_MAX;++count) {
        const size_t candidate=5u+static_cast<size_t>(count)*103u;
        if (f.length>candidate && f.bytes[f.length-candidate]==count) {
            suffix=candidate; ++matches;
        }
    }
    assert(matches==1 && f.length>suffix);
    f.length-=suffix; f.bytes[4]=9; repair_crc(f);
}
void remove_v7_manual(Fake &f) {
    remove_v12_mode(f);
    if (f.bytes[4]==11) remove_v11_language(f);
    if (f.bytes[4]==10) remove_v10_wifi_profiles(f);
    if (f.bytes[4]==9) remove_v9_names(f);
    assert(f.bytes[4]==7 && f.length>77);
    f.length-=77; // v7 appends one bounded manual-deadline projection.
    f.bytes[4]=6;
    repair_crc(f);
}
void remove_v6_empty_conflicts(Fake &f) {
    remove_v7_manual(f);
    assert(f.bytes[4]==6 && f.length>310);
    --f.length; // v6 appends one origin byte; older blobs cannot prove it.
    constexpr size_t suffix=210+16+83; // Provision, v4 provenance, v3 confirmation.
    const size_t at=f.length-suffix-2;
    assert(f.bytes[at]==0 && f.bytes[at+1]==0);
    std::memmove(f.bytes+at,f.bytes+at+2,f.length-at-2);
    f.length-=2;
    f.bytes[4]=4;
    repair_crc(f);
}
Store *fresh_store() { static Store stores[2]; static unsigned index=0; return &stores[(index++)%2]; }
StoreRecovery open(Fake &f,Store *s) {
    StoreRecovery r{};
    assert(store_open(s,backend(f),kDevice,catalog,75,&r)==STORE_OK);
    return r;
}
ClockSample clock(uint64_t epoch,uint64_t ms) { return {ms,epoch,0,false,CLOCK_UNAVAILABLE}; }
AppState boot_state(const StoreRecovery &r,uint64_t ms=1000) {
    AppState state{};
    const ClockSample now=clock(r.boot_identity.epoch,ms);
    assert(app_init(&state,&r.shared,&now,r.boot_identity,75,catalog)==APP_OK);
    assert(state.alarm_active && state.start_reason==START_BOOT);
    return state;
}
AppState due_state(const StoreRecovery &r,uint64_t ms=1000) {
    AppState state=boot_state(r,ms);
    const ClockSample started=clock(r.boot_identity.epoch,ms);
    AppEffects effects{};
    assert(app_dismiss_boot_alarm(&state,&started,&effects)==APP_OK);
    assert(!effects.persistence_required&&!effects.broadcast_required);
    AppEvent settings{};
    settings.kind=EVENT_SET_MEDICATION_INTERVAL;
    settings.id={99,r.boot_identity.epoch,1};settings.sender=settings.id.device;
    settings.round=state.round;settings.parent_revision=state.revision;
    settings.occurred_at=started;settings.coordinator_accepted=true;
    settings.value.interval_seconds=1;
    assert(app_apply_event(&state,&settings,&started,&effects)==APP_OK);
    const ClockSample due=clock(r.boot_identity.epoch,ms+1000);
    assert(app_tick(&state,&due,&effects)==APP_OK);
    assert(state.start_reason==START_SETTINGS&&state.alarm_active&&
           !state.has_last_confirmation);
    return state;
}
AppEvent confirmation(Store *s,const AppState &state,const ClockSample &now) {
    AppEvent e{};
    assert(store_next_event_id(s,&e.id)==STORE_OK);
    e.kind=EVENT_CONFIRM; e.sender=e.id.device; e.round=state.round;
    e.parent_revision=state.revision; e.occurred_at=now;
    e.coordinator_accepted=false;
    return e;
}
void test_missing_and_local() {
    Fake f{}; Store *s=fresh_store(); const StoreRecovery r=open(f,s);
    assert(f.writes==1 && r.local.orientation==ORIENTATION_0 && r.local.clock_style==DIGITAL_LARGE);
    assert(r.boot_identity.epoch==1 && r.boot_identity.sequence==1 && !r.has_last_confirmation);
    LocalSettings active=r.local, requested=active;
    requested.orientation=ORIENTATION_270; requested.clock_style=ANALOG;
    assert(store_set_local(s,&active,&requested)==STORE_OK);
    assert(active.orientation==ORIENTATION_270 && active.clock_style==ANALOG && f.writes==2);
    assert(store_set_local(s,&active,&requested)==STORE_DUPLICATE && f.writes==2);
    StoreRecovery reboot=open(f,fresh_store());
    assert(reboot.boot_identity.epoch==2 && reboot.local.orientation==ORIENTATION_270 &&
           reboot.local.clock_style==ANALOG && !reboot.has_last_confirmation);
    AppState boot=boot_state(reboot);
    assert(boot.alarm_active);
}
void test_v8_mesh_name_migration_past_999() {
    Fake fixture;
    Store initial{}; StoreRecovery recovery{};
    assert(store_open(&initial,backend(fixture),kDevice,catalog,75,&recovery)==STORE_OK);
    const StoreMeshNameEntry legacy_last[]={{kDevice,999}};
    assert(store_mesh_name_merge(&initial,nullptr,legacy_last,1)==STORE_OK);
    assert(store_mesh_name_next_index(&initial)==1000);
    downgrade_names_to_v8(fixture);
    assert(fixture.bytes[4]==8);
    Store migrated{};
    assert(store_open(&migrated,backend(fixture),kDevice,catalog,75,&recovery)==STORE_OK);
    assert(std::strcmp(store_mesh_name(&migrated),"KatmedESP-999")==0);
    assert(store_mesh_name_next_index(&migrated)==1000);
    assert(store_mesh_name_assign(&migrated,UINT64_C(0xAC276E46BC2D),1000)==STORE_OK);
    assert(fixture.bytes[4]==12 && store_mesh_name_next_index(&migrated)==1001);
}
void test_corrupt_legacy_future() {
    Fake original{}; Store *s=fresh_store(); StoreRecovery r=open(original,s);
    AppState state=due_state(r);
    ClockSample at=clock(r.boot_identity.epoch,2000);
    AppEvent e=confirmation(s,state,at);
    AppEffects fx{}; AppResult core=APP_INVALID;
    assert(store_apply_event(s,&state,&e,&at,true,&fx,&core)==STORE_OK);
    assert(!state.alarm_active && fx.persistence_required && original.writes==3);
    {
        Fake f=original; f.bytes[20]^=1; const unsigned before=f.writes;
        StoreRecovery x{}; Store *t=fresh_store();
        assert(store_open(t,backend(f),kDevice,catalog,75,&x)==STORE_CORRUPT);
        assert(f.writes==before && !t->open);
    }
    {
        Fake f=original; --f.length; const unsigned before=f.writes;
        StoreRecovery x{};
        assert(store_open(fresh_store(),backend(f),kDevice,catalog,75,&x)==STORE_CORRUPT);
        assert(f.writes==before);
    }
    {
        Fake f=original; f.bytes[4]=99; const unsigned before=f.writes;
        StoreRecovery x{};
        assert(store_open(fresh_store(),backend(f),kDevice,catalog,75,&x)==STORE_UNSUPPORTED);
        assert(f.writes==before);
    }
    {
        Fake f=original;
        remove_v7_manual(f);
        f.bytes[f.length-78]=3; // v6 origin before the v7 manual projection.
        repair_crc(f);
        StoreRecovery x{}; const unsigned before=f.writes;
        Store *t=fresh_store();
        assert(store_open(t,backend(f),kDevice,catalog,75,&x)==STORE_CORRUPT);
        assert(f.writes==before && !t->open &&
               t->staged.group_origin==STORE_GROUP_ORIGIN_UNKNOWN);
    }
    {
        Fake f=original;
        remove_v7_manual(f);
        --f.length; f.bytes[4]=5; f.bytes[5]=0; repair_crc(f);
        Store *t=fresh_store(); const StoreRecovery x=open(f,t);
        assert(x.pending_count==1 &&
               t->record.group_origin==STORE_GROUP_ORIGIN_UNKNOWN);
        Store *again=fresh_store(); open(f,again);
        assert(again->record.group_origin==STORE_GROUP_ORIGIN_UNKNOWN);
    }
    {
        Fake f=original; f.bytes[97]=255; repair_crc(f);
        StoreRecovery x{}; const unsigned before=f.writes;
        Store *t=fresh_store();
        assert(store_open(t,backend(f),kDevice,catalog,75,&x)==STORE_CORRUPT);
        assert(f.writes==before && !t->open && t->record.device==0);
        // CRC passes, so the decoder reached a partially populated staged
        // record before semantic validation rejected the orientation.
        assert(t->staged.device==0 && t->staged.pending_count==0 &&
               !t->staged.has_history && !t->staged.provision.configured);
        for (uint8_t byte : t->staged.provision.secret) assert(byte==0);
    }
    {
        Fake f=original; std::memset(f.bytes+72,0,4); repair_crc(f);
        StoreRecovery x{}; const unsigned before=f.writes;
        assert(store_open(fresh_store(),backend(f),kDevice,catalog,75,&x)==STORE_CORRUPT);
        assert(f.writes==before);
    }
    {
        Fake f=original; std::memset(f.bytes+108,0,4); repair_crc(f);
        StoreRecovery x{}; const unsigned before=f.writes;
        assert(store_open(fresh_store(),backend(f),kDevice,catalog,75,&x)==STORE_CORRUPT);
        assert(f.writes==before);
    }
    {
        Fake f=original;
        remove_v6_empty_conflicts(f);
        // Version 1 is the same bounded field codec before clock_style existed.
        // Remove exactly that field, keeping medication/image history untouched.
        constexpr size_t clock_style_offset=98;
        constexpr size_t confirmation_suffix=83+16; // v4 bound_group plus v3 confirmation.
        f.length-=confirmation_suffix;
        std::memmove(f.bytes+clock_style_offset,f.bytes+clock_style_offset+1,
                     f.length-clock_style_offset-1);
        --f.length; f.bytes[4]=1; f.bytes[5]=0; repair_crc(f);
        StoreRecovery migrated=open(f,fresh_store());
        assert(migrated.local.clock_style==DIGITAL_LARGE);
        assert(migrated.has_last_confirmation && migrated.pending_count==1);
        assert(migrated.last_confirmation_id.sequence==e.id.sequence);
    }
    {
        Fake f=original; f.bytes[98]=255; repair_crc(f);
        StoreRecovery migrated=open(f,fresh_store());
        assert(migrated.local.clock_style==DIGITAL_LARGE &&
               migrated.has_last_confirmation && migrated.pending_count==1);
    }
    {
        Fake f=original;
        remove_v6_empty_conflicts(f);
        f.length-=83+16; f.bytes[4]=2; f.bytes[5]=0; repair_crc(f);
        StoreRecovery migrated=open(f,fresh_store());
        assert(migrated.has_last_confirmation && migrated.pending_count==1 &&
               migrated.last_confirmation_id.sequence==e.id.sequence);
    }
    {
        Fake f=original; StoreRecovery x{};
        uint8_t other_catalog[32]{}; other_catalog[0]=0xee;
        Store *t=fresh_store();
        assert(store_open(t,backend(f),kDevice,other_catalog,0,&x)==STORE_OK);
        assert(x.catalog_mismatch && x.has_last_confirmation && x.pending_count==1);
        AppState neutral{}; ClockSample now=clock(x.boot_identity.epoch,1000);
        assert(app_init(&neutral,&x.shared,&now,x.boot_identity,0,other_catalog)==APP_OK);
        assert(neutral.alarm_active && neutral.image_count==0 && neutral.catalog_mismatch);
    }
}
void test_commit_matrix() {
    for (Fault fault : {BEFORE,DURING,AFTER}) {
        Fake f{}; Store *s=fresh_store(); StoreRecovery r=open(f,s);
        AppState state=due_state(r); ClockSample at=clock(r.boot_identity.epoch,2000);
        AppEvent e=confirmation(s,state,at);
        const AppState old=state;
        f.fault=fault;
        AppEffects fx{}; AppResult core=APP_INVALID;
        assert(store_apply_event(s,&state,&e,&at,true,&fx,&core)!=STORE_OK);
        assert(state.alarm_active==old.alarm_active && state.round.ordinal==old.round.ordinal &&
               !fx.state_changed && s->uncertain);
        f.fault=NONE;
        StoreRecovery reboot=open(f,fresh_store());
        assert(reboot.pending_count==(fault==AFTER ? 1 : 0));
        assert(reboot.has_last_confirmation==(fault==AFTER));
        AppState boot=boot_state(reboot);
        assert(boot.alarm_active); // Neither old monotonic deadline is restarted.
    }
    {
        Fake f{}; Store *s=fresh_store(); StoreRecovery r=open(f,s);
        AppState state=due_state(r); ClockSample at=clock(r.boot_identity.epoch,2000);
        AppEvent e=confirmation(s,state,at); AppEffects fx{}; AppResult core{};
        assert(store_apply_event(s,&state,&e,&at,true,&fx,&core)==STORE_OK);
        assert(!state.alarm_active && state.confirmation_pending_sync && s->record.pending_count==1);
        assert(store_apply_event(s,&state,&e,&at,true,&fx,&core)==STORE_DUPLICATE);
        assert(s->record.pending_count==1);
        f.fault=AFTER;
        assert(store_ack_pending(s,&state,e.id,true)==STORE_COMMIT_UNKNOWN);
        assert(state.confirmation_pending_sync);
        f.fault=NONE;
        StoreRecovery reboot=open(f,fresh_store());
        assert(reboot.pending_count==0 && reboot.has_last_confirmation);
    }
    for (Fault fault : {BEFORE,DURING,AFTER}) {
        Fake f{}; Store *s=fresh_store(); StoreRecovery r=open(f,s);
        LocalSettings active=r.local, requested=active;
        requested.orientation=ORIENTATION_90;
        f.fault=fault;
        assert(store_set_local(s,&active,&requested)!=STORE_OK);
        assert(active.orientation==ORIENTATION_0 && s->uncertain);
        f.fault=NONE;
        StoreRecovery reboot=open(f,fresh_store());
        assert(reboot.local.orientation==(fault==AFTER ? ORIENTATION_90 : ORIENTATION_0));
    }
    for (Fault fault : {BEFORE,DURING,AFTER}) {
        Fake f{}; Store *first=fresh_store(); StoreRecovery r=open(f,first);
        AppState active=due_state(r); const ClockSample at=clock(r.boot_identity.epoch,2000);
        const AppEvent e=confirmation(first,active,at);
        AppEffects fx{}; AppResult core{};
        assert(store_apply_event(first,&active,&e,&at,true,&fx,&core)==STORE_OK);
        Store *durable=fresh_store(); open(f,durable);
        assert(durable->record.group_origin==STORE_GROUP_ORIGIN_FRESH_UNBOUND &&
               durable->record.pending_count==1);
        const std::string cmd="KM1 PROVISION group="+std::string(32,'1')+
            " secret="+std::string(64,'2')+
            " ssid=486f6d65 password=3132333435363738 time=706f6f6c2e6e74702e6f7267\n";
        char reply[256]{};
        f.fault=fault;
        assert(store_usb_handle_line(durable,cmd.c_str(),cmd.size(),nullptr,
                                     reply,sizeof(reply))==
               (fault==BEFORE ? STORE_IO : STORE_COMMIT_UNKNOWN));
        assert(std::strcmp(reply,"KM1 ERR COMMIT\n")==0 && durable->uncertain &&
               durable->record.group_origin==STORE_GROUP_ORIGIN_FRESH_UNBOUND &&
               durable->record.pending_count==1 && !durable->record.provision.configured);
        f.fault=NONE;
        Store *reopened=fresh_store(); const StoreRecovery recovered=open(f,reopened);
        assert(recovered.pending_count==1 && reopened->record.pending[0].id.device==e.id.device &&
               reopened->record.pending[0].id.epoch==e.id.epoch &&
               reopened->record.pending[0].id.sequence==e.id.sequence &&
               reopened->record.group_origin==(fault==AFTER ?
                   STORE_GROUP_ORIGIN_FIRST_BOUND : STORE_GROUP_ORIGIN_FRESH_UNBOUND));
        assert(recovered.configured==(fault==AFTER));
    }
}
void test_history_survives_later_boot_state_commit() {
    Fake f{}; Store *first=fresh_store(); StoreRecovery recovery=open(f,first);
    AppState first_state=due_state(recovery);
    const ClockSample confirmed_at=clock(recovery.boot_identity.epoch,2000);
    const AppEvent confirm=confirmation(first,first_state,confirmed_at);
    AppEffects effects{}; AppResult core{};
    assert(store_apply_event(first,&first_state,&confirm,&confirmed_at,true,&effects,&core)==STORE_OK);
    assert(first->record.pending_count==1);

    Store *second=fresh_store(); recovery=open(f,second);
    assert(recovery.has_last_confirmation && recovery.pending_count==1);
    assert(recovery.last_confirmation_id.sequence==confirm.id.sequence);
    AppState second_state=boot_state(recovery);
    const ClockSample changed_at=clock(recovery.boot_identity.epoch,2000);
    AppEvent change{};
    assert(store_next_event_id(second,&change.id)==STORE_OK);
    change.kind=EVENT_SET_IMAGE_INTERVAL;
    change.sender=change.id.device;
    change.round=second_state.round;
    change.parent_revision=second_state.revision;
    change.occurred_at=changed_at;
    change.coordinator_accepted=true;
    change.value.interval_seconds=3600;
    assert(store_apply_event(second,&second_state,&change,&changed_at,false,&effects,&core)==STORE_OK);
    assert(second_state.alarm_active && !second_state.has_last_confirmation);
    assert(second->record.pending_count==1 && second->record.last_confirmation.valid);

    // A v2 record written after the same reboot transition has no separate
    // history field. Its still-pending original event permits a bounded salvage.
    Fake legacy=f;
    remove_v6_empty_conflicts(legacy);
    legacy.length-=83+16;
    legacy.bytes[4]=2; legacy.bytes[5]=0; repair_crc(legacy);
    Store *migrated=fresh_store();
    StoreRecovery migrated_recovery=open(legacy,migrated);
    assert(migrated_recovery.has_last_confirmation && migrated_recovery.pending_count==1);
    assert(migrated_recovery.last_confirmation_id.sequence==confirm.id.sequence);

    Store *third=fresh_store(); recovery=open(f,third);
    assert(recovery.has_last_confirmation && recovery.pending_count==1);
    assert(recovery.last_confirmation_id.device==confirm.id.device &&
           recovery.last_confirmation_id.epoch==confirm.id.epoch &&
           recovery.last_confirmation_id.sequence==confirm.id.sequence);
    assert(recovery.shared.image_seconds==3600);
    assert(recovery.last_confirmation.domain!=recovery.boot_identity.epoch);
    assert(boot_state(recovery).alarm_active);
}
void test_manual_intent_migration_and_clear() {
    Fake f{}; Store *first=fresh_store(); StoreRecovery recovery=open(f,first);
    AppState state=due_state(recovery);
    const ClockSample confirmation_at=clock(recovery.boot_identity.epoch,2000);
    AppEvent confirm=confirmation(first,state,confirmation_at);
    confirm.coordinator_accepted=true;
    AppEffects effects{}; AppResult core{};
    assert(store_apply_event(first,&state,&confirm,&confirmation_at,false,
                             &effects,&core)==STORE_OK);
    ClockSample now{2001,recovery.boot_identity.epoch,INT64_C(1792400400000),
                    true,CLOCK_HOLDOVER};
    ManualTimePreview preview{};
    assert(app_preview_local_time(&now,14,30,&preview)==APP_OK);
    AppEvent next{};
    assert(store_next_event_id(first,&next.id)==STORE_OK);
    next.kind=EVENT_SET_NEXT_LOCAL_TIME; next.sender=next.id.device;
    next.round=state.round; next.parent_revision=state.revision;
    next.occurred_at=now; next.coordinator_accepted=true;
    next.value.local_time={14,30,preview.regular_utc,preview.effective_utc};
    assert(store_apply_event(first,&state,&next,&now,false,&effects,&core)==STORE_OK);
    assert(first->record.manual_intent.pending &&
           first->record.manual_intent.source.sequence==next.id.sequence &&
           first->record.manual_intent.regular_utc.ms==preview.regular_utc.ms);

    Store *reopened=fresh_store(); recovery=open(f,reopened);
    assert(recovery.manual_intent.pending && recovery.manual_intent.hour==14 &&
           recovery.manual_intent.minute==30 &&
           recovery.manual_intent.effective_utc.ms==preview.effective_utc.ms);
    Fake v6=f; remove_v7_manual(v6);
    Store *legacy=fresh_store(); StoreRecovery migrated=open(v6,legacy);
    assert(migrated.manual_intent.pending && migrated.manual_intent.hour==14 &&
           migrated.manual_intent.regular_utc.ms==preview.regular_utc.ms);

    for (Fault fault : {BEFORE,AFTER}) {
        Fake candidate=f; Store *s=fresh_store(); open(candidate,s);
        candidate.fault=fault;
        assert(store_clear_manual_intent(s)==
               (fault==BEFORE ? STORE_IO : STORE_COMMIT_UNKNOWN));
        assert(s->uncertain && s->record.manual_intent.pending);
        candidate.fault=NONE;
        Store *after=fresh_store(); const StoreRecovery result=open(candidate,after);
        assert(result.manual_intent.pending==(fault==BEFORE));
    }
    Store *clear=fresh_store(); open(f,clear);
    assert(store_clear_manual_intent(clear)==STORE_OK);
    assert(!clear->record.manual_intent.pending);
    assert(store_clear_manual_intent(clear)==STORE_DUPLICATE);
    Store *after=fresh_store(); recovery=open(f,after);
    assert(!recovery.manual_intent.pending);
}
void test_boundaries_and_no_tick_writes() {
    Fake f{}; Store *s=fresh_store(); StoreRecovery r=open(f,s);
    AppState state=boot_state(r); ClockSample now=clock(r.boot_identity.epoch,2000);
    const unsigned writes=f.writes;
    for (unsigned i=0;i<100;++i) {
        AppEffects fx{};
        assert(app_tick(&state,&now,&fx)==APP_OK);
        assert(!fx.persistence_required);
        now.monotonic_ms+=25;
    }
    // Clock overlay, blink and transient navigation have no Store call.
    assert(f.writes==writes);
    state=due_state(r,now.monotonic_ms-1000);
    s->record.next_sequence=UINT64_MAX;
    EventId id{};
    assert(store_next_event_id(s,&id)==STORE_FULL && f.writes==writes);
    s->record.next_sequence=2; // Only the boundary is injected; queue entries use real commits.
    AppEvent first{};
    for (unsigned i=0;i<KATMED_STORE_PENDING_MAX;++i) {
        if (i>0) {
            now.monotonic_ms=state.deadline.effective_monotonic_ms+1;
            AppEffects tick_fx{};
            assert(app_tick(&state,&now,&tick_fx)==APP_OK && state.alarm_active);
        }
        AppEvent e=confirmation(s,state,now);
        if (i==0) first=e;
        AppEffects fx{}; AppResult core{};
        assert(store_apply_event(s,&state,&e,&now,true,&fx,&core)==STORE_OK);
        assert(!state.alarm_active && s->record.pending_count==i+1);
    }
    AppEffects fx{}; AppResult core{};
    assert(store_apply_event(s,&state,&first,&now,true,&fx,&core)==STORE_STALE);
    AppEvent latest{};
    latest.id=state.last_confirmation_id;
    latest.kind=EVENT_CONFIRM; latest.sender=latest.id.device;
    latest.round=state.last_confirmed_round; latest.parent_revision=state.revision;
    latest.occurred_at=state.last_confirmation;
    assert(store_apply_event(s,&state,&latest,&now,true,&fx,&core)==STORE_DUPLICATE);
    now.monotonic_ms=state.deadline.effective_monotonic_ms+1;
    assert(app_tick(&state,&now,&fx)==APP_OK && state.alarm_active);
    AppEvent e=confirmation(s,state,now);
    const unsigned full_writes=f.writes;
    assert(store_apply_event(s,&state,&e,&now,true,&fx,&core)==STORE_FULL);
    assert(state.alarm_active && s->record.pending_count==KATMED_STORE_PENDING_MAX && f.writes==full_writes);
}
std::string hex(const uint8_t *data,size_t count) {
    static const char d[]="0123456789abcdef";
    std::string out;
    for (size_t i=0;i<count;++i) { out+=d[data[i]>>4]; out+=d[data[i]&15]; }
    return out;
}
void test_usb() {
    Fake f{}; Store *s=fresh_store(); open(f,s);
    StoreUsbStatusInfo info{"1.0.0","0123456789abcdef01234567",kDevice,CLOCK_UNAVAILABLE,{}};
    char reply[256]{};
    const char status[]="KM1 STATUS\n";
    assert(store_usb_handle_line(s,status,sizeof(status)-1,&info,reply,sizeof(reply))==STORE_OK);
    assert(std::strstr(reply,"configured=0 durable=1 pending=0 clock=0") &&
           std::strstr(reply,"catalog=") && std::strstr(reply,"build=0123456789abcdef01234567"));
    uint8_t group[16]{}, secret[32]{}, ssid[32]{}, pw[63]{}, time[63]{};
    std::memset(group,1,sizeof(group)); std::memset(secret,2,sizeof(secret));
    std::memset(ssid,'s',sizeof(ssid)); std::memset(pw,'p',sizeof(pw));
    std::memset(time,'a',sizeof(time));
    std::string cmd="KM1 PROVISION group="+hex(group,16)+" secret="+hex(secret,32)+
        " ssid="+hex(ssid,32)+" password="+hex(pw,63)+" time="+hex(time,63)+"\n";
    assert(cmd.size()<=512);
    const unsigned before=f.writes;
    assert(store_usb_handle_line(s,cmd.data(),cmd.size(),&info,reply,sizeof(reply))==STORE_OK);
    assert(std::strcmp(reply,"KM1 OK PROVISION\n")==0 && f.writes==before+1);
    Fake legacy_v7=f; remove_v7_manual(legacy_v7);
    Store migrated_v7_storage{}; Store *migrated_v7=&migrated_v7_storage;
    StoreRecovery migrated_recovery{};
    assert(store_open(migrated_v7,backend(legacy_v7),kDevice,catalog,75,
                      &migrated_recovery)==STORE_OK);
    StoreWifiProfile migrated_profiles[KATMED_STORE_WIFI_PROFILE_MAX]{};
    size_t migrated_count=0;
    assert(store_wifi_profiles_load(migrated_v7,migrated_profiles,
                                    KATMED_STORE_WIFI_PROFILE_MAX,
                                    &migrated_count)==STORE_OK);
    assert(migrated_count==1 && migrated_profiles[0].id==1 &&
           migrated_profiles[0].priority==1 &&
           migrated_profiles[0].ssid_length==sizeof(ssid) &&
           std::memcmp(migrated_profiles[0].ssid,ssid,sizeof(ssid))==0 &&
           migrated_profiles[0].password_length==sizeof(pw) &&
           std::memcmp(migrated_profiles[0].password,pw,sizeof(pw))==0);
    {
        Fake forged=f;
        constexpr size_t mesh_name_suffix=1+17+2+1+KATMED_MESH_NAME_MAX_ENTRIES*10+4;
        constexpr size_t wifi_suffix=5u+103u;
        forged.bytes[forged.length-1-wifi_suffix-mesh_name_suffix]=9; // Invalid mesh-name state.
        repair_crc(forged);
        StoreRecovery x{}; const unsigned before_forged=forged.writes;
        Store forged_store{};
        assert(store_open(&forged_store,backend(forged),kDevice,catalog,75,&x)==STORE_CORRUPT);
        assert(forged.writes==before_forged);
    }
    assert(store_usb_handle_line(s,status,sizeof(status)-1,&info,reply,sizeof(reply))==STORE_OK);
    assert(std::strstr(reply,"configured=1"));
    std::string bad=cmd;
    const size_t ssid_at=bad.find("ssid=")+5;
    assert(ssid_at>=5);
    bad.replace(ssid_at,2,"00");
    const unsigned unchanged=f.writes;
    assert(store_usb_handle_line(s,bad.data(),bad.size(),&info,reply,sizeof(reply))==STORE_INVALID);
    assert(std::strcmp(reply,"KM1 ERR CONFIG\n")==0 && f.writes==unchanged);
    bad=cmd;
    bad.replace(ssid_at,4,"c080"); // Overlong UTF-8 sequence.
    assert(store_usb_handle_line(s,bad.data(),bad.size(),&info,reply,sizeof(reply))==STORE_INVALID);
    assert(f.writes==unchanged);
    assert(store_usb_handle_line(s,"KM1 RESET CONFIG\n",17,&info,reply,sizeof(reply))==STORE_OK);
    assert(std::strcmp(reply,"KM1 OK RESET_CONFIG\n")==0 && !s->record.provision.configured);
    for (uint8_t b : s->record.provision.secret) assert(b==0);
    cmd.append(100,'x');
    assert(store_usb_handle_line(s,cmd.data(),cmd.size(),&info,reply,sizeof(reply))==STORE_INVALID);
    assert(std::strcmp(reply,"KM1 ERR LENGTH\n")==0);
    info.device=UINT64_MAX;
    assert(store_usb_handle_line(s,status,sizeof(status)-1,&info,reply,sizeof(reply))==STORE_INVALID);
    assert(std::strcmp(reply,"KM1 ERR STATUS\n")==0);
    Fake damaged=f; damaged.bytes[20]^=1; Store *t=fresh_store(); StoreRecovery x{};
    assert(store_open(t,backend(damaged),kDevice,catalog,75,&x)==STORE_CORRUPT);
    info.device=kDevice;
    assert(store_usb_handle_line(t,status,sizeof(status)-1,&info,reply,sizeof(reply))==STORE_OK);
    assert(std::strstr(reply,"durable=0") && std::strstr(reply,"error=CORRUPT"));
    assert(store_usb_handle_line(t,"KM1 RESET CONFIG\n",17,&info,reply,sizeof(reply))==STORE_UNAVAILABLE);
}
void test_language_reboot_and_factory_default() {
    Fake disk{};
    Store first{};
    StoreRecovery initial{};
    assert(store_open(&first,backend(disk),kDevice,catalog,75,&initial)==STORE_OK);
    assert(initial.language==KATMED_LANG_EN && first.record.language==KATMED_LANG_EN);
    for (uint8_t language=1; language<KATMED_LANGUAGE_COUNT; ++language) {
        assert(store_set_language(&first,language)==STORE_OK);
        assert(disk.length>12 && disk.bytes[4]==12 && first.record.language==language);
        assert(store_set_language(&first,language)==STORE_DUPLICATE);
        Store reboot{};
        StoreRecovery restored{};
        assert(store_open(&reboot,backend(disk),kDevice,catalog,75,&restored)==STORE_OK);
        assert(restored.language==language && reboot.record.language==language);
    }
    assert(store_set_language(&first,KATMED_LANGUAGE_COUNT)==STORE_INVALID);
    assert(store_set_language(&first,KATMED_LANG_EN)==STORE_OK);
    Store reboot{};
    StoreRecovery restored{};
    assert(store_open(&reboot,backend(disk),kDevice,catalog,75,&restored)==STORE_OK);
    assert(restored.language==KATMED_LANG_EN && reboot.record.language==KATMED_LANG_EN);
    Fake wiped{}; /* Synthetic empty backend after a full factory wipe. */
    Store reset{};
    StoreRecovery fresh{};
    assert(store_open(&reset,backend(wiped),kDevice,catalog,75,&fresh)==STORE_OK);
    assert(fresh.language==KATMED_LANG_EN && reset.record.language==KATMED_LANG_EN);
}
void test_explicit_network_modes() {
    Fake disk{}; Store store{}; StoreRecovery recovery{};
    assert(store_open(&store,backend(disk),kDevice,catalog,75,&recovery)==STORE_OK);
    const unsigned before=disk.writes;
    StoreProvision setup{}; setup.configured=true; setup.network_mode=STORE_NETWORK_OFFLINE;
    assert(store.record.provision.configured==false && disk.writes==before);
    store.record.shared.image_seconds=8u*3600u;
    assert(store_save_setup(&store,&setup)==STORE_OK);
    assert(store.record.shared.image_seconds==8u*3600u);
    Store again{};
    assert(store_open(&again,backend(disk),kDevice,catalog,75,&recovery)==STORE_OK);
    assert(again.record.provision.configured && again.record.provision.network_mode==STORE_NETWORK_OFFLINE);
    setup.network_mode=STORE_NETWORK_MESH_ONLY; setup.group[0]=1; setup.secret[0]=2;
    assert(store_save_setup(&again,&setup)==STORE_OK); // still genuinely first binding
    const unsigned saved=disk.writes;
    setup.group[0]=3;
    assert(store_save_setup(&again,&setup)==STORE_STALE);
    assert(disk.writes==saved && again.record.provision.group[0]==1);
    setup=again.record.provision; setup.network_mode=STORE_NETWORK_OFFLINE;
    assert(store_save_setup(&again,&setup)==STORE_OK);
    assert(again.record.bound_group[0]==1 && again.record.provision.network_mode==STORE_NETWORK_OFFLINE);
    setup.network_mode=STORE_NETWORK_ONLINE; // absent WLAN is not an online setup
    assert(store_save_setup(&again,&setup)==STORE_INVALID);
    assert(store_request_setup(&again)==STORE_OK && again.record.setup_requested);
    setup=again.record.provision;
    assert(store_save_setup(&again,&setup)==STORE_OK && !again.record.setup_requested);
    setup=again.record.provision; disk.fault=BEFORE;
    setup.network_mode=STORE_NETWORK_MESH_ONLY;
    assert(store_save_setup(&again,&setup)==STORE_IO);
    assert(again.record.provision.network_mode==STORE_NETWORK_OFFLINE && again.uncertain);
}

}
int main() {
    for (unsigned i=0;i<sizeof(catalog);++i) catalog[i]=static_cast<uint8_t>(i+1);
    test_explicit_network_modes();
    test_missing_and_local();
    test_language_reboot_and_factory_default();
    test_v8_mesh_name_migration_past_999();
    test_corrupt_legacy_future();
    test_commit_matrix();
    test_history_survives_later_boot_state_commit();
    test_manual_intent_migration_and_clear();
    test_boundaries_and_no_tick_writes();
    test_usb();
    std::puts("store host matrix passed");
}
