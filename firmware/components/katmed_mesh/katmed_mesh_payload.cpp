#include "katmed_mesh_payload.h"
#include <cstring>

namespace {
struct Codec {
    uint8_t *p; const uint8_t *r; size_t n, at; bool ok;
    void u8(uint8_t v) { if (at<n) p[at++]=v; else ok=false; }
    void u32(uint32_t v) { for (int i=3;i>=0;--i) u8(uint8_t(v>>(i*8))); }
    void u64(uint64_t v) { for (int i=7;i>=0;--i) u8(uint8_t(v>>(i*8))); }
    void i64(int64_t v) { u64(uint64_t(v)); }
    void bytes(const uint8_t *v,size_t count) { for(size_t i=0;i<count;++i)u8(v[i]); }
    uint8_t b8() { if(at>=n){ok=false;return 0;}return r[at++]; }
    uint32_t b32() { uint32_t v=0;for(int i=0;i<4;++i)v=(v<<8)|b8();return v; }
    uint64_t b64() { uint64_t v=0;for(int i=0;i<8;++i)v=(v<<8)|b8();return v; }
    int64_t bi64() { uint64_t bits=b64();int64_t v;std::memcpy(&v,&bits,sizeof(v));return v; }
    void read(uint8_t *v,size_t count){for(size_t i=0;i<count;++i)v[i]=b8();}
    void boolean(bool v){u8(v?1:0);}
    bool bbool(){uint8_t v=b8();if(v>1)ok=false;return v==1;}
    void id(EventId v){u64(v.device);u64(v.epoch);u64(v.sequence);}
    EventId bid(){return EventId{b64(),b64(),b64()};}
    void round(AlarmRoundId v){id(v.origin);u64(v.ordinal);}
    AlarmRoundId bround(){AlarmRoundId v{};v.origin=bid();v.ordinal=b64();return v;}
    void rev(StateRevision v){u64(v.number);u64(v.coordinator);}
    StateRevision brev(){return StateRevision{b64(),b64()};}
    void clock(ClockSample v){u64(v.monotonic_ms);u64(v.domain);i64(v.utc_ms);boolean(v.utc_valid);u8(uint8_t(v.quality));}
    ClockSample bclock(){ClockSample v{};v.monotonic_ms=b64();v.domain=b64();v.utc_ms=bi64();v.utc_valid=bbool();v.quality=ClockQuality(b8());if(v.quality>CLOCK_SYNCHRONIZED)ok=false;return v;}
    void instant(UtcInstant v){boolean(v.valid);i64(v.ms);}
    UtcInstant binstant(){UtcInstant v{};v.valid=bbool();v.ms=bi64();return v;}
    void deadline(Deadline v){u64(v.regular_monotonic_ms);u64(v.effective_monotonic_ms);instant(v.regular_utc);instant(v.effective_utc);boolean(v.night_advanced);}
    Deadline bdeadline(){Deadline v{};v.regular_monotonic_ms=b64();v.effective_monotonic_ms=b64();v.regular_utc=binstant();v.effective_utc=binstant();v.night_advanced=bbool();return v;}
    void settings(SharedSettings v){u32(v.medication_seconds);u32(v.image_seconds);u8(uint8_t(v.rotation));rev(v.revision);}
    SharedSettings bsettings(){SharedSettings v{};v.medication_seconds=b32();v.image_seconds=b32();v.rotation=RotationMode(b8());v.revision=brev();if(v.medication_seconds==0||v.image_seconds==0||v.rotation>ROTATION_RANDOM)ok=false;return v;}
};
bool id_valid(EventId id){return id.device!=0&&id.epoch!=0&&id.sequence!=0;}
}

extern "C" bool katmed_mesh_encode_confirm(const StorePendingConfirm *pending,
                                              uint64_t age_ms_at_send,
                                              uint8_t *out,size_t capacity,uint16_t *length){
    if(!pending||!out||!length||capacity<66||!id_valid(pending->id)||
       !id_valid(pending->round.origin)||pending->round.ordinal==0)return false;
    Codec w{out,nullptr,capacity,0,true};
    w.id(pending->round.origin);w.u64(pending->round.ordinal);
    w.clock(pending->occurred_at);
    w.u64(age_ms_at_send);
    if(!w.ok)return false;
    *length=uint16_t(w.at);return true;
}
extern "C" bool katmed_mesh_decode_confirm(const uint8_t *bytes,size_t length,
                                              StorePendingConfirm *pending,
                                              uint64_t *age_ms_at_send){
    if(!bytes||!pending||!age_ms_at_send||length!=66)return false;
    Codec r{nullptr,bytes,length,0,true};
    StorePendingConfirm v{};
    v.round=r.bround();v.occurred_at=r.bclock();
    const uint64_t age=r.b64();
    if(!r.ok||r.at!=length||!id_valid(v.round.origin)||v.round.ordinal==0||
       v.occurred_at.domain==0)return false;
    *pending=v;*age_ms_at_send=age;return true;
}
extern "C" bool katmed_mesh_encode_snapshot(const StateSnapshot *snapshot,
                                               uint8_t *out,size_t capacity,uint16_t *length){
    if(!snapshot||!out||!length||snapshot->schema_version!=KATMED_SCHEMA_VERSION||
       snapshot->state.recent_event_count>KATMED_RECENT_EVENTS||
       snapshot->state.recent_event_next>=KATMED_RECENT_EVENTS)return false;
    Codec w{out,nullptr,capacity,0,true};
    const AppState &s=snapshot->state;
    w.u32(snapshot->schema_version);w.bytes(snapshot->catalog_id,32);
    w.u32(s.schema_version);w.settings(s.settings);w.rev(s.revision);w.round(s.round);
    w.boolean(s.alarm_active);w.u8(uint8_t(s.start_reason));w.clock(s.clock);
    w.clock(s.countdown_started);w.id(s.countdown_origin);
    w.boolean(s.has_time_sync);w.clock(s.last_time_sync);w.id(s.last_time_sync_id);
    w.deadline(s.deadline);w.boolean(s.has_last_confirmation);
    w.id(s.last_confirmation_id);w.round(s.last_confirmed_round);
    w.clock(s.last_confirmation);w.clock(s.last_settings_change);
    w.u32(s.image_id);w.u32(s.image_count);w.u64(s.image_remaining_ms);
    w.boolean(s.image_due_pending);w.boolean(s.confirmation_pending_sync);
    w.boolean(s.divergent);w.boolean(s.catalog_mismatch);w.bytes(s.catalog_id,32);
    w.u8(s.recent_event_count);w.u8(s.recent_event_next);
    for(const EventId &id:s.recent_events)w.id(id);
    w.clock(snapshot->captured_at);w.u64(snapshot->medication_regular_remaining_ms);
    w.u64(snapshot->medication_effective_remaining_ms);w.u64(snapshot->image_remaining_ms);
    if(!w.ok||w.at>KATMED_MESH_PAYLOAD_MAX)return false;
    *length=uint16_t(w.at);return true;
}
extern "C" bool katmed_mesh_decode_snapshot(const uint8_t *bytes,size_t length,
                                               StateSnapshot *snapshot){
    if(!bytes||!snapshot||length>KATMED_MESH_PAYLOAD_MAX)return false;
    Codec r{nullptr,bytes,length,0,true};StateSnapshot v{};AppState &s=v.state;
    v.schema_version=r.b32();r.read(v.catalog_id,32);
    s.schema_version=r.b32();s.settings=r.bsettings();s.revision=r.brev();s.round=r.bround();
    s.alarm_active=r.bbool();s.start_reason=StartReason(r.b8());s.clock=r.bclock();
    s.countdown_started=r.bclock();s.countdown_origin=r.bid();
    s.has_time_sync=r.bbool();s.last_time_sync=r.bclock();s.last_time_sync_id=r.bid();
    s.deadline=r.bdeadline();s.has_last_confirmation=r.bbool();
    s.last_confirmation_id=r.bid();s.last_confirmed_round=r.bround();
    s.last_confirmation=r.bclock();s.last_settings_change=r.bclock();
    s.image_id=r.b32();s.image_count=r.b32();s.image_remaining_ms=r.b64();
    s.image_due_pending=r.bbool();s.confirmation_pending_sync=r.bbool();
    s.divergent=r.bbool();s.catalog_mismatch=r.bbool();r.read(s.catalog_id,32);
    s.recent_event_count=r.b8();s.recent_event_next=r.b8();
    for(EventId &id:s.recent_events)id=r.bid();
    v.captured_at=r.bclock();v.medication_regular_remaining_ms=r.b64();
    v.medication_effective_remaining_ms=r.b64();v.image_remaining_ms=r.b64();
    if(!r.ok||r.at!=length||v.schema_version!=KATMED_SCHEMA_VERSION||
       s.schema_version!=KATMED_SCHEMA_VERSION||s.start_reason>START_SNAPSHOT||
       s.recent_event_count>KATMED_RECENT_EVENTS||s.recent_event_next>=KATMED_RECENT_EVENTS||
       !id_valid(s.round.origin)||s.round.ordinal==0||
       std::memcmp(v.catalog_id,s.catalog_id,32)!=0)return false;
    *snapshot=v;return true;
}
extern "C" bool katmed_mesh_encode_hello(const uint8_t echo[16],
    const uint8_t catalog[32],const char *version,uint8_t pending_count,uint8_t *out,
    size_t capacity,uint16_t *length){
    if(!echo||!catalog||!version||!out||!length||
       pending_count>KATMED_STORE_PENDING_MAX)return false;
    const size_t n=strnlen(version,33);
    if(n==0||n>31||capacity<54+n)return false;
    for(size_t i=0;i<n;++i)if((unsigned char)version[i]<0x21||
                                (unsigned char)version[i]>0x7e)return false;
    std::memcpy(out,echo,16);std::memcpy(out+16,catalog,32);
    out[48]=0;out[49]=0;out[50]=0;out[51]=KATMED_SCHEMA_VERSION;
    out[52]=uint8_t(n);std::memcpy(out+53,version,n);
    out[53+n]=pending_count;
    *length=uint16_t(54+n);return true;
}
extern "C" bool katmed_mesh_decode_hello(const uint8_t *bytes,size_t length,
    const uint8_t expected_catalog[32],const char *expected_version,
    uint8_t echo[16],bool *catalog_mismatch,uint8_t *pending_count){
    if(!bytes||!expected_catalog||!expected_version||!echo||!catalog_mismatch||
       !pending_count||length<55||length>85||bytes[52]==0||bytes[52]>=32||
       length!=size_t(54)+bytes[52]||bytes[53+bytes[52]]>KATMED_STORE_PENDING_MAX||
       bytes[48]!=0||bytes[49]!=0||bytes[50]!=0||
       bytes[51]!=KATMED_SCHEMA_VERSION)return false;
    const size_t n=strnlen(expected_version,33);
    if(n!=bytes[52]||n==0||n>31)return false;
    for(size_t i=0;i<n;++i){
        if(bytes[53+i]<0x21||bytes[53+i]>0x7e)return false;
        if(bytes[53+i]!=(uint8_t)expected_version[i])return false;
    }
    std::memcpy(echo,bytes,16);
    *catalog_mismatch=std::memcmp(bytes+16,expected_catalog,32)!=0;
    *pending_count=bytes[53+n];
    return true;
}

extern "C" bool katmed_mesh_encode_name_map(const StoreMeshNameEntry *entries,
                                              uint8_t count, uint8_t *out,
                                              size_t capacity, uint16_t *length) {
    if (!entries || !out || !length || count == 0 ||
        count > KATMED_MESH_NAME_MAX_ENTRIES || capacity < 1u + 10u * count)
        return false;
    out[0]=count;
    for (unsigned i=0;i<count;++i) {
        if (!entries[i].device || entries[i].index == 0) return false;
        for (unsigned j=0;j<i;++j)
            if (entries[j].device == entries[i].device ||
                entries[j].index == entries[i].index) return false;
        for (unsigned j=0;j<8;++j)
            out[1+10*i+j]=uint8_t(entries[i].device>>(56-8*j));
        out[1+10*i+8]=uint8_t(entries[i].index>>8);
        out[1+10*i+9]=uint8_t(entries[i].index);
    }
    *length=uint16_t(1u+10u*count);
    return true;
}

extern "C" bool katmed_mesh_decode_name_map(const uint8_t *bytes, size_t length,
                                              StoreMeshNameEntry *entries,
                                              uint8_t *count) {
    if (!bytes || !entries || !count || length < 11 || bytes[0] == 0 ||
        bytes[0] > KATMED_MESH_NAME_MAX_ENTRIES ||
        length != 1u + 10u * bytes[0]) return false;
    const uint8_t n=bytes[0];
    StoreMeshNameEntry decoded[KATMED_MESH_NAME_MAX_ENTRIES]{};
    for (unsigned i=0;i<n;++i) {
        uint64_t device=0;
        for (unsigned j=0;j<8;++j) device=(device<<8)|bytes[1+10*i+j];
        const uint16_t index=uint16_t(bytes[1+10*i+8]<<8)|bytes[1+10*i+9];
        if (!device || index == 0) return false;
        for (unsigned j=0;j<i;++j)
            if (decoded[j].device == device || decoded[j].index == index) return false;
        decoded[i]={device,index};
    }
    std::memcpy(entries,decoded,sizeof(decoded));
    *count=n;
    return true;
}
