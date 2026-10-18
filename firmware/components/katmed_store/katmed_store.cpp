#include "katmed_store.h"

#include <cstdio>
#include <cstring>
#include <inttypes.h>

#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#else
#include <zlib.h>
#endif

namespace {
constexpr uint8_t kMagic[4] = {'K', 'M', 'S', 'T'};
constexpr uint16_t kCurrentVersion = 6;
constexpr size_t kHeaderSize = 12;

uint32_t checksum(const uint8_t *data, size_t length) {
#ifdef ESP_PLATFORM
    return esp_rom_crc32_le(0, data, length);
#else
    return static_cast<uint32_t>(crc32(0, data, length));
#endif
}

struct Writer {
    uint8_t *data;
    size_t capacity;
    size_t position = 0;
    bool good = true;
    void u8(uint8_t value) { if (position >= capacity) { good = false; return; } data[position++] = value; }
    void u16(uint16_t value) { u8(static_cast<uint8_t>(value)); u8(static_cast<uint8_t>(value >> 8)); }
    void u32(uint32_t value) { for (unsigned i = 0; i < 4; ++i) u8(static_cast<uint8_t>(value >> (8 * i))); }
    void u64(uint64_t value) { for (unsigned i = 0; i < 8; ++i) u8(static_cast<uint8_t>(value >> (8 * i))); }
    void i64(int64_t value) { u64(static_cast<uint64_t>(value)); }
    void boolean(bool value) { u8(value ? 1 : 0); }
    void bytes(const uint8_t *value, size_t length) { for (size_t i = 0; i < length; ++i) u8(value[i]); }
};

struct Reader {
    const uint8_t *data;
    size_t length;
    size_t position = 0;
    bool good = true;
    uint8_t u8() { if (position >= length) { good = false; return 0; } return data[position++]; }
    uint16_t u16() { uint16_t v = 0; for (unsigned i = 0; i < 2; ++i) v |= uint16_t(u8()) << (8 * i); return v; }
    uint32_t u32() { uint32_t v = 0; for (unsigned i = 0; i < 4; ++i) v |= uint32_t(u8()) << (8 * i); return v; }
    uint64_t u64() { uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v |= uint64_t(u8()) << (8 * i); return v; }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    bool boolean() { const uint8_t v = u8(); if (v > 1) good = false; return v == 1; }
    void bytes(uint8_t *value, size_t count) { for (size_t i = 0; i < count; ++i) value[i] = u8(); }
};

void put_id(Writer &w, EventId v) { w.u64(v.device); w.u64(v.epoch); w.u64(v.sequence); }
EventId get_id(Reader &r) { EventId v{}; v.device=r.u64(); v.epoch=r.u64(); v.sequence=r.u64(); return v; }
void put_round(Writer &w, AlarmRoundId v) { put_id(w,v.origin); w.u64(v.ordinal); }
AlarmRoundId get_round(Reader &r) { AlarmRoundId v{}; v.origin=get_id(r); v.ordinal=r.u64(); return v; }
void put_revision(Writer &w, StateRevision v) { w.u64(v.number); w.u64(v.coordinator); }
StateRevision get_revision(Reader &r) { StateRevision v{}; v.number=r.u64(); v.coordinator=r.u64(); return v; }
void put_clock(Writer &w, ClockSample v) {
    w.u64(v.monotonic_ms); w.u64(v.domain); w.i64(v.utc_ms); w.boolean(v.utc_valid); w.u8(static_cast<uint8_t>(v.quality));
}
ClockSample get_clock(Reader &r) {
    ClockSample v{}; v.monotonic_ms=r.u64(); v.domain=r.u64(); v.utc_ms=r.i64(); v.utc_valid=r.boolean();
    v.quality=static_cast<ClockQuality>(r.u8()); return v;
}
void put_instant(Writer &w, UtcInstant v) { w.boolean(v.valid); w.i64(v.ms); }
UtcInstant get_instant(Reader &r) { UtcInstant v{}; v.valid=r.boolean(); v.ms=r.i64(); return v; }
void put_settings(Writer &w, SharedSettings v) {
    w.u32(v.medication_seconds); w.u32(v.image_seconds); w.u8(static_cast<uint8_t>(v.rotation)); put_revision(w,v.revision);
}
SharedSettings get_settings(Reader &r) {
    SharedSettings v{}; v.medication_seconds=r.u32(); v.image_seconds=r.u32();
    v.rotation=static_cast<RotationMode>(r.u8()); v.revision=get_revision(r); return v;
}
void put_state(Writer &w, const AppState &s) {
    w.u32(s.schema_version); put_settings(w,s.settings); put_revision(w,s.revision);
    put_round(w,s.round); w.boolean(s.alarm_active); w.u8(static_cast<uint8_t>(s.start_reason));
    put_clock(w,s.clock); put_clock(w,s.countdown_started); put_id(w,s.countdown_origin);
    w.boolean(s.has_time_sync); put_clock(w,s.last_time_sync); put_id(w,s.last_time_sync_id);
    w.u64(s.deadline.regular_monotonic_ms); w.u64(s.deadline.effective_monotonic_ms);
    put_instant(w,s.deadline.regular_utc); put_instant(w,s.deadline.effective_utc); w.boolean(s.deadline.night_advanced);
    w.boolean(s.has_last_confirmation); put_id(w,s.last_confirmation_id);
    put_round(w,s.last_confirmed_round); put_clock(w,s.last_confirmation); put_clock(w,s.last_settings_change);
    w.u32(s.image_id); w.u32(s.image_count); w.u64(s.image_remaining_ms);
    w.boolean(s.image_due_pending); w.boolean(s.confirmation_pending_sync);
    w.boolean(s.divergent); w.boolean(s.catalog_mismatch); w.bytes(s.catalog_id,KATMED_CATALOG_BYTES);
    for (EventId id : s.recent_events) put_id(w,id);
    w.u8(s.recent_event_count); w.u8(s.recent_event_next);
}
AppState get_state(Reader &r) {
    AppState s{};
    s.schema_version=r.u32(); s.settings=get_settings(r); s.revision=get_revision(r);
    s.round=get_round(r); s.alarm_active=r.boolean(); s.start_reason=static_cast<StartReason>(r.u8());
    s.clock=get_clock(r); s.countdown_started=get_clock(r); s.countdown_origin=get_id(r);
    s.has_time_sync=r.boolean(); s.last_time_sync=get_clock(r); s.last_time_sync_id=get_id(r);
    s.deadline.regular_monotonic_ms=r.u64(); s.deadline.effective_monotonic_ms=r.u64();
    s.deadline.regular_utc=get_instant(r); s.deadline.effective_utc=get_instant(r); s.deadline.night_advanced=r.boolean();
    s.has_last_confirmation=r.boolean(); s.last_confirmation_id=get_id(r);
    s.last_confirmed_round=get_round(r); s.last_confirmation=get_clock(r); s.last_settings_change=get_clock(r);
    s.image_id=r.u32(); s.image_count=r.u32(); s.image_remaining_ms=r.u64();
    s.image_due_pending=r.boolean(); s.confirmation_pending_sync=r.boolean();
    s.divergent=r.boolean(); s.catalog_mismatch=r.boolean(); r.bytes(s.catalog_id,KATMED_CATALOG_BYTES);
    for (EventId &id : s.recent_events) id=get_id(r);
    s.recent_event_count=r.u8(); s.recent_event_next=r.u8();
    return s;
}
void put_pending(Writer &w, StorePendingConfirm p) {
    put_id(w,p.id); w.u64(p.sender); put_round(w,p.round);
    put_revision(w,p.parent_revision); put_clock(w,p.occurred_at);
}
StorePendingConfirm get_pending(Reader &r) {
    StorePendingConfirm p{}; p.id=get_id(r); p.sender=r.u64(); p.round=get_round(r);
    p.parent_revision=get_revision(r); p.occurred_at=get_clock(r); return p;
}
void put_confirmation(Writer &w, const StoreConfirmationHistory &h) {
    w.boolean(h.valid); put_id(w,h.id); put_round(w,h.round); put_clock(w,h.occurred_at);
}
StoreConfirmationHistory get_confirmation(Reader &r) {
    StoreConfirmationHistory h{};
    h.valid=r.boolean(); h.id=get_id(r); h.round=get_round(r); h.occurred_at=get_clock(r);
    return h;
}
StoreConfirmationHistory confirmation_from_state(const AppState &s) {
    StoreConfirmationHistory h{};
    if (s.has_last_confirmation) {
        h.valid=true; h.id=s.last_confirmation_id;
        h.round=s.last_confirmed_round; h.occurred_at=s.last_confirmation;
    }
    return h;
}

bool valid_id(EventId id) { return id.device != 0 && id.epoch != 0 && id.sequence != 0; }
bool same_id(EventId a, EventId b) { return a.device==b.device && a.epoch==b.epoch && a.sequence==b.sequence; }
bool same_round(AlarmRoundId a, AlarmRoundId b) {
    return same_id(a.origin,b.origin) && a.ordinal==b.ordinal;
}
bool same_clock(ClockSample a, ClockSample b) {
    return a.monotonic_ms==b.monotonic_ms && a.domain==b.domain &&
           a.utc_ms==b.utc_ms && a.utc_valid==b.utc_valid && a.quality==b.quality;
}
bool same_pending(StorePendingConfirm a, StorePendingConfirm b) {
    return same_id(a.id,b.id) && a.sender==b.sender && same_round(a.round,b.round) &&
           a.parent_revision.number==b.parent_revision.number &&
           a.parent_revision.coordinator==b.parent_revision.coordinator &&
           same_clock(a.occurred_at,b.occurred_at);
}
bool valid_clock(ClockSample c);
bool valid_pending(StorePendingConfirm p) {
    return valid_id(p.id) && p.sender==p.id.device && valid_id(p.round.origin) &&
           p.round.ordinal!=0 && p.parent_revision.coordinator!=0 &&
           valid_clock(p.occurred_at);
}
bool valid_clock(ClockSample c) {
    LocalDateTime local{};
    return c.domain != 0 && c.quality >= CLOCK_UNAVAILABLE && c.quality <= CLOCK_SYNCHRONIZED &&
           ((!c.utc_valid && c.utc_ms == 0 && c.quality == CLOCK_UNAVAILABLE) ||
            (c.utc_valid && c.quality != CLOCK_UNAVAILABLE &&
             app_berlin_local_time(c.utc_ms,&local)==APP_OK));
}
bool valid_settings(SharedSettings s) {
    return s.medication_seconds != 0 && s.image_seconds != 0 &&
           s.rotation >= ROTATION_OFF && s.rotation <= ROTATION_RANDOM && s.revision.coordinator != 0;
}
bool same_settings(SharedSettings a, SharedSettings b) {
    return a.medication_seconds==b.medication_seconds && a.image_seconds==b.image_seconds &&
           a.rotation==b.rotation && a.revision.number==b.revision.number &&
           a.revision.coordinator==b.revision.coordinator;
}
bool valid_local(LocalSettings l, uint64_t device) {
    return l.device == device && l.orientation >= ORIENTATION_0 && l.orientation <= ORIENTATION_270 &&
           l.clock_style >= DIGITAL_LARGE && l.clock_style <= ANALOG;
}
bool valid_utf8_text(const uint8_t *bytes, size_t length) {
    for (size_t i=0;i<length;) {
        const uint8_t c=bytes[i];
        if (c<0x80) {
            if (c<0x20 || c==0x7f) return false;
            ++i; continue;
        }
        unsigned extra=0;
        uint32_t value=0;
        if (c>=0xc2 && c<=0xdf) { extra=1; value=c&0x1f; }
        else if (c>=0xe0 && c<=0xef) { extra=2; value=c&0x0f; }
        else if (c>=0xf0 && c<=0xf4) { extra=3; value=c&0x07; }
        else return false;
        if (i+extra>=length) return false;
        for (unsigned j=1;j<=extra;++j) {
            const uint8_t next=bytes[i+j];
            if ((next&0xc0)!=0x80) return false;
            value=(value<<6)|(next&0x3f);
        }
        if ((extra==1 && value<0x80) || (extra==2 && value<0x800) ||
            (extra==3 && value<0x10000) || value>0x10ffff ||
            (value>=0xd800 && value<=0xdfff)) return false;
        i+=extra+1;
    }
    return true;
}
bool valid_provision(const StoreProvision &p) {
    if (!p.configured) {
        if (p.ssid_length != 0 || p.password_length != 0 || p.time_source_length != 0) return false;
        for (uint8_t b : p.group) if (b != 0) return false;
        for (uint8_t b : p.secret) if (b != 0) return false;
        for (uint8_t b : p.ssid) if (b != 0) return false;
        for (uint8_t b : p.password) if (b != 0) return false;
        for (uint8_t b : p.time_source) if (b != 0) return false;
        return true;
    }
    if (p.ssid_length == 0 || p.ssid_length > sizeof(p.ssid) || p.password_length < 8 ||
        p.password_length > sizeof(p.password) || p.time_source_length == 0 ||
        p.time_source_length > sizeof(p.time_source)) return false;
    bool group_nonzero=false, secret_nonzero=false;
    for (uint8_t b : p.group) group_nonzero |= b != 0;
    for (uint8_t b : p.secret) secret_nonzero |= b != 0;
    if (!group_nonzero || !secret_nonzero) return false;
    if (!valid_utf8_text(p.ssid,p.ssid_length) ||
        !valid_utf8_text(p.password,p.password_length)) return false;
    for (size_t i=p.ssid_length;i<sizeof(p.ssid);++i) if (p.ssid[i]!=0) return false;
    for (size_t i=p.password_length;i<sizeof(p.password);++i) if (p.password[i]!=0) return false;
    for (size_t i=p.time_source_length;i<sizeof(p.time_source);++i) if (p.time_source[i]!=0) return false;
    for (size_t i=0;i<p.time_source_length;++i) {
        const uint8_t c=p.time_source[i];
        if (!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='-')) return false;
    }
    return true;
}

bool valid_record(const StoreRecord &v) {
    if (v.device==0 || v.epoch==0 || v.next_sequence < 2 || !valid_settings(v.shared) ||
        !valid_local(v.local,v.device) || v.pending_count > KATMED_STORE_PENDING_MAX ||
        v.conflict_count > KATMED_STORE_CONFLICT_MAX ||
        !valid_provision(v.provision)) return false;
    if (v.group_origin!=STORE_GROUP_ORIGIN_UNKNOWN &&
        v.group_origin!=STORE_GROUP_ORIGIN_FRESH_UNBOUND &&
        v.group_origin!=STORE_GROUP_ORIGIN_FIRST_BOUND) return false;
    bool bound=false;
    for (uint8_t byte : v.bound_group) bound |= byte!=0;
    if ((v.group_origin==STORE_GROUP_ORIGIN_FRESH_UNBOUND &&
         (bound || v.provision.configured)) ||
        (v.group_origin==STORE_GROUP_ORIGIN_FIRST_BOUND && !bound)) return false;
    if (v.provision.configured &&
        std::memcmp(v.provision.group,v.bound_group,sizeof(v.bound_group))!=0)
        return false;
    if (v.has_history) {
        StateSnapshot probe{};
        if (app_make_snapshot(&v.history,&v.history.clock,&probe) != APP_OK ||
            std::memcmp(v.history.catalog_id,v.catalog_id,KATMED_CATALOG_BYTES)!=0 ||
            v.history.image_count != v.image_count ||
            !same_settings(v.history.settings,v.shared)) return false;
    }
    const StoreConfirmationHistory &h=v.last_confirmation;
    if (h.valid) {
        if (!valid_id(h.id) || !valid_id(h.round.origin) || h.round.ordinal==0 ||
            !valid_clock(h.occurred_at)) return false;
    } else {
        if (h.id.device!=0 || h.id.epoch!=0 || h.id.sequence!=0 ||
            h.round.origin.device!=0 || h.round.origin.epoch!=0 ||
            h.round.origin.sequence!=0 || h.round.ordinal!=0 ||
            h.occurred_at.monotonic_ms!=0 || h.occurred_at.domain!=0 ||
            h.occurred_at.utc_ms!=0 || h.occurred_at.utc_valid ||
            h.occurred_at.quality!=CLOCK_UNAVAILABLE) return false;
    }
    if (v.has_history && v.history.has_last_confirmation) {
        if (!h.valid || !same_id(h.id,v.history.last_confirmation_id) ||
            !same_id(h.round.origin,v.history.last_confirmed_round.origin) ||
            h.round.ordinal!=v.history.last_confirmed_round.ordinal ||
            h.occurred_at.monotonic_ms!=v.history.last_confirmation.monotonic_ms ||
            h.occurred_at.domain!=v.history.last_confirmation.domain ||
            h.occurred_at.utc_ms!=v.history.last_confirmation.utc_ms ||
            h.occurred_at.utc_valid!=v.history.last_confirmation.utc_valid ||
            h.occurred_at.quality!=v.history.last_confirmation.quality) return false;
    }
    for (unsigned i=0;i<v.pending_count;++i) {
        const StorePendingConfirm &p=v.pending[i];
        if (!valid_pending(p)) return false;
        for (unsigned j=0;j<i;++j) if (same_id(v.pending[j].id,p.id)) return false;
    }
    for (unsigned i=0;i<v.conflict_count;++i) {
        const StorePendingConfirm &c=v.conflicts[i];
        if (!valid_pending(c)) return false;
        for (unsigned j=0;j<i;++j) if (same_id(v.conflicts[j].id,c.id)) return false;
        for (unsigned j=0;j<v.pending_count;++j)
            if (same_id(v.pending[j].id,c.id)) return false;
    }
    if (v.conflict_count>0 && !v.group_divergent) return false;
    return true;
}

StoreResult encode_record(const StoreRecord &v, uint8_t *bytes, size_t *length, uint16_t version=kCurrentVersion) {
    if (!valid_record(v) || !bytes || !length) return STORE_INVALID;
    Writer w{bytes,KATMED_STORE_BLOB_MAX};
    w.bytes(kMagic,4); w.u16(version); w.u16(0); w.u32(0);
    w.u64(v.device); w.u64(v.epoch); w.u64(v.next_sequence);
    w.bytes(v.catalog_id,KATMED_CATALOG_BYTES); w.u32(v.image_count);
    put_settings(w,v.shared); w.u8(static_cast<uint8_t>(v.local.orientation));
    if (version >= 2) w.u8(static_cast<uint8_t>(v.local.clock_style));
    w.u64(v.local.device); w.boolean(v.has_history);
    if (v.has_history) put_state(w,v.history);
    w.u8(v.pending_count);
    for (unsigned i=0;i<v.pending_count;++i) put_pending(w,v.pending[i]);
    if (version>=5) {
        w.u8(v.conflict_count);
        for (unsigned i=0;i<v.conflict_count;++i) put_pending(w,v.conflicts[i]);
        w.boolean(v.group_divergent);
    }
    w.boolean(v.provision.configured); w.bytes(v.provision.group,sizeof(v.provision.group));
    w.bytes(v.provision.secret,sizeof(v.provision.secret));
    w.u8(v.provision.ssid_length); w.bytes(v.provision.ssid,sizeof(v.provision.ssid));
    w.u8(v.provision.password_length); w.bytes(v.provision.password,sizeof(v.provision.password));
    w.u8(v.provision.time_source_length); w.bytes(v.provision.time_source,sizeof(v.provision.time_source));
    if (version>=4) w.bytes(v.bound_group,sizeof(v.bound_group));
    if (version>=3) put_confirmation(w,v.last_confirmation);
    if (version>=6) w.u8(static_cast<uint8_t>(v.group_origin));
    if (!w.good || w.position > UINT16_MAX) return STORE_FULL;
    const uint16_t n=static_cast<uint16_t>(w.position);
    bytes[6]=static_cast<uint8_t>(n); bytes[7]=static_cast<uint8_t>(n>>8);
    const uint32_t crc=checksum(bytes+kHeaderSize,n-kHeaderSize);
    for (unsigned i=0;i<4;++i) bytes[8+i]=static_cast<uint8_t>(crc>>(8*i));
    *length=n; return STORE_OK;
}

StoreResult decode_record(const uint8_t *bytes, size_t length, StoreRecord *out) {
    if (!bytes || !out || length < kHeaderSize || length > KATMED_STORE_BLOB_MAX) return STORE_CORRUPT;
    Reader r{bytes,length};
    uint8_t magic[4]; r.bytes(magic,4);
    const uint16_t version=r.u16(); const uint16_t stated_length=r.u16(); const uint32_t crc=r.u32();
    if (std::memcmp(magic,kMagic,4)!=0 || stated_length!=length) return STORE_CORRUPT;
    if (version<1 || version>kCurrentVersion) return STORE_UNSUPPORTED;
    if (crc!=checksum(bytes+kHeaderSize,length-kHeaderSize)) return STORE_CORRUPT;
    /* store_open supplies its already allocated staged record. Keeping a
     * second StoreRecord here exhausts the C3 main stack when history is
     * validated by app_make_snapshot during a pending-confirmation reboot. */
    StoreRecord &v=*out;
    v={};
    const auto corrupt=[out]() {
        *out=StoreRecord{}; /* Do not retain a partially decoded record. */
        return STORE_CORRUPT;
    };
    v.device=r.u64(); v.epoch=r.u64(); v.next_sequence=r.u64();
    r.bytes(v.catalog_id,KATMED_CATALOG_BYTES); v.image_count=r.u32();
    v.shared=get_settings(r); v.local.orientation=static_cast<Orientation>(r.u8());
    v.local.clock_style=version==1 ? DIGITAL_LARGE : static_cast<ClockStyle>(r.u8());
    if (v.local.clock_style< DIGITAL_LARGE || v.local.clock_style>ANALOG) v.local.clock_style=DIGITAL_LARGE;
    v.local.device=r.u64(); v.has_history=r.boolean();
    if (v.has_history) v.history=get_state(r);
    v.pending_count=r.u8();
    if (v.pending_count>KATMED_STORE_PENDING_MAX) return corrupt();
    for (unsigned i=0;i<v.pending_count;++i) v.pending[i]=get_pending(r);
    if (version>=5) {
        v.conflict_count=r.u8();
        if (v.conflict_count>KATMED_STORE_CONFLICT_MAX) return corrupt();
        for (unsigned i=0;i<v.conflict_count;++i) v.conflicts[i]=get_pending(r);
        v.group_divergent=r.boolean();
    } else if (v.has_history) {
        v.group_divergent=v.history.divergent;
    }
    v.provision.configured=r.boolean(); r.bytes(v.provision.group,sizeof(v.provision.group));
    r.bytes(v.provision.secret,sizeof(v.provision.secret));
    v.provision.ssid_length=r.u8(); r.bytes(v.provision.ssid,sizeof(v.provision.ssid));
    v.provision.password_length=r.u8(); r.bytes(v.provision.password,sizeof(v.provision.password));
    v.provision.time_source_length=r.u8(); r.bytes(v.provision.time_source,sizeof(v.provision.time_source));
    if (version>=4) r.bytes(v.bound_group,sizeof(v.bound_group));
    else if (v.provision.configured)
        std::memcpy(v.bound_group,v.provision.group,sizeof(v.bound_group));
    if (version>=3) v.last_confirmation=get_confirmation(r);
    else if (v.has_history && v.history.has_last_confirmation)
        v.last_confirmation=confirmation_from_state(v.history);
    else if (v.pending_count>0) {
        const StorePendingConfirm &last=v.pending[v.pending_count-1];
        v.last_confirmation={true,last.id,last.round,last.occurred_at};
    }
    if (version>=6) v.group_origin=static_cast<StoreGroupOrigin>(r.u8());
    if (!r.good || r.position!=length || !valid_record(v)) return corrupt();
    return STORE_OK;
}

StoreResult write_record(Store *store, const StoreRecord &next) {
    size_t length=0;
    const StoreResult encoded=encode_record(next,store->scratch,&length);
    if (encoded!=STORE_OK) return store->last_error=encoded;
    const StoreResult written=store->backend.write_commit(store->backend.context,store->scratch,length);
    std::memset(store->scratch,0,sizeof(store->scratch));
    if (written!=STORE_OK) {
        if (store->backend_commit_failures!=UINT32_MAX) ++store->backend_commit_failures;
        store->uncertain=true;
        return store->last_error=written;
    }
    store->record=next;
    store->last_error=STORE_OK;
    return STORE_OK;
}

bool ready(const Store *store) { return store && store->open && !store->uncertain; }

/* The blob has one historical state and bounded pending/conflict journals, but
 * no second group's archive. Refuse a domain switch while there is any
 * durable group-related information instead of silently deleting it. */
bool has_group_state(const StoreRecord &record) {
    const SharedSettings defaults=app_default_settings();
    return record.has_history || record.pending_count>0 || record.conflict_count>0 ||
           record.group_divergent ||
           record.last_confirmation.valid ||
           record.shared.medication_seconds!=defaults.medication_seconds ||
           record.shared.image_seconds!=defaults.image_seconds ||
           record.shared.rotation!=defaults.rotation ||
           record.shared.revision.number!=0 ||
           record.shared.revision.coordinator!=record.device;
}

const char *error_token(StoreResult result) {
    switch (result) {
    case STORE_OK: return "OK";
    case STORE_NOT_FOUND: return "NOT_FOUND";
    case STORE_INVALID: return "INVALID";
    case STORE_CORRUPT: return "CORRUPT";
    case STORE_UNSUPPORTED: return "UNSUPPORTED";
    case STORE_FULL: return "FULL";
    case STORE_IO: return "IO";
    case STORE_COMMIT_UNKNOWN: return "COMMIT_UNKNOWN";
    case STORE_DUPLICATE: return "DUPLICATE";
    case STORE_STALE: return "STALE";
    case STORE_UNAVAILABLE: return "UNAVAILABLE";
    }
    return "UNKNOWN";
}
void hex_encode(const uint8_t *bytes, size_t n, char *out) {
    static const char h[]="0123456789abcdef";
    for (size_t i=0;i<n;++i) { out[2*i]=h[bytes[i]>>4]; out[2*i+1]=h[bytes[i]&15]; }
    out[2*n]='\0';
}
int nibble(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return -1;
}
bool parse_hex(const char *text, size_t n, uint8_t *out, size_t capacity, size_t *decoded) {
    if ((n&1)!=0 || n/2>capacity) return false;
    for (size_t i=0;i<n/2;++i) {
        const int hi=nibble(text[2*i]), lo=nibble(text[2*i+1]);
        if (hi<0 || lo<0) return false;
        out[i]=static_cast<uint8_t>((hi<<4)|lo);
    }
    *decoded=n/2;
    return true;
}
bool token_safe(const char *value) {
    if (!value) return false;
    const size_t n=std::strlen(value);
    if (n==0 || n>24) return false;
    for (size_t i=0;i<n;++i) {
        const char c=value[i];
        if (!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
              c=='.'||c=='-'||c=='_'||c=='+')) return false;
    }
    return true;
}
StoreResult make_reply(char *out, size_t capacity, const char *text) {
    if (!out || capacity==0) return STORE_INVALID;
    const size_t n=std::strlen(text);
    if (n>=capacity || n>KATMED_STORE_USB_REPLY_MAX) return STORE_FULL;
    std::memcpy(out,text,n+1);
    return STORE_OK;
}
} // namespace

extern "C" {

bool store_diagnostics(const Store *store, StoreDiagnostics *out) {
    if (!store || !out || !store->open) return false;
    *out={store->backend_read_failures,store->decoded_load_failures,
          store->backend_commit_failures};
    return true;
}

StoreResult store_open(Store *store, StoreBackend backend, uint64_t device,
                       const uint8_t catalog_id[KATMED_CATALOG_BYTES],
                       uint32_t image_count, StoreRecovery *recovery) {
    if (!store || !backend.read || !backend.write_commit || !catalog_id || !recovery || device==0)
        return STORE_INVALID;
    *store=Store{};
    store->backend=backend;
    store->current_image_count=image_count;
    std::memcpy(store->current_catalog,catalog_id,KATMED_CATALOG_BYTES);
    size_t length=sizeof(store->scratch);
    const StoreResult read=backend.read(backend.context,store->scratch,&length);
    if (read==STORE_NOT_FOUND) {
        StoreRecord &r=store->staged;
        r.device=device; r.epoch=1; r.next_sequence=2;
        r.group_origin=STORE_GROUP_ORIGIN_FRESH_UNBOUND;
        r.image_count=image_count;
        std::memcpy(r.catalog_id,catalog_id,KATMED_CATALOG_BYTES);
        r.shared=app_default_settings();
        r.shared.revision.coordinator=device;
        r.local={ORIENTATION_0,DIGITAL_LARGE,device};
        const StoreResult committed=write_record(store,r);
        if (committed!=STORE_OK) return committed;
    } else if (read==STORE_OK) {
        const StoreResult decoded=decode_record(store->scratch,length,&store->staged);
        std::memset(store->scratch,0,sizeof(store->scratch));
        if (decoded!=STORE_OK || store->staged.device!=device ||
            store->staged.epoch==UINT64_MAX) {
            if (store->decoded_load_failures!=UINT32_MAX) ++store->decoded_load_failures;
            return store->last_error=decoded!=STORE_OK ? decoded :
                (store->staged.device!=device ? STORE_CORRUPT : STORE_FULL);
        }
        store->catalog_mismatch=std::memcmp(store->staged.catalog_id,catalog_id,KATMED_CATALOG_BYTES)!=0 ||
                                store->staged.image_count!=image_count;
        ++store->staged.epoch;
        store->staged.next_sequence=2;
        const StoreResult committed=write_record(store,store->staged);
        if (committed!=STORE_OK) return committed;
    } else {
        if (store->backend_read_failures!=UINT32_MAX) ++store->backend_read_failures;
        std::memset(store->scratch,0,sizeof(store->scratch));
        return store->last_error=read;
    }
    store->open=true;
    return store_get_recovery(store,recovery);
}

StoreResult store_get_recovery(const Store *store, StoreRecovery *recovery) {
    if (!ready(store) || !recovery) return STORE_UNAVAILABLE;
    StoreRecovery r{};
    r.shared=store->record.shared; r.local=store->record.local;
    r.boot_identity={store->record.device,store->record.epoch,1};
    if (store->record.last_confirmation.valid) {
        r.has_last_confirmation=true;
        r.last_confirmation_id=store->record.last_confirmation.id;
        r.last_confirmed_round=store->record.last_confirmation.round;
        r.last_confirmation=store->record.last_confirmation.occurred_at;
    }
    r.pending_count=store->record.pending_count;
    r.catalog_mismatch=store->catalog_mismatch;
    r.configured=store->record.provision.configured;
    *recovery=r;
    return STORE_OK;
}

StoreResult store_next_event_id(Store *store, EventId *id) {
    return store_reserve_event_ids(store,1,id);
}

StoreResult store_reserve_event_ids(Store *store, uint32_t count, EventId *first) {
    if (!ready(store) || !first) return STORE_UNAVAILABLE;
    if (count==0) return store->last_error=STORE_INVALID;
    if (store->record.next_sequence>UINT64_MAX-count) return store->last_error=STORE_FULL;
    store->staged=store->record;
    const EventId candidate={store->record.device,store->record.epoch,store->record.next_sequence};
    store->staged.next_sequence+=count;
    const StoreResult result=write_record(store,store->staged);
    if (result==STORE_OK) *first=candidate;
    return result;
}

StoreResult store_apply_event(Store *store, AppState *active, const AppEvent *event,
                              const ClockSample *now, bool queue_offline,
                              AppEffects *effects, AppResult *core_result) {
    if (effects) *effects=AppEffects{};
    if (core_result) *core_result=APP_INVALID;
    if (!ready(store) || !active || !event || !now) return STORE_UNAVAILABLE;
    if (queue_offline && (event->kind!=EVENT_CONFIRM || event->coordinator_accepted))
        return store->last_error=STORE_INVALID;
    if (event->sender==store->record.device &&
        (event->id.epoch!=store->record.epoch || event->id.sequence < 2 ||
         event->id.sequence>=store->record.next_sequence))
        return store->last_error=STORE_INVALID;
    AppState candidate=*active;
    AppEffects candidate_effects{};
    const AppResult applied=app_apply_event(&candidate,event,now,&candidate_effects);
    if (core_result) *core_result=applied;
    if (applied==APP_DUPLICATE) return store->last_error=STORE_DUPLICATE;
    if (applied==APP_STALE || applied==APP_CONFLICT) return store->last_error=STORE_STALE;
    if (applied!=APP_OK) return store->last_error=STORE_INVALID;
    if (queue_offline && store->record.pending_count==KATMED_STORE_PENDING_MAX)
        return store->last_error=STORE_FULL;
    if (queue_offline && !candidate_effects.persistence_required)
        return store->last_error=STORE_INVALID;
    if (candidate_effects.persistence_required) {
        store->staged=store->record;
        store->staged.has_history=true;
        store->staged.history=candidate;
        if (candidate.has_last_confirmation)
            store->staged.last_confirmation=confirmation_from_state(candidate);
        store->staged.shared=candidate.settings;
        store->staged.image_count=candidate.image_count;
        std::memcpy(store->staged.catalog_id,candidate.catalog_id,KATMED_CATALOG_BYTES);
        const bool mismatch=std::memcmp(candidate.catalog_id,store->current_catalog,KATMED_CATALOG_BYTES)!=0 ||
                            candidate.image_count!=store->current_image_count;
        if (queue_offline) {
            StorePendingConfirm &p=store->staged.pending[store->staged.pending_count++];
            p={event->id,event->sender,event->round,event->parent_revision,event->occurred_at};
        }
        const StoreResult committed=write_record(store,store->staged);
        if (committed!=STORE_OK) return committed;
        store->catalog_mismatch=mismatch;
    }
    *active=candidate;
    if (effects) *effects=candidate_effects;
    return store->last_error=STORE_OK;
}

StoreResult store_set_local(Store *store, LocalSettings *active,
                            const LocalSettings *requested) {
    if (!ready(store) || !active || !requested) return STORE_UNAVAILABLE;
    if (!valid_local(*requested,store->record.device)) return store->last_error=STORE_INVALID;
    if (active->orientation==requested->orientation && active->clock_style==requested->clock_style &&
        active->device==requested->device) return store->last_error=STORE_DUPLICATE;
    store->staged=store->record;
    store->staged.local=*requested;
    const StoreResult committed=write_record(store,store->staged);
    if (committed==STORE_OK) *active=*requested;
    return committed;
}

StoreResult store_ack_pending(Store *store, AppState *active, EventId id,
                              bool coordinator_accepted) {
    if (!ready(store)) return STORE_UNAVAILABLE;
    if (!coordinator_accepted || !valid_id(id)) return store->last_error=STORE_INVALID;
    unsigned at=0;
    while (at<store->record.pending_count && !same_id(store->record.pending[at].id,id)) ++at;
    if (at==store->record.pending_count) return store->last_error=STORE_NOT_FOUND;
    store->staged=store->record;
    for (unsigned i=at+1;i<store->staged.pending_count;++i)
        store->staged.pending[i-1]=store->staged.pending[i];
    --store->staged.pending_count;
    store->staged.pending[store->staged.pending_count]=StorePendingConfirm{};
    bool change_active=active && active->has_last_confirmation &&
                       same_id(active->last_confirmation_id,id);
    AppState candidate{};
    if (change_active) {
        candidate=*active; candidate.confirmation_pending_sync=false;
        store->staged.history=candidate;
        store->staged.has_history=true;
    } else if (store->staged.has_history && store->staged.history.has_last_confirmation &&
               same_id(store->staged.history.last_confirmation_id,id)) {
        store->staged.history.confirmation_pending_sync=false;
    }
    const StoreResult committed=write_record(store,store->staged);
    if (committed==STORE_OK && change_active) *active=candidate;
    return committed;
}

StoreResult store_pending_at(const Store *store, size_t index,
                             StorePendingConfirm *pending) {
    if (!ready(store) || !pending) return STORE_UNAVAILABLE;
    if (index>=store->record.pending_count) return STORE_NOT_FOUND;
    *pending=store->record.pending[index];
    return STORE_OK;
}

StoreResult store_record_conflict(Store *store, AppState *active,
                                  const StorePendingConfirm *conflict,
                                  const ClockSample *now) {
    if (!ready(store) || !active || !conflict || !now) return STORE_UNAVAILABLE;
    if (!valid_pending(*conflict)) return store->last_error=STORE_INVALID;
    for (unsigned i=0;i<store->record.conflict_count;++i) {
        if (same_id(store->record.conflicts[i].id,conflict->id))
            return store->last_error=same_pending(store->record.conflicts[i],*conflict)
                ? STORE_DUPLICATE : STORE_STALE;
    }
    for (unsigned i=0;i<store->record.pending_count;++i)
        if (same_id(store->record.pending[i].id,conflict->id))
            return store->last_error=STORE_STALE;
    if (store->record.conflict_count==KATMED_STORE_CONFLICT_MAX)
        return store->last_error=STORE_FULL;
    AppEvent event{};
    event.kind=EVENT_CONFIRM; event.id=conflict->id; event.sender=conflict->sender;
    event.round=conflict->round; event.parent_revision=conflict->parent_revision;
    event.occurred_at=conflict->occurred_at; event.coordinator_accepted=true;
    AppState probe=*active;
    AppEffects effects{};
    const AppResult applicability=app_apply_event(&probe,&event,now,&effects);
    if (applicability!=APP_STALE && applicability!=APP_CONFLICT)
        return store->last_error=STORE_INVALID;
    store->staged=store->record;
    store->staged.conflicts[store->staged.conflict_count++]=*conflict;
    store->staged.group_divergent=true;
    AppState candidate=*active;
    candidate.divergent=true;
    store->staged.has_history=true;
    store->staged.history=candidate;
    if (candidate.has_last_confirmation)
        store->staged.last_confirmation=confirmation_from_state(candidate);
    const StoreResult committed=write_record(store,store->staged);
    if (committed==STORE_OK) *active=candidate;
    return committed;
}

StoreResult store_reject_pending(Store *store, AppState *active, EventId id) {
    if (!ready(store) || !active || !valid_id(id)) return STORE_UNAVAILABLE;
    unsigned at=0;
    while (at<store->record.pending_count && !same_id(store->record.pending[at].id,id)) ++at;
    if (at==store->record.pending_count) {
        for (unsigned i=0;i<store->record.conflict_count;++i)
            if (same_id(store->record.conflicts[i].id,id))
                return store->last_error=STORE_DUPLICATE;
        return store->last_error=STORE_NOT_FOUND;
    }
    if (store->record.conflict_count==KATMED_STORE_CONFLICT_MAX)
        return store->last_error=STORE_FULL;
    store->staged=store->record;
    store->staged.conflicts[store->staged.conflict_count++]=store->staged.pending[at];
    store->staged.group_divergent=true;
    for (unsigned i=at+1;i<store->staged.pending_count;++i)
        store->staged.pending[i-1]=store->staged.pending[i];
    --store->staged.pending_count;
    store->staged.pending[store->staged.pending_count]=StorePendingConfirm{};
    AppState candidate=*active;
    candidate.divergent=true;
    if (candidate.has_last_confirmation && same_id(candidate.last_confirmation_id,id))
        candidate.confirmation_pending_sync=false;
    if (store->staged.has_history) {
        store->staged.history.divergent=true;
        if (store->staged.history.has_last_confirmation &&
            same_id(store->staged.history.last_confirmation_id,id))
            store->staged.history.confirmation_pending_sync=false;
    } else {
        store->staged.has_history=true;
        store->staged.history=candidate;
    }
    const StoreResult committed=write_record(store,store->staged);
    if (committed==STORE_OK) *active=candidate;
    return committed;
}

StoreResult store_mark_divergent(Store *store, AppState *active) {
    if (!ready(store) || !active) return STORE_UNAVAILABLE;
    if (store->record.group_divergent) {
        active->divergent=true;
        return store->last_error=STORE_DUPLICATE;
    }
    store->staged=store->record;
    store->staged.group_divergent=true;
    if (store->staged.has_history) store->staged.history.divergent=true;
    const StoreResult committed=write_record(store,store->staged);
    if (committed==STORE_OK) active->divergent=true;
    return committed;
}

StoreResult store_conflict_count(const Store *store, uint8_t *count) {
    if (!ready(store) || !count) return STORE_UNAVAILABLE;
    *count=store->record.conflict_count;
    return STORE_OK;
}

StoreResult store_conflict_at(const Store *store, size_t index,
                              StorePendingConfirm *conflict) {
    if (!ready(store) || !conflict) return STORE_UNAVAILABLE;
    if (index>=store->record.conflict_count) return STORE_NOT_FOUND;
    *conflict=store->record.conflicts[index];
    return STORE_OK;
}

bool store_durable_divergent(const Store *store) {
    return ready(store) && store->record.group_divergent;
}

StoreResult store_get_status(const Store *store, bool *configured,
                             uint8_t *pending_count, StoreResult *last_error) {
    if (!store) return STORE_INVALID;
    if (configured) *configured=store->open && store->record.provision.configured;
    if (pending_count) *pending_count=store->open ? store->record.pending_count : 0;
    if (last_error) *last_error=store->last_error;
    return store->open && !store->uncertain ? STORE_OK : STORE_UNAVAILABLE;
}

StoreResult store_usb_handle_line_with_wifi_defaults(
    Store *store, const char *line, size_t length,
    const StoreUsbStatusInfo *status,
    const uint8_t *default_ssid, size_t default_ssid_length,
    const uint8_t *default_password, size_t default_password_length,
    char *reply, size_t reply_capacity) {
    if (!line || !reply || reply_capacity==0) return STORE_INVALID;
    if (length==0 || length>KATMED_STORE_USB_LINE_MAX || line[length-1]!='\n') {
        make_reply(reply,reply_capacity,"KM1 ERR LENGTH\n"); return STORE_INVALID;
    }
    for (size_t i=0;i+1<length;++i) if (line[i]<0x20 || line[i]>0x7e) {
        make_reply(reply,reply_capacity,"KM1 ERR FORMAT\n"); return STORE_INVALID;
    }
    const size_t command_length=length-1;
    if (command_length<4 || std::memcmp(line,"KM1 ",4)!=0) {
        make_reply(reply,reply_capacity,"KM1 ERR VERSION\n"); return STORE_INVALID;
    }
    if (command_length==10 && std::memcmp(line,"KM1 STATUS",10)==0) {
        if (!status || status->device==0 || status->device>UINT64_C(0xFFFFFFFFFFFF) ||
            !token_safe(status->firmware_version) || !token_safe(status->build_id) ||
            status->clock_quality<CLOCK_UNAVAILABLE || status->clock_quality>CLOCK_SYNCHRONIZED) {
            make_reply(reply,reply_capacity,"KM1 ERR STATUS\n"); return STORE_INVALID;
        }
        char cat[65]; hex_encode(status->catalog_id,KATMED_CATALOG_BYTES,cat);
        bool configured=false; uint8_t pending=0; StoreResult error=STORE_UNAVAILABLE;
        const StoreResult durable=store_get_status(store,&configured,&pending,&error);
        char text[KATMED_STORE_USB_REPLY_MAX+1];
        const int n=std::snprintf(text,sizeof(text),
            "KM1 OK STATUS configured=%u durable=%u pending=%u clock=%u device=%012" PRIx64
            " catalog=%s firmware=%s build=%s error=%s\n",
            configured?1u:0u,durable==STORE_OK?1u:0u,static_cast<unsigned>(pending),
            static_cast<unsigned>(status->clock_quality),status->device,cat,
            status->firmware_version,status->build_id,error_token(error));
        if (n<0 || static_cast<size_t>(n)>=sizeof(text) ||
            make_reply(reply,reply_capacity,text)!=STORE_OK) {
            make_reply(reply,reply_capacity,"KM1 ERR OUTPUT\n"); return STORE_FULL;
        }
        return STORE_OK;
    }
    if (command_length==16 && std::memcmp(line,"KM1 RESET CONFIG",16)==0) {
        if (!ready(store)) { make_reply(reply,reply_capacity,"KM1 ERR UNAVAILABLE\n"); return STORE_UNAVAILABLE; }
        store->staged=store->record;
        store->staged.provision=StoreProvision{};
        const StoreResult committed=write_record(store,store->staged);
        if (committed==STORE_OK) make_reply(reply,reply_capacity,"KM1 OK RESET_CONFIG\n");
        else make_reply(reply,reply_capacity,"KM1 ERR COMMIT\n");
        return committed;
    }
    const char prefix[]="KM1 PROVISION group=";
    if (command_length>=sizeof(prefix)-1 && std::memcmp(line,prefix,sizeof(prefix)-1)==0) {
        if (!ready(store)) { make_reply(reply,reply_capacity,"KM1 ERR UNAVAILABLE\n"); return STORE_UNAVAILABLE; }
        struct Part { const char *name; uint8_t *data; size_t capacity; size_t *length; };
        StoreProvision p{}; size_t group_n=0, secret_n=0, ssid_n=0, password_n=0, time_n=0;
        Part parts[]={{"group=",p.group,sizeof(p.group),&group_n},
                      {"secret=",p.secret,sizeof(p.secret),&secret_n},
                      {"ssid=",p.ssid,sizeof(p.ssid),&ssid_n},
                      {"password=",p.password,sizeof(p.password),&password_n},
                      {"time=",p.time_source,sizeof(p.time_source),&time_n}};
        size_t pos=sizeof("KM1 PROVISION ")-1; bool valid=true;
        for (unsigned i=0;i<5 && valid;++i) {
            const size_t name_n=std::strlen(parts[i].name);
            if (pos+name_n>command_length || std::memcmp(line+pos,parts[i].name,name_n)!=0) { valid=false; break; }
            pos+=name_n;
            const size_t start=pos;
            while (pos<command_length && line[pos]!=' ') ++pos;
            valid=parse_hex(line+start,pos-start,parts[i].data,parts[i].capacity,parts[i].length);
            if (i<4) { if (pos>=command_length) valid=false; else ++pos; }
        }
        if (pos!=command_length || group_n!=16 || secret_n!=32 || time_n==0) valid=false;
        if (ssid_n==0 || password_n==0) {
            const bool valid_defaults = default_ssid != nullptr &&
                default_ssid_length > 0 && default_ssid_length <= sizeof(p.ssid) &&
                default_password != nullptr && default_password_length >= 8 &&
                default_password_length <= sizeof(p.password);
            if (ssid_n != 0 || password_n != 0 || !valid_defaults) valid=false;
            else {
                const StoreProvision &saved=store->record.provision;
                if (saved.configured && saved.ssid_length>0 &&
                    saved.ssid_length<=sizeof(saved.ssid) && saved.password_length>=8 &&
                    saved.password_length<=sizeof(saved.password)) {
                    std::memcpy(p.ssid,saved.ssid,saved.ssid_length);
                    std::memcpy(p.password,saved.password,saved.password_length);
                    ssid_n=saved.ssid_length;
                    password_n=saved.password_length;
                } else {
                    std::memcpy(p.ssid,default_ssid,default_ssid_length);
                    std::memcpy(p.password,default_password,default_password_length);
                    ssid_n=default_ssid_length;
                    password_n=default_password_length;
                }
            }
        }
        if (password_n<8) valid=false;
        p.ssid_length=static_cast<uint8_t>(ssid_n);
        p.password_length=static_cast<uint8_t>(password_n);
        p.time_source_length=static_cast<uint8_t>(time_n);
        p.configured=true;
        if (!valid || !valid_provision(p)) {
            std::memset(&p,0,sizeof(p));
            make_reply(reply,reply_capacity,"KM1 ERR CONFIG\n"); return STORE_INVALID;
        }
        const bool different_group=std::memcmp(store->record.bound_group,p.group,
                                               sizeof(p.group))!=0;
        const bool first_binding=store->record.group_origin==STORE_GROUP_ORIGIN_FRESH_UNBOUND;
        if (different_group && !first_binding &&
            (has_group_state(store->record) ||
             (store->record.group_origin==STORE_GROUP_ORIGIN_FIRST_BOUND &&
              !store->record.provision.configured))) {
            std::memset(&p,0,sizeof(p));
            make_reply(reply,reply_capacity,"KM1 ERR CONFIG\n");
            return store->last_error=STORE_STALE;
        }
        store->staged=store->record;
        store->staged.provision=p;
        std::memcpy(store->staged.bound_group,p.group,sizeof(p.group));
        if (first_binding) store->staged.group_origin=STORE_GROUP_ORIGIN_FIRST_BOUND;
        std::memset(&p,0,sizeof(p));
        const StoreResult committed=write_record(store,store->staged);
        if (committed==STORE_OK) make_reply(reply,reply_capacity,"KM1 OK PROVISION\n");
        else make_reply(reply,reply_capacity,"KM1 ERR COMMIT\n");
        return committed;
    }
    make_reply(reply,reply_capacity,"KM1 ERR COMMAND\n");
    return STORE_INVALID;
}

StoreResult store_usb_handle_line(Store *store, const char *line, size_t length,
                                  const StoreUsbStatusInfo *status,
                                  char *reply, size_t reply_capacity) {
    return store_usb_handle_line_with_wifi_defaults(store,line,length,status,
        nullptr,0,nullptr,0,reply,reply_capacity);
}

} // extern "C"
