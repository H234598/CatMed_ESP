#include "katmed_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned assertions = 0;
unsigned groups = 0;
uint64_t serial = 100;
const uint8_t catalog[KATMED_CATALOG_BYTES] = {0x42, 0x13};

void check(bool condition, const char *expression, int line) {
    ++assertions;
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)
#define OK(expression) CHECK((expression) == APP_OK)
void group(const char *name) { ++groups; std::printf("PASS %s\n", name); }

// Independent test calendar: simple year/month summation, no production helper,
// host timezone, mktime, wall clock, or timezone mutation.
int64_t utc(int year, unsigned month, unsigned day, unsigned hour = 0,
            unsigned minute = 0, unsigned second = 0, unsigned millisecond = 0) {
    const unsigned lengths[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int64_t days = 0;
    for (int y = 1970; y < year; ++y)
        days += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (unsigned m = 1; m < month; ++m) {
        days += lengths[m - 1];
        if (m == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ++days;
    }
    return (days + day - 1) * 86400000 + hour * 3600000 + minute * 60000 + second * 1000 + millisecond;
}
ClockSample known(int64_t at, uint64_t mono = 1000, uint64_t domain = 1) {
    return {mono, domain, at, true, CLOCK_SYNCHRONIZED};
}
ClockSample unknown(uint64_t mono = 1000, uint64_t domain = 1) {
    return {mono, domain, 0, false, CLOCK_UNAVAILABLE};
}
ClockSample later(ClockSample at, uint64_t ms) {
    at.monotonic_ms += ms;
    if (at.utc_valid) at.utc_ms += static_cast<int64_t>(ms);
    return at;
}
AppState boot(ClockSample now, uint32_t medication = 43200, uint32_t images = 21600,
               RotationMode rotation = ROTATION_NAME_ORDER, uint64_t device = 10) {
    SharedSettings settings = app_default_settings();
    settings.medication_seconds = medication;
    settings.image_seconds = images;
    settings.rotation = rotation;
    AppState s{};
    OK(app_init(&s, &settings, &now, {device, 1, 1}, 3, catalog));
    return s;
}
AppEvent event(const AppState &s, AppEventKind kind, ClockSample when, bool accepted = true) {
    AppEvent e{};
    e.kind = kind;
    e.id = {10, 9, ++serial};
    e.sender = 10;
    e.round = s.round;
    e.parent_revision = s.revision;
    e.occurred_at = when;
    e.coordinator_accepted = accepted;
    return e;
}
AppEvent confirm(AppState &s, ClockSample when, bool accepted = true) {
    AppEvent e = event(s, EVENT_CONFIRM, when, accepted);
    AppEffects fx{};
    OK(app_apply_event(&s, &e, &when, &fx));
    CHECK(fx.persistence_required && fx.alarm_ended && fx.broadcast_required);
    return e;
}
AppEffects tick(AppState &s, ClockSample when) {
    AppEffects fx{};
    const AppResult r = app_tick(&s, &when, &fx);
    CHECK(r == APP_OK || r == APP_NO_CHANGE);
    CHECK(!fx.persistence_required);
    return fx;
}
void unchanged(AppState &s, const AppEvent &e, ClockSample now, AppResult expected) {
    const AppState previous = s;
    AppEffects fx{true, true, true, true, true, true};
    const AppResult actual = app_apply_event(&s, &e, &now, &fx);
    if (actual != expected)
        std::fprintf(stderr, "Event %d: expected result %d, got %d\n", e.kind, expected, actual);
    CHECK(actual == expected);
    CHECK(std::memcmp(&previous, &s, sizeof(s)) == 0);
    CHECK(!fx.state_changed && !fx.alarm_started && !fx.alarm_ended &&
          !fx.image_changed && !fx.persistence_required && !fx.broadcast_required);
}

void night_table() {
    struct Row { int64_t start, regular, effective; uint32_t seconds; bool night; };
    const Row rows[] = {
        {utc(2026,1,15,8), utc(2026,1,15,20), utc(2026,1,15,20), 43200, false},
        {utc(2026,1,15,13), utc(2026,1,16,1), utc(2026,1,15,23), 43200, true},
        {utc(2026,1,15,17), utc(2026,1,16,5), utc(2026,1,15,23), 43200, true},
        {utc(2026,1,15,17,0,1), utc(2026,1,16,5,0,1), utc(2026,1,16,5,0,1), 43200, false},
        {utc(2026,1,14,23,30), utc(2026,1,15,3,30), utc(2026,1,15,3,30), 14400, false},
        {utc(2026,1,15,11), utc(2026,1,15,23), utc(2026,1,15,23), 43200, false}
    };
    for (const auto &row : rows) {
        ClockSample start = known(row.start);
        Deadline d{};
        OK(app_calculate_deadline(&start, row.seconds, &d));
        CHECK(d.regular_utc.valid && d.effective_utc.valid);
        CHECK(d.regular_utc.ms == row.regular && d.effective_utc.ms == row.effective);
        CHECK(d.regular_monotonic_ms == 1000 + uint64_t{row.seconds} * 1000);
        CHECK(d.effective_monotonic_ms == 1000 + static_cast<uint64_t>(row.effective - row.start));
        CHECK(d.night_advanced == row.night);
    }
    group("2.2: alle sechs Tabellenzeilen mit expliziten UTC-Erwartungen");
    const Row edges[] = {
        {utc(2026,1,15,11), utc(2026,1,15,23), utc(2026,1,15,23), 43200, false},
        {utc(2026,1,15,11,0,59), utc(2026,1,15,23,0,59), utc(2026,1,15,23,0,59), 43200, false},
        {utc(2026,1,15,11,1), utc(2026,1,15,23,1), utc(2026,1,15,23), 43200, true},
        {utc(2026,1,15,17), utc(2026,1,16,5), utc(2026,1,15,23), 43200, true},
        {utc(2026,1,15,17,0,1), utc(2026,1,16,5,0,1), utc(2026,1,16,5,0,1), 43200, false},
        {utc(2026,1,15,17,0,0,1), utc(2026,1,16,5,0,0,1), utc(2026,1,16,5,0,0,1), 43200, false},
        {utc(2026,1,15,11,0,59,999), utc(2026,1,15,23,0,59,999), utc(2026,1,15,23,0,59,999), 43200, false},
        {utc(2026,1,15,23), utc(2026,1,16,3), utc(2026,1,16,3), 14400, false}
    };
    for (const auto &row : edges) {
        ClockSample start = known(row.start);
        Deadline d{};
        OK(app_calculate_deadline(&start, row.seconds, &d));
        CHECK(d.regular_utc.ms == row.regular && d.effective_utc.ms == row.effective);
        CHECK(d.night_advanced == row.night);
    }
    group("Nachtgrenzen: 00:00, 00:00:59, 00:01, 06:00, 06:00:01 und Millisekunden");
}

void timezone_tests() {
    struct Row { int64_t when; unsigned hour, minute, second; bool dst; };
    const Row rows[] = {
        {utc(2026,3,29,0,59,59),1,59,59,false},
        {utc(2026,3,29,1),3,0,0,true},
        {utc(2026,10,25,0,59,59),2,59,59,true},
        {utc(2026,10,25,1),2,0,0,false},
        {utc(2026,10,25,0,30),2,30,0,true},
        {utc(2026,10,25,1,30),2,30,0,false},
        {utc(2024,2,29,11,30),12,30,0,false}
    };
    for (const auto &row : rows) {
        LocalDateTime local{};
        OK(app_berlin_local_time(row.when, &local));
        CHECK(local.hour == row.hour && local.minute == row.minute && local.second == row.second);
        CHECK(local.daylight_saving == row.dst && local.utc_offset_seconds == (row.dst ? 7200 : 3600));
    }
    ClockSample spring = known(utc(2026,3,28,18));
    ClockSample autumn = known(utc(2026,10,24,18));
    Deadline d{};
    OK(app_calculate_deadline(&spring, 43200, &d));
    CHECK(d.regular_utc.ms == utc(2026,3,29,6));
    CHECK(d.regular_monotonic_ms == 43201000);
    OK(app_calculate_deadline(&autumn, 43200, &d));
    CHECK(d.regular_utc.ms == utc(2026,10,25,6));
    CHECK(d.regular_monotonic_ms == 43201000);
    LocalDateTime local{};
    CHECK(app_berlin_local_time(utc(1995,12,31,23), &local) == APP_INVALID);
    OK(app_berlin_local_time(utc(1996,1,1), &local));
    CHECK(app_berlin_local_time(utc(2099,12,31,23), &local) == APP_INVALID);
    CHECK(app_berlin_local_time(INT64_MAX, &local) == APP_INVALID);
    CHECK(app_berlin_local_time(INT64_MIN, &local) == APP_INVALID);
    group("Europe/Berlin: beide Sommerzeitwechsel, reale zwölf Stunden, Schaltjahr, Bereichsgrenzen");
}

void preview_tests() {
    ManualTimePreview p{};
    ClockSample now = known(utc(2026,10,25,0,15));
    OK(app_preview_local_time(&now, 2, 30, &p));
    CHECK(p.regular_utc.ms == utc(2026,10,25,0,30));
    CHECK(p.regular_local.daylight_saving && p.regular_local.utc_offset_seconds == 7200);
    CHECK(!p.night_advanced);
    now = known(utc(2026,10,25,0,45));
    OK(app_preview_local_time(&now, 2, 30, &p));
    CHECK(p.regular_utc.ms == utc(2026,10,25,1,30));
    CHECK(!p.regular_local.daylight_saving && p.regular_local.utc_offset_seconds == 3600);
    now = known(utc(2026,10,25,0,30));
    OK(app_preview_local_time(&now, 2, 30, &p));
    CHECK(p.regular_utc.ms == utc(2026,10,25,1,30));
    now = known(utc(2026,10,25,1,30));
    OK(app_preview_local_time(&now, 2, 30, &p));
    CHECK(p.regular_utc.ms == utc(2026,10,26,1,30));
    CHECK(p.effective_utc.ms == utc(2026,10,25,23) && p.night_advanced);
    now = known(utc(2026,3,29,0,15));
    OK(app_preview_local_time(&now, 2, 30, &p));
    CHECK(p.regular_utc.ms == utc(2026,3,30,0,30));
    CHECK(p.effective_utc.ms == utc(2026,3,29,22));
    CHECK(p.skipped_nonexistent_time && p.regular_local.day == 30 && p.night_advanced);
    now = known(utc(2026,1,15,12));
    OK(app_preview_local_time(&now, 2, 0, &p));
    CHECK(p.regular_utc.ms == utc(2026,1,16,1));
    CHECK(p.effective_utc.ms == utc(2026,1,15,23));
    CHECK(app_preview_local_time(&now,24,0,&p) == APP_INVALID);
    CHECK(app_preview_local_time(&now,23,60,&p) == APP_INVALID);
    now = unknown();
    CHECK(app_preview_local_time(&now,2,30,&p) == APP_TIME_UNAVAILABLE);
    group("HH:MM: beide doppelte Instanzen, exakte Jetzt-Grenze, fehlende Stunde und expliziter Folgetag");

    now = known(utc(2026,1,15,11));
    AppState s = boot(now);
    confirm(s, now);
    ClockSample save = later(now, 60000);
    OK(app_preview_local_time(&save, 14, 0, &p));
    AppEvent e = event(s, EVENT_SET_NEXT_LOCAL_TIME, save);
    e.value.local_time = {14, 0, p.regular_utc, p.effective_utc};
    OK(app_apply_event(&s, &e, &save, nullptr));
    CHECK(s.deadline.regular_utc.ms == utc(2026,1,15,13));
    CHECK(s.settings.medication_seconds == 43200 && s.start_reason == START_MANUAL);
    ClockSample due = known(utc(2026,1,15,13), now.monotonic_ms + 7200000);
    tick(s, due);
    CHECK(s.alarm_active);
    confirm(s, due);
    CHECK(s.deadline.regular_utc.ms == utc(2026,1,16,1));
    CHECK(s.deadline.effective_utc.ms == utc(2026,1,15,23));
    AppState stale = boot(now);
    confirm(stale, now);
    OK(app_preview_local_time(&now, 12, 1, &p));
    e = event(stale, EVENT_SET_NEXT_LOCAL_TIME, later(now,120000));
    e.value.local_time = {12,1,p.regular_utc,p.effective_utc};
    unchanged(stale,e,e.occurred_at,APP_STALE);
    e.value.local_time.selected_utc.valid = false;
    unchanged(stale,e,e.occurred_at,APP_INVALID);
    group("HH:MM-Speicherung: bindende Vorschau, veraltete Vorschau atomar abgewiesen, danach Stundenintervall");

    now = known(utc(2026,1,15,22));
    stale = boot(now); confirm(stale,now);
    ClockSample shown = later(now,3540000);
    OK(app_preview_local_time(&shown,2,0,&p));
    CHECK(p.regular_utc.ms == utc(2026,1,16,1));
    CHECK(p.effective_utc.ms == utc(2026,1,15,23));
    ClockSample saved = later(now,3601000);
    e = event(stale,EVENT_SET_NEXT_LOCAL_TIME,saved);
    e.value.local_time = {2,0,p.regular_utc,p.effective_utc};
    unchanged(stale,e,saved,APP_STALE);
    // A new consciously accepted preview now has the regular 02:00 deadline.
    OK(app_preview_local_time(&saved,2,0,&p));
    CHECK(p.regular_utc.ms == utc(2026,1,16,1) && p.effective_utc.ms == p.regular_utc.ms);
    e.value.local_time = {2,0,p.regular_utc,p.effective_utc};
    OK(app_apply_event(&stale,&e,&saved,nullptr));
    CHECK(stale.deadline.effective_utc.ms == utc(2026,1,16,1));
    group("HH:MM-Mitternachtsgrenze: auch wirksame Preview ist gebunden und benötigt nach Ablauf neue Zustimmung");
}

void parse_tests() {
    struct Row { const char *text; uint32_t expected; };
    const Row rows[] = {{"12",43200},{"6",21600},{"0.5",1800},{"0,5",1800},
        {".5",1800},{",5",1800},{"1.25",4500},{"0.000277778",1},
        {"0.000138889",1},{"1193046.470833333",UINT32_MAX}};
    for (const auto &row : rows) {
        uint32_t output = 17;
        OK(app_parse_hours(row.text,&output));
        CHECK(output == row.expected);
    }
    const char *bad[] = {"",".",",","0","-1","+1","NaN","inf","Infinity","1e3", " 1", "1 ",
        "1.2.3","1,2,3","1,2.3","1x","1.","1,","0.000000001","0.000138888","1.1234567890"};
    for (const char *text : bad) {
        uint32_t output = 17;
        CHECK(app_parse_hours(text,&output) == APP_INVALID);
        CHECK(output == 17);
    }
    const char *overflow[] = {"1193046.471", "1193047", "9999999999999999999999999999999999999999"};
    for (const char *text : overflow) {
        uint32_t output = 17;
        CHECK(app_parse_hours(text,&output) == APP_OVERFLOW);
        CHECK(output == 17);
    }
    CHECK(app_parse_hours(nullptr,nullptr) == APP_INVALID);
    group("Stundeneingaben: Punkt/Komma, ganzzahlige Rundung, Null/NaN/Inf/Fehlformat/Überlauf");
}

void lifecycle_tests() {
    ClockSample now = known(utc(2026,1,15,8));
    AppState s = boot(now);
    CHECK(s.alarm_active && s.start_reason == START_BOOT && !s.has_last_confirmation);
    CHECK(s.image_id == 0 && s.image_remaining_ms == 21600000);
    ClockSample first = later(now,3000);
    tick(s,first);
    CHECK(s.alarm_active && s.image_remaining_ms == 21600000);
    AppEvent c = confirm(s,first,false);
    CHECK(!s.alarm_active && s.has_last_confirmation && s.confirmation_pending_sync);
    CHECK(s.last_confirmation.utc_ms == utc(2026,1,15,8,0,3));
    CHECK(s.deadline.regular_utc.ms == utc(2026,1,15,20,0,3));
    unchanged(s,c,first,APP_DUPLICATE);
    AppEvent premature = event(s,EVENT_CONFIRM,first);
    unchanged(s,premature,first,APP_STALE);
    ClockSample change = later(first,3600000);
    AppEvent e = event(s,EVENT_SET_MEDICATION_INTERVAL,change);
    e.value.interval_seconds = 14400;
    e.coordinator_accepted = false;
    unchanged(s,e,change,APP_NOT_AUTHORIZED);
    e.coordinator_accepted = true;
    OK(app_apply_event(&s,&e,&change,nullptr));
    CHECK(s.deadline.regular_utc.ms == utc(2026,1,15,13,0,3));
    CHECK(s.last_confirmation.utc_ms == first.utc_ms && s.last_settings_change.utc_ms == change.utc_ms);
    CHECK(s.start_reason == START_SETTINGS && s.round.origin.sequence == c.id.sequence);
    ClockSample due = later(change,14400000);
    AppEffects fx = tick(s,due);
    CHECK(s.alarm_active && fx.alarm_started);
    unchanged(s,c,due,APP_DUPLICATE);
    e = event(s,EVENT_SET_MEDICATION_INTERVAL,due);
    e.value.interval_seconds = 3600;
    unchanged(s,e,due,APP_CONFLICT);
    e = event(s,EVENT_SET_NEXT_LOCAL_TIME,due);
    e.value.local_time = {12,0,{true,utc(2026,1,16,11)},{true,utc(2026,1,16,11)}};
    unchanged(s,e,due,APP_CONFLICT);
    confirm(s,due);
    CHECK(!s.alarm_active && s.deadline.regular_monotonic_ms == due.monotonic_ms + 14400000);
    group("Bootalarm, echte Runde, Offlinebestätigung, Speichern ab jetzt und unveränderte letzte Gabe");

    AppEvent old = event(s,EVENT_SET_IMAGE_INTERVAL,due);
    old.value.interval_seconds = 100;
    OK(app_apply_event(&s,&old,&due,nullptr));
    for (unsigned i=0;i<20;++i) {
        AppEvent mode = event(s,EVENT_SET_ROTATION_MODE,due);
        mode.value.rotation = i % 2 ? ROTATION_OFF : ROTATION_NAME_ORDER;
        OK(app_apply_event(&s,&mode,&due,nullptr));
    }
    CHECK(s.recent_event_count == KATMED_RECENT_EVENTS);
    unchanged(s,old,due,APP_STALE);
    unchanged(s,c,due,APP_STALE);
    AppEvent wrong_round = event(s,EVENT_CONFIRM,due);
    wrong_round.round.ordinal -= 1;
    unchanged(s,wrong_round,due,APP_STALE);
    AppEvent conflicting = event(s,EVENT_SET_ROTATION_MODE,due);
    conflicting.parent_revision.coordinator = 88;
    conflicting.value.rotation = ROTATION_OFF;
    unchanged(s,conflicting,due,APP_CONFLICT);
    group("Mehr als 16 Ereignisse: alte Settings/Bestätigungen kausal gesperrt, Koordinatorkonflikt atomar");
}

void sync_tests() {
    ClockSample base = unknown(1000);
    AppState s = boot(base);
    confirm(s,base);
    ClockSample first = later(base,3600000);
    first.utc_valid = true; first.quality = CLOCK_SYNCHRONIZED; first.utc_ms = utc(2026,1,15,14);
    AppEvent sync = event(s,EVENT_TIME_SYNC,first);
    sync.value.time_sync = first;
    const uint64_t rev = s.revision.number;
    AppEffects fx{};
    OK(app_apply_event(&s,&sync,&first,&fx));
    CHECK(s.revision.number == rev && !fx.persistence_required && !fx.broadcast_required);
    CHECK(s.deadline.regular_utc.ms == utc(2026,1,16,1));
    CHECK(s.deadline.effective_utc.ms == utc(2026,1,15,23));
    CHECK(s.deadline.regular_monotonic_ms == 43201000);
    CHECK(s.deadline.effective_monotonic_ms == 36001000);
    CHECK(!s.last_confirmation.utc_valid);
    unchanged(s,sync,first,APP_DUPLICATE);
    group("Erste UTC-Zeit: Nachtregel auf Restdauer, keine erfundene UTC-Gabe, revisionsneutral");

    AppState midnight = boot(base);
    confirm(midnight,base);
    AppEvent short_interval = event(midnight,EVENT_SET_MEDICATION_INTERVAL,base);
    short_interval.value.interval_seconds = 14400;
    OK(app_apply_event(&midnight,&short_interval,&base,nullptr));
    ClockSample after = known(utc(2026,1,14,23,30),1000);
    sync = event(midnight,EVENT_TIME_SYNC,after);
    sync.value.time_sync = after;
    OK(app_apply_event(&midnight,&sync,&after,nullptr));
    CHECK(midnight.deadline.effective_utc.ms == utc(2026,1,15,3,30));
    CHECK(!midnight.deadline.night_advanced && !midnight.alarm_active);
    group("Erste UTC nach Mitternacht erzeugt keine rückwirkende Vorziehung");

    ClockSample now = known(utc(2026,1,15,8));
    AppState correction = boot(now);
    confirm(correction,now);
    const uint64_t original = correction.deadline.effective_monotonic_ms;
    ClockSample backward = later(now,3600000);
    backward.utc_ms = utc(2026,1,15,6);
    sync = event(correction,EVENT_TIME_SYNC,backward);
    sync.value.time_sync = backward;
    OK(app_apply_event(&correction,&sync,&backward,nullptr));
    CHECK(correction.deadline.effective_monotonic_ms == original);
    CHECK(correction.deadline.regular_utc.ms == utc(2026,1,15,17));
    ClockSample forward = later(backward,1000);
    forward.utc_ms = utc(2026,1,15,18);
    AppEvent sync2 = event(correction,EVENT_TIME_SYNC,forward);
    sync2.value.time_sync = forward;
    OK(app_apply_event(&correction,&sync2,&forward,nullptr));
    CHECK(correction.alarm_active && correction.deadline.effective_monotonic_ms <= original);
    ClockSample rewind = later(forward,1000);
    rewind.utc_ms = utc(2026,1,15,1);
    sync2 = event(correction,EVENT_TIME_SYNC,rewind);
    sync2.value.time_sync = rewind;
    OK(app_apply_event(&correction,&sync2,&rewind,nullptr));
    CHECK(correction.alarm_active);
    group("Vorwärts-/Rückwärtskorrekturen: keine monotone Verlängerung, aktiver Alarm bleibt aktiv");

    AppState delayed = boot(now);
    confirm(delayed,now);
    ClockSample measurement = later(now,1000);
    sync = event(delayed,EVENT_TIME_SYNC,measurement);
    sync.value.time_sync = measurement;
    tick(delayed,later(now,1500));
    ClockSample receive = later(now,2000);
    OK(app_apply_event(&delayed,&sync,&receive,nullptr));
    CHECK(delayed.last_time_sync.monotonic_ms == measurement.monotonic_ms);
    CHECK(delayed.clock.utc_ms == utc(2026,1,15,8,0,2));
    AppEvent old = sync;
    for (unsigned i=0;i<20;++i) {
        receive = later(receive,1000);
        sync = event(delayed,EVENT_TIME_SYNC,receive);
        sync.value.time_sync = receive;
        OK(app_apply_event(&delayed,&sync,&receive,nullptr));
    }
    unchanged(delayed,old,receive,APP_STALE);
    AppEvent older_sample = event(delayed,EVENT_TIME_SYNC,receive);
    older_sample.value.time_sync = measurement;
    unchanged(delayed,older_sample,receive,APP_STALE);
    AppEvent foreign = event(delayed,EVENT_TIME_SYNC,receive);
    foreign.value.time_sync = receive; foreign.value.time_sync.domain = 99;
    unchanged(delayed,foreign,receive,APP_TIME_UNAVAILABLE);
    CHECK(delayed.revision.number == 1);
    group("Verspätete Sync-Messung, mehr als 16 Messungen, alte Anker und fremde Monotondomain");
}

void image_tests() {
    ClockSample now = unknown();
    AppState s = boot(now,10,6);
    confirm(s,now);
    tick(s,later(now,4000));
    CHECK(s.image_remaining_ms == 2000 && s.image_id == 0);
    tick(s,later(now,10000));
    CHECK(s.alarm_active && s.image_id == 0 && s.image_due_pending && s.image_remaining_ms == 0);
    tick(s,later(now,1000000));
    CHECK(s.image_id == 0 && s.image_remaining_ms == 0);
    confirm(s,later(now,1000000));
    CHECK(!s.alarm_active && s.image_id == 1 && s.image_remaining_ms == 6000 && !s.image_due_pending);
    tick(s,later(now,1000001));
    CHECK(s.image_id == 1 && s.image_remaining_ms == 5999);
    group("Alarm vor Bildwechsel; lange Alarmphase holt höchstens einen Wechsel nach");

    s = boot(now,10,20);
    confirm(s,now);
    tick(s,later(now,10000));
    CHECK(s.alarm_active && s.image_remaining_ms == 10000);
    tick(s,later(now,50000));
    CHECK(s.image_remaining_ms == 10000);
    confirm(s,later(now,50000));
    CHECK(s.image_remaining_ms == 10000 && s.image_id == 0);
    tick(s,later(now,55000));
    CHECK(s.image_remaining_ms == 5000);
    group("Bildrestzeit pausiert während Alarm und läuft nach Bestätigung weiter");

    s = boot(now,10,10);
    confirm(s,now);
    AppEffects fx = tick(s,later(now,10000));
    CHECK(fx.alarm_started && !fx.image_changed && s.image_id == 0 && s.image_due_pending);
    confirm(s,later(now,20000));
    CHECK(s.image_id == 1);
    group("Exakt gleichzeitige Medikamenten-/Bildfälligkeit priorisiert Alarm");

    s = boot(now,1000,20);
    confirm(s,now);
    ClockSample off = later(now,5000);
    AppEvent mode = event(s,EVENT_SET_ROTATION_MODE,off);
    mode.value.rotation = ROTATION_OFF;
    const uint64_t medicine_due = s.deadline.effective_monotonic_ms;
    OK(app_apply_event(&s,&mode,&off,nullptr));
    CHECK(s.image_remaining_ms == 15000 && s.deadline.effective_monotonic_ms == medicine_due);
    tick(s,later(now,100000));
    CHECK(s.image_remaining_ms == 15000 && s.image_id == 0);
    ClockSample on = later(now,100000);
    mode = event(s,EVENT_SET_ROTATION_MODE,on);
    mode.value.rotation = ROTATION_NAME_ORDER;
    OK(app_apply_event(&s,&mode,&on,nullptr));
    tick(s,later(on,14000));
    CHECK(s.image_id == 0 && s.image_remaining_ms == 1000);
    tick(s,later(on,15000));
    CHECK(s.image_id == 1 && s.image_remaining_ms == 20000);
    AppEvent interval = event(s,EVENT_SET_IMAGE_INTERVAL,s.clock);
    interval.value.interval_seconds = 7;
    OK(app_apply_event(&s,&interval,&s.clock,nullptr));
    CHECK(s.image_remaining_ms == 7000 && s.deadline.effective_monotonic_ms == medicine_due);
    group("ROTATION_OFF pausiert; Moduswechsel erhält Restzeit/Medikament, neues Intervall setzt Bilddauer");

    s = boot(now,1000,5,ROTATION_RANDOM);
    confirm(s,now);
    ClockSample due = later(now,5000);
    tick(s,due);
    CHECK(s.image_id == 0 && s.image_due_pending);
    tick(s,later(due,5000));
    CHECK(s.image_id == 0 && s.image_due_pending);
    AppEvent image = event(s,EVENT_SET_IMAGE,s.clock,false);
    image.value.image_id = 2;
    unchanged(s,image,s.clock,APP_NOT_AUTHORIZED);
    image.coordinator_accepted = true;
    image.value.image_id = 0;
    unchanged(s,image,s.clock,APP_INVALID);
    image.value.image_id = 2;
    OK(app_apply_event(&s,&image,&s.clock,nullptr));
    CHECK(s.image_id == 2 && s.image_remaining_ms == 5000 && !s.image_due_pending);
    group("Zufall nur über autorisiertes EVENT_SET_IMAGE, keine direkte Wiederholung");

    s = boot(now,1000,20);
    confirm(s,now);
    off = later(now,5000);
    mode = event(s,EVENT_SET_ROTATION_MODE,off);
    mode.value.rotation = ROTATION_OFF;
    ClockSample receive = later(now,10000);
    OK(app_apply_event(&s,&mode,&receive,nullptr));
    CHECK(s.image_remaining_ms == 15000);
    group("Verzögerte Moduszustellung pausiert ab ursprünglichem Ereigniszeitpunkt");

    s = boot(now,1000,20); confirm(s,now);
    off = later(now,5000);
    mode = event(s,EVENT_SET_ROTATION_MODE,off); mode.value.rotation = ROTATION_OFF;
    receive = later(now,10000);
    tick(s,receive);
    unchanged(s,mode,receive,APP_STALE);
    CHECK(s.settings.rotation == ROTATION_NAME_ORDER && s.image_remaining_ms == 10000);
    group("Bereits verarbeitete Bildhistorie: alte Modusänderung verlangt Snapshot statt falschem Erfolg");
}

void snapshot_tests() {
    ClockSample origin = known(utc(2026,1,15,8),1000,1);
    AppState source = boot(origin);
    confirm(source,origin);
    ClockSample captured = later(origin,3600000);
    StateSnapshot snap{};
    OK(app_make_snapshot(&source,&captured,&snap));
    CHECK(snap.medication_regular_remaining_ms == 39600000 && snap.image_remaining_ms == 18000000);
    CHECK(source.clock.monotonic_ms == origin.monotonic_ms);
    ClockSample receiver = known(utc(2026,1,15,10),5000,2);
    AppState target = boot(receiver,43200,21600,ROTATION_NAME_ORDER,20);
    AppEvent adoption = event(target,EVENT_ADOPT_SNAPSHOT,receiver);
    adoption.value.snapshot = &snap;
    AppEffects fx{};
    OK(app_apply_event(&target,&adoption,&receiver,&fx));
    CHECK(!target.alarm_active && fx.alarm_ended && fx.persistence_required);
    CHECK(target.deadline.regular_monotonic_ms == 36005000);
    CHECK(target.deadline.regular_utc.ms == utc(2026,1,15,20));
    CHECK(target.image_remaining_ms == 14400000 && target.last_confirmation.utc_ms == origin.utc_ms);
    CHECK(target.countdown_started.domain == 1 && target.clock.domain == 2);
    unchanged(target,adoption,receiver,APP_DUPLICATE);
    group("Laufende Gruppe auf neuer Domain: UTC-Alter, bestehender Countdown und Bildrestzeit übernommen");

    StateSnapshot old = snap;
    old.state.revision.number = 0;
    old.state.settings.revision.number = 0;
    adoption = event(target,EVENT_ADOPT_SNAPSHOT,receiver);
    adoption.value.snapshot = &old;
    unchanged(target,adoption,receiver,APP_STALE);
    StateSnapshot conflict = snap;
    conflict.state.revision.coordinator = 99;
    adoption.value.snapshot = &conflict;
    unchanged(target,adoption,receiver,APP_CONFLICT);
    StateSnapshot corrupt = snap;
    corrupt.medication_regular_remaining_ms += 1;
    adoption.value.snapshot = &corrupt;
    unchanged(target,adoption,receiver,APP_INVALID);
    corrupt = snap; corrupt.state.settings.rotation = static_cast<RotationMode>(99);
    unchanged(target,adoption,receiver,APP_INVALID);
    corrupt = snap; corrupt.schema_version = 44;
    unchanged(target,adoption,receiver,APP_INVALID);
    corrupt = snap; corrupt.captured_at.domain = 88;
    unchanged(target,adoption,receiver,APP_INVALID);
    corrupt = snap; corrupt.state.image_id = 3;
    unchanged(target,adoption,receiver,APP_INVALID);
    corrupt = snap; corrupt.state.round.origin.sequence = 0;
    unchanged(target,adoption,receiver,APP_INVALID);
    corrupt = snap; corrupt.state.recent_event_count = 17;
    unchanged(target,adoption,receiver,APP_INVALID);
    group("Snapshots: alte Revision, gleicher Zähler/fremder Koordinator, kaputte Dauer/Version/Enums/Identitäten");

    source = boot(unknown(1000,77));
    confirm(source,source.clock);
    ClockSample local_capture = unknown(2000,77);
    OK(app_make_snapshot(&source,&local_capture,&snap));
    receiver = unknown(4000,77);
    target = boot(receiver,43200,21600,ROTATION_NAME_ORDER,20);
    adoption = event(target,EVENT_ADOPT_SNAPSHOT,receiver); adoption.value.snapshot = &snap;
    OK(app_apply_event(&target,&adoption,&receiver,nullptr));
    CHECK(target.deadline.regular_monotonic_ms == 43201000 && !target.deadline.regular_utc.valid);
    AppState no_relation = boot(unknown(4000,88),43200,21600,ROTATION_NAME_ORDER,20);
    adoption = event(no_relation,EVENT_ADOPT_SNAPSHOT,no_relation.clock); adoption.value.snapshot = &snap;
    unchanged(no_relation,adoption,no_relation.clock,APP_TIME_UNAVAILABLE);
    group("Snapshots ohne UTC: nur nachgewiesene gleiche Monotondomain erlaubt");

    origin = known(utc(2026,1,15,8),1000,1);
    source = boot(origin); confirm(source,origin);
    captured = later(origin,3600000);
    OK(app_make_snapshot(&source,&captured,&snap));
    target = source;
    receiver = later(origin,7200000);
    tick(target,receiver);
    const uint64_t deadline = target.deadline.effective_monotonic_ms;
    adoption = event(target,EVENT_ADOPT_SNAPSHOT,receiver); adoption.value.snapshot = &snap;
    OK(app_apply_event(&target,&adoption,&receiver,&fx));
    CHECK(target.deadline.effective_monotonic_ms == deadline && target.revision.number == 1);
    CHECK(target.image_remaining_ms == 14400000);
    CHECK(!fx.persistence_required);
    group("Gleiche Snapshotrevision schreibt nur belegbaren Fortschritt fort und startet Countdown nicht neu");

    ClockSample medicine_due = later(origin,43200000);
    tick(target,medicine_due);
    confirm(target,medicine_due);
    StateSnapshot lying_newer = snap;
    lying_newer.state.revision.number = target.revision.number + 10;
    adoption = event(target,EVENT_ADOPT_SNAPSHOT,medicine_due);
    adoption.value.snapshot = &lying_newer;
    unchanged(target,adoption,medicine_due,APP_STALE);
    group("Ältere letzte Gabe wird auch durch höhere Snapshotrevision nicht zurückgerollt");

    source = boot(origin); confirm(source,origin);
    tick(source,later(origin,43200000));
    OK(app_make_snapshot(&source,&source.clock,&snap));
    target = boot(known(utc(2026,1,15,20),5000,99),43200,21600,ROTATION_NAME_ORDER,20);
    adoption = event(target,EVENT_ADOPT_SNAPSHOT,target.clock); adoption.value.snapshot = &snap;
    OK(app_apply_event(&target,&adoption,&target.clock,nullptr));
    CHECK(target.alarm_active);
    AppEvent bad_clear = event(target,EVENT_ADOPT_SNAPSHOT,target.clock);
    StateSnapshot false_idle = snap;
    false_idle.state.revision.number += 1;
    false_idle.state.alarm_active = false;
    bad_clear.value.snapshot = &false_idle;
    unchanged(target,bad_clear,target.clock,APP_INVALID);
    group("Aktiver Gruppenalarm bleibt beim Abgleich verriegelt");

    source = boot(origin); confirm(source,origin);
    AppEvent sync = event(source,EVENT_TIME_SYNC,origin); sync.value.time_sync = origin;
    OK(app_apply_event(&source,&sync,&origin,nullptr));
    OK(app_make_snapshot(&source,&origin,&snap));
    receiver = known(utc(2026,1,15,8,0,1),2000,2);
    target = boot(receiver,43200,21600,ROTATION_NAME_ORDER,20);
    adoption = event(target,EVENT_ADOPT_SNAPSHOT,receiver); adoption.value.snapshot = &snap;
    OK(app_apply_event(&target,&adoption,&receiver,nullptr));
    CHECK(!target.has_time_sync && target.last_time_sync.domain == 0);
    group("Snapshotadoption übernimmt keinen fremden lokalen Sync-Deduplizierungsanker");

    origin = known(utc(2026,1,15,8),1000,1);
    source = boot(origin,10,20); confirm(source,origin);
    target = source;
    ClockSample alarm = later(origin,10000);
    tick(source,alarm); tick(target,alarm);
    ClockSample accepted = later(alarm,1000);
    confirm(source,accepted);
    OK(app_make_snapshot(&source,&accepted,&snap));
    adoption = event(target,EVENT_ADOPT_SNAPSHOT,accepted); adoption.value.snapshot = &snap;
    StateSnapshot branch = snap;
    branch.state.last_confirmed_round.origin.epoch += 1;
    adoption.value.snapshot = &branch;
    unchanged(target,adoption,accepted,APP_CONFLICT);
    adoption.value.snapshot = &snap;
    OK(app_apply_event(&target,&adoption,&accepted,&fx));
    CHECK(!target.alarm_active && fx.alarm_ended);
    CHECK(target.last_confirmation.utc_ms == accepted.utc_ms);
    CHECK(target.last_confirmed_round.origin.sequence == source.last_confirmed_round.origin.sequence);
    group("Neue Gruppenbestätigung löscht exakt ihre Elternrunde; gleicher Ordinalwert aus anderem Zweig reicht nicht");

    source = boot(origin,1000,2); confirm(source,origin);
    OK(app_make_snapshot(&source,&origin,&snap));
    target = source;
    tick(target,later(origin,2000));
    tick(target,later(origin,4000));
    CHECK(target.image_id == 2);
    receiver = later(origin,4500);
    adoption = event(target,EVENT_ADOPT_SNAPSHOT,receiver); adoption.value.snapshot = &snap;
    OK(app_apply_event(&target,&adoption,&receiver,nullptr));
    CHECK(target.image_id == 2 && target.image_remaining_ms == 1500);
    StateSnapshot modified_history = snap;
    modified_history.state.last_confirmation.utc_ms -= 1;
    adoption = event(target,EVENT_ADOPT_SNAPSHOT,receiver); adoption.value.snapshot = &modified_history;
    unchanged(target,adoption,receiver,APP_INVALID);
    group("Alte gleiche Revision rollt weder fortgeschrittenes Bild noch Zeitstempel derselben Gabe zurück");

    source = boot(origin); confirm(source,origin);
    OK(app_make_snapshot(&source,&origin,&snap));
    snap.catalog_id[0] ^= 1;
    snap.state.catalog_id[0] ^= 1;
    receiver = known(utc(2026,1,15,9),2000,2);
    target = boot(receiver,43200,21600,ROTATION_NAME_ORDER,20);
    adoption = event(target,EVENT_ADOPT_SNAPSHOT,receiver); adoption.value.snapshot = &snap;
    OK(app_apply_event(&target,&adoption,&receiver,nullptr));
    CHECK(target.catalog_mismatch && !target.alarm_active && target.image_id == 0);
    CHECK(target.catalog_id[0] == catalog[0]);
    group("Fremder Bildkatalog erzeugt Statusfehler und beeinträchtigt die Medikamentenübernahme nicht");

    origin = known(utc(2026,1,15,8));
    target = boot(origin); confirm(target,origin);
    source = target;
    ClockSample corrected = known(utc(2026,1,15,10),3601000);
    sync = event(target,EVENT_TIME_SYNC,corrected); sync.value.time_sync = corrected;
    OK(app_apply_event(&target,&sync,&corrected,nullptr));
    corrected = known(utc(2026,1,15,9,0,1),3602000);
    sync = event(target,EVENT_TIME_SYNC,corrected); sync.value.time_sync = corrected;
    OK(app_apply_event(&target,&sync,&corrected,nullptr));
    CHECK(target.deadline.effective_monotonic_ms == 39601000);
    const EventId countdown = target.countdown_origin;
    AppEvent images = event(source,EVENT_SET_IMAGE_INTERVAL,corrected); images.value.interval_seconds = 100;
    OK(app_apply_event(&source,&images,&corrected,nullptr));
    OK(app_make_snapshot(&source,&corrected,&snap));
    adoption = event(target,EVENT_ADOPT_SNAPSHOT,corrected); adoption.value.snapshot = &snap;
    OK(app_apply_event(&target,&adoption,&corrected,nullptr));
    CHECK(target.deadline.effective_monotonic_ms == 39601000);
    CHECK(target.countdown_origin.sequence == countdown.sequence);
    CHECK(target.settings.image_seconds == 100 && target.revision.number == 2);
    // A real, coordinator-accepted medication restart is allowed to choose a
    // new future deadline and is distinguished by its stable origin identity.
    AppEvent restart = event(source,EVENT_SET_MEDICATION_INTERVAL,corrected);
    restart.value.interval_seconds = 43200;
    OK(app_apply_event(&source,&restart,&corrected,nullptr));
    OK(app_make_snapshot(&source,&corrected,&snap));
    adoption = event(target,EVENT_ADOPT_SNAPSHOT,corrected); adoption.value.snapshot = &snap;
    OK(app_apply_event(&target,&adoption,&corrected,nullptr));
    CHECK(target.deadline.regular_monotonic_ms == 46802000);
    CHECK(target.countdown_origin.sequence == restart.id.sequence);
    group("Höhere Bildrevision verlängert korrigierte Deadline nicht; nur echter neuer Countdownanker darf neu starten");

    source = boot(origin); confirm(source,origin);
    OK(app_make_snapshot(&source,&origin,&snap));
    StateSnapshot inflated = snap;
    inflated.state.deadline.regular_monotonic_ms += 3600000;
    inflated.state.deadline.effective_monotonic_ms += 3600000;
    inflated.state.deadline.regular_utc.ms += 3600000;
    inflated.state.deadline.effective_utc.ms += 3600000;
    inflated.medication_regular_remaining_ms += 3600000;
    inflated.medication_effective_remaining_ms += 3600000;
    target = boot(origin,43200,21600,ROTATION_NAME_ORDER,20);
    adoption = event(target,EVENT_ADOPT_SNAPSHOT,origin); adoption.value.snapshot = &inflated;
    unchanged(target,adoption,origin,APP_INVALID);
    inflated.state.start_reason = START_SNAPSHOT;
    unchanged(target,adoption,origin,APP_INVALID);
    group("Semantische Snapshotprüfung: konsistent manipulierte 13 statt 12 Stunden und verlorener Startgrund abgewiesen");
}

void delayed_event_tests() {
    ClockSample origin = unknown(1000);
    AppState s = boot(origin,10,100);
    AppEvent first = event(s,EVENT_CONFIRM,later(origin,1000));
    ClockSample received = later(origin,100000);
    AppEffects fx{};
    OK(app_apply_event(&s,&first,&received,&fx));
    CHECK(s.alarm_active && fx.alarm_ended && fx.alarm_started);
    CHECK(s.last_confirmation.monotonic_ms == 2000);
    CHECK(s.deadline.effective_monotonic_ms == 12000);
    CHECK(s.image_remaining_ms == 90000);
    unchanged(s,first,received,APP_DUPLICATE);
    AppEvent too_early = event(s,EVENT_CONFIRM,later(origin,5000));
    unchanged(s,too_early,received,APP_STALE);
    AppEvent actual = event(s,EVENT_CONFIRM,later(origin,15000));
    OK(app_apply_event(&s,&actual,&received,&fx));
    CHECK(s.alarm_active && s.last_confirmation.monotonic_ms == 16000);
    CHECK(s.deadline.effective_monotonic_ms == 26000 && s.image_remaining_ms == 80000);
    group("Verspätete Offlinebestätigung nutzt Originalzeit; bereits nächste Runde bleibt Alarm, Bildlauf nur bis Fälligkeit");

    ClockSample real = known(utc(2026,1,15,8),1000,1);
    s = boot(real,10,100); confirm(s,real);
    ClockSample due = later(real,10000); tick(s,due);
    AppEvent remote = event(s,EVENT_CONFIRM,known(utc(2026,1,15,8,0,11),9000,77));
    received = later(real,12000);
    OK(app_apply_event(&s,&remote,&received,nullptr));
    CHECK(s.last_confirmation.domain == 77 && s.last_confirmation.monotonic_ms == 9000);
    CHECK(s.deadline.regular_monotonic_ms == 22000 && !s.alarm_active);
    CHECK(s.image_remaining_ms == 89000);
    group("Fremde Ereignisdomain wird nur über belegbare UTC umgerechnet; Originalhistorie bleibt erhalten");

    origin = unknown(1000);
    s = boot(origin);
    AppEvent no_utc_confirmation = event(s,EVENT_CONFIRM,origin);
    received = known(utc(2026,1,15,23,30),36001000);
    OK(app_apply_event(&s,&no_utc_confirmation,&received,nullptr));
    CHECK(!s.alarm_active && !s.deadline.night_advanced);
    CHECK(s.deadline.effective_monotonic_ms == 43201000);
    CHECK(s.deadline.effective_utc.ms == utc(2026,1,16,1,30));
    CHECK(!s.last_confirmation.utc_valid);
    // If UTC was already known when confirming, the midnight deadline really
    // existed then and is correctly overdue at delivery.
    real = known(utc(2026,1,15,13,30),1000);
    s = boot(real);
    AppEvent with_utc_confirmation = event(s,EVENT_CONFIRM,real);
    OK(app_apply_event(&s,&with_utc_confirmation,&received,nullptr));
    CHECK(s.alarm_active && s.deadline.night_advanced);
    CHECK(s.deadline.effective_utc.ms == utc(2026,1,15,23));
    group("Verspätete Gabe mit erster UTC: keine rückwirkende Nachtvorziehung; damals belegte UTC-Fälligkeit bleibt gültig");
}

void bounds_tests() {
    ClockSample now = unknown(49ULL*86400000 + 1000);
    AppState s = boot(now,43200,21600);
    confirm(s,now);
    CHECK(s.deadline.regular_monotonic_ms == 49ULL*86400000 + 43201000);
    tick(s,later(now,43200000));
    CHECK(s.alarm_active);
    Deadline d{};
    now = unknown(1000);
    OK(app_calculate_deadline(&now,UINT32_MAX,&d));
    CHECK(d.regular_monotonic_ms == 4294967296000ULL);
    CHECK(!d.regular_utc.valid);
    ClockSample huge = unknown(UINT64_MAX-999);
    const Deadline previous = d;
    CHECK(app_calculate_deadline(&huge,1,&d) == APP_OVERFLOW);
    CHECK(std::memcmp(&previous,&d,sizeof(d)) == 0);
    CHECK(app_calculate_deadline(&now,0,&d) == APP_INVALID);
    now = known(utc(2099,12,31,22));
    CHECK(app_calculate_deadline(&now,7200,&d) == APP_OVERFLOW);
    group("Mehr als 49 Tage, UINT32_MAX Sekunden, uint64-Überlauf und Kalenderrand atomar");

    now = unknown(); s = boot(now);
    AppEvent c = event(s,EVENT_CONFIRM,now);
    c.id.sequence = 0; unchanged(s,c,now,APP_INVALID);
    c = event(s,EVENT_CONFIRM,now); c.sender = 99; unchanged(s,c,now,APP_INVALID);
    c = event(s,EVENT_CONFIRM,now); c.occurred_at.domain = 0; unchanged(s,c,now,APP_INVALID);
    c = event(s,EVENT_CONFIRM,now); c.occurred_at.quality = static_cast<ClockQuality>(9);
    unchanged(s,c,now,APP_INVALID);
    c = event(s,static_cast<AppEventKind>(99),now); unchanged(s,c,now,APP_INVALID);
    c = event(s,EVENT_CONFIRM,later(now,1000)); unchanged(s,c,now,APP_STALE);
    c = event(s,EVENT_CONFIRM,now);
    AppState corrupt = s; corrupt.recent_event_next = 16; unchanged(corrupt,c,now,APP_INVALID);
    corrupt = s; corrupt.deadline.effective_monotonic_ms = 1; unchanged(corrupt,c,now,APP_INVALID);
    corrupt = s; corrupt.image_remaining_ms = UINT64_MAX; unchanged(corrupt,c,now,APP_INVALID);
    corrupt = s; corrupt.revision.number = UINT64_MAX;
    c = event(corrupt,EVENT_CONFIRM,now); unchanged(corrupt,c,now,APP_OVERFLOW);
    corrupt = s;
    confirm(corrupt,now);
    ClockSample overflow_due = later(now,43200000);
    tick(corrupt,overflow_due);
    corrupt.round.ordinal = UINT64_MAX;
    corrupt.last_confirmed_round.ordinal = UINT64_MAX - 1;
    c = event(corrupt,EVENT_CONFIRM,overflow_due);
    unchanged(corrupt,c,overflow_due,APP_OVERFLOW);
    AppEffects fx{};
    const AppState saved = s;
    ClockSample foreign = unknown(1000,2);
    CHECK(app_tick(&s,&foreign,&fx) == APP_INVALID);
    CHECK(std::memcmp(&saved,&s,sizeof(s)) == 0);
    ClockSample earlier = unknown(999);
    CHECK(app_tick(&s,&earlier,&fx) == APP_STALE);
    CHECK(std::memcmp(&saved,&s,sizeof(s)) == 0);
    group("Ungültige Enums/Identitäten/Snapshots/Zeitdomänen und Revision-/Rundenüberlauf atomar");
}
} // namespace

int main() {
    night_table();
    timezone_tests();
    preview_tests();
    parse_tests();
    lifecycle_tests();
    sync_tests();
    image_tests();
    snapshot_tests();
    delayed_event_tests();
    bounds_tests();
    std::printf("Core: %u Gruppen, %u explizite Assertions erfolgreich.\n",groups,assertions);
    return 0;
}
