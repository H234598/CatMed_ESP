#include "katmed_core.h"

#include <cassert>
#include <cstdio>

static constexpr uint8_t catalog[KATMED_CATALOG_BYTES]={0x42,0x13};
static constexpr int64_t utc_ms=INT64_C(1760460000000);

static AppState boot(uint64_t device,uint64_t domain,int64_t utc) {
    SharedSettings settings=app_default_settings();
    ClockSample now{1000,domain,utc,utc!=0,utc!=0?CLOCK_SYNCHRONIZED:CLOCK_UNAVAILABLE};
    AppState state{};
    assert(app_init(&state,&settings,&now,{device,5,1},75,catalog)==APP_OK);
    return state;
}
static AppEvent adoption(const AppState &receiver,const StateSnapshot &snapshot,
                         ClockSample now,uint64_t sequence) {
    AppEvent event{};
    event.kind=EVENT_ADOPT_SNAPSHOT;
    event.id={snapshot.state.round.origin.device,5,sequence};
    event.sender=event.id.device;
    event.round=receiver.round;
    event.parent_revision=receiver.revision;
    event.occurred_at=now;
    event.coordinator_accepted=true; /* Authenticated Mesh session. */
    event.value.snapshot=&snapshot;
    return event;
}
int main() {
    AppState root=boot(99,99,utc_ms);
    AppState leaf=boot(11,7,utc_ms);
    ClockSample captured{1000,99,utc_ms,true,CLOCK_SYNCHRONIZED};
    StateSnapshot snapshot{};
    assert(app_make_snapshot(&root,&captured,&snapshot)==APP_OK);
    ClockSample received{2000,7,utc_ms+1000,true,CLOCK_SYNCHRONIZED};
    AppEvent event=adoption(leaf,snapshot,received,2);
    AppEffects effects{};
    assert(app_apply_event(&leaf,&event,&received,&effects)==APP_OK);
    assert(leaf.alarm_active && leaf.start_reason==START_BOOT &&
           leaf.round.origin.device==99 && leaf.revision.coordinator==99 &&
           leaf.deadline.regular_monotonic_ms==0 &&
           leaf.deadline.effective_monotonic_ms==0 &&
           !leaf.has_last_confirmation && !effects.alarm_ended);

    /* Periodic coordinator state for the already adopted Boot round must
     * not reopen this device's locally closed notice. */
    ClockSample local_close_after_adopt{3000,7,utc_ms+2000,true,CLOCK_SYNCHRONIZED};
    assert(app_dismiss_boot_alarm(&leaf,&local_close_after_adopt,&effects)==APP_OK);
    assert(!effects.persistence_required && !effects.broadcast_required);
    ClockSample later_capture{3000,99,utc_ms+2000,true,CLOCK_SYNCHRONIZED};
    assert(app_make_snapshot(&root,&later_capture,&snapshot)==APP_OK);
    ClockSample later_received{4000,7,utc_ms+3000,true,CLOCK_SYNCHRONIZED};
    event=adoption(leaf,snapshot,later_received,3);
    assert(app_apply_event(&leaf,&event,&later_received,&effects)==APP_OK);
    assert(!leaf.alarm_active && leaf.start_reason==START_BOOT &&
           leaf.round.origin.device==99 && !leaf.has_last_confirmation &&
           leaf.deadline.effective_monotonic_ms==0 &&
           !effects.alarm_started && !effects.persistence_required &&
           !effects.broadcast_required);

    /* A later genuinely due coordinator round still replaces the notice
     * and keeps its ordinary confirmation path. */
    AppEvent interval{};
    interval.kind=EVENT_SET_MEDICATION_INTERVAL;
    interval.id={99,5,4};interval.sender=99;
    interval.round=root.round;interval.parent_revision=root.revision;
    interval.occurred_at=later_capture;interval.coordinator_accepted=true;
    interval.value.interval_seconds=3600;
    assert(app_dismiss_boot_alarm(&root,&later_capture,&effects)==APP_OK);
    assert(app_apply_event(&root,&interval,&later_capture,&effects)==APP_OK);
    ClockSample due_capture{3603000,99,utc_ms+3602000,true,CLOCK_SYNCHRONIZED};
    assert(app_make_snapshot(&root,&due_capture,&snapshot)==APP_OK);
    ClockSample due_received{3604000,7,utc_ms+3603000,true,CLOCK_SYNCHRONIZED};
    event=adoption(leaf,snapshot,due_received,5);
    assert(app_apply_event(&leaf,&event,&due_received,&effects)==APP_OK);
    assert(leaf.alarm_active && leaf.start_reason==START_SETTINGS &&
           leaf.deadline.effective_utc.valid && !leaf.has_last_confirmation);
    AppEvent real_confirm{};
    real_confirm.kind=EVENT_CONFIRM;real_confirm.id={11,5,10};
    real_confirm.sender=11;real_confirm.round=leaf.round;
    real_confirm.parent_revision=leaf.revision;
    real_confirm.occurred_at=due_received;real_confirm.coordinator_accepted=true;
    assert(app_apply_event(&leaf,&real_confirm,&due_received,&effects)==APP_OK);
    assert(!leaf.alarm_active && leaf.has_last_confirmation &&
           leaf.last_confirmation_id.sequence==10 && effects.persistence_required);

    root=boot(99,99,utc_ms);
    assert(app_make_snapshot(&root,&captured,&snapshot)==APP_OK);

    leaf=boot(11,7,utc_ms);
    assert(app_dismiss_boot_alarm(&leaf,&captured,&effects)==APP_INVALID);
    ClockSample local_close{1000,7,utc_ms,true,CLOCK_SYNCHRONIZED};
    assert(app_dismiss_boot_alarm(&leaf,&local_close,&effects)==APP_OK);
    assert(!effects.persistence_required && !effects.broadcast_required);
    event=adoption(leaf,snapshot,received,6);
    assert(app_apply_event(&leaf,&event,&received,&effects)==APP_OK);
    assert(!leaf.alarm_active && leaf.start_reason==START_BOOT &&
           leaf.deadline.effective_monotonic_ms==0 && !leaf.has_last_confirmation);

    root=boot(99,99,utc_ms);
    ClockSample root_close{1000,99,utc_ms,true,CLOCK_SYNCHRONIZED};
    assert(app_dismiss_boot_alarm(&root,&root_close,&effects)==APP_OK);
    assert(app_make_snapshot(&root,&root_close,&snapshot)==APP_OK);
    leaf=boot(11,7,utc_ms);
    event=adoption(leaf,snapshot,received,7);
    assert(app_apply_event(&leaf,&event,&received,&effects)==APP_OK);
    assert(leaf.alarm_active && leaf.start_reason==START_BOOT &&
           !leaf.has_last_confirmation);

    /* A Boot snapshot across unrelated clocks needs UTC. */
    root=boot(99,99,0);leaf=boot(11,7,0);
    captured={1000,99,0,false,CLOCK_UNAVAILABLE};
    received={2000,7,0,false,CLOCK_UNAVAILABLE};
    assert(app_make_snapshot(&root,&captured,&snapshot)==APP_OK);
    event=adoption(leaf,snapshot,received,3);
    assert(app_apply_event(&leaf,&event,&received,&effects)==APP_TIME_UNAVAILABLE);
    assert(leaf.round.origin.device==11 && leaf.alarm_active);

    /* The same round may carry a newer accepted setting while alarm stays. */
    root=boot(99,99,utc_ms);leaf=root;
    captured={1000,99,utc_ms,true,CLOCK_SYNCHRONIZED};
    AppEvent setting{};
    setting.kind=EVENT_SET_ROTATION_MODE;setting.id={99,5,4};setting.sender=99;
    setting.round=root.round;setting.parent_revision=root.revision;
    setting.occurred_at=captured;setting.coordinator_accepted=true;
    setting.value.rotation=ROTATION_OFF;
    assert(app_apply_event(&root,&setting,&captured,&effects)==APP_OK);
    assert(app_make_snapshot(&root,&captured,&snapshot)==APP_OK);
    event=adoption(leaf,snapshot,captured,5);
    assert(app_apply_event(&leaf,&event,&captured,&effects)==APP_OK);
    assert(leaf.alarm_active && leaf.start_reason==START_BOOT &&
           leaf.settings.rotation==ROTATION_OFF &&
           leaf.deadline.regular_monotonic_ms==0 && !leaf.has_last_confirmation);
    std::puts("distinct Boot snapshot and zero-deadline adoption passed");
}
