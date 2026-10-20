#include <assert.h>
#include <stdio.h>

#define main established_main_store_repro
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type" /* Renamed C main loses implicit return 0. */
#include "../host_store/check_main_submit.c"
#pragma GCC diagnostic pop
#undef main

int main(void)
{
    fake_microseconds=1000000;
    committed_length=0;
    writes=0;
    fail_commit=false;
    initialize_runtime();
    ClockSample now=clock_sample(runtime.boot_domain);
    KatmedRuntimeEvidenceState sample=evidence_state(&runtime,&now,1);
    assert(sample.boot_domain==runtime.boot_domain &&
           sample.round_device==runtime.state.round.origin.device &&
           sample.start_domain==runtime.state.countdown_started.domain &&
           sample.deadline_local==1 && sample.utc_source==0 &&
           sample.image_id==runtime.state.image_id);
    fake_microseconds=2000000;
    assert(submit_due_confirm()==UI_SUBMIT_PENDING);
    now=clock_sample(runtime.boot_domain);
    sample=evidence_state(&runtime,&now,2);
    assert(sample.alarm==0 && sample.origin_sequence==runtime.state.countdown_origin.sequence &&
           sample.effective_ms==runtime.state.deadline.effective_monotonic_ms);
    katmed_time_init(&runtime.time,runtime.boot_domain);
    fake_microseconds=4000000;
    mesh_utc_received(&runtime,3000,INT64_C(1791973800000),CLOCK_HOLDOVER);
    assert(runtime.utc_source==2);
    now=clock_sample(runtime.boot_domain);
    sample=evidence_state(&runtime,&now,3);
    assert(sample.utc_source==2 && sample.now_utc_valid &&
           sample.now_quality==CLOCK_HOLDOVER);
    Runtime foreign=runtime;
    foreign.state.countdown_started.domain=UINT64_C(99);
    foreign.state.clock.domain=UINT64_C(99);
    sample=evidence_state(&foreign,&now,4);
    assert(sample.start_domain==99 && sample.clock_domain==99 &&
           sample.deadline_local==0 && sample.boot_domain!=99);
    puts("main serialized state and UTC-source mapping passed");
    return 0;
}
