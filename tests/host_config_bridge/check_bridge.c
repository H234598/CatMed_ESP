#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/task.h"
#include "katmed_config_bridge.h"

typedef struct {
    KatmedConfigRequest request;
    KatmedConfigResponse response;
    uint32_t timeout_ms;
    KatmedConfigStatus result;
    KatmedConfigBridgeDiagnostic diagnostic;
    KatmedConfigBridgeTrace trace;
} Client;
static unsigned executions;
TickType_t katmed_test_tick_offset;
static pthread_mutex_t wait_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_changed=PTHREAD_COND_INITIALIZER;
static bool wait_armed, wait_observed;

/* The existing semaphore stub calls this only immediately before a real
 * blocking take. With the gate already free, this is the done wait after
 * the request entered SLOT_QUEUED; no timing guess or dummy owner sleep. */
void katmed_test_before_sem_wait(void)
{
    pthread_mutex_lock(&wait_mutex);
    if (wait_armed) {
        wait_observed=true;
        pthread_cond_signal(&wait_changed);
    }
    pthread_mutex_unlock(&wait_mutex);
}
static void arm_done_wait(void)
{
    pthread_mutex_lock(&wait_mutex);
    wait_armed=true;
    wait_observed=false;
    pthread_mutex_unlock(&wait_mutex);
}
static void await_done_wait(void)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME,&deadline)==0);
    deadline.tv_sec+=2;
    pthread_mutex_lock(&wait_mutex);
    while (!wait_observed)
        assert(pthread_cond_timedwait(&wait_changed,&wait_mutex,&deadline)==0);
    wait_armed=false;
    pthread_mutex_unlock(&wait_mutex);
}
static void *foreign_mark(void *unused)
{
    (void)unused;
    katmed_config_render_inner(KATMED_RENDER_INNER_JPEG_DECODE,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_JPEG_DECODE,false,0);
    return NULL;
}

static void pause_ms(long ms)
{
    const struct timespec time={.tv_sec=ms/1000,.tv_nsec=(ms%1000)*1000000L};
    nanosleep(&time,NULL);
}
static void *call(void *context)
{
    Client *client=context;
    client->result=katmed_config_exchange_diagnostic(&client->request,&client->response,
                                                    client->timeout_ms,&client->diagnostic,
                                                    &client->trace);
    return NULL;
}
static void fast(void *context, const KatmedConfigRequest *request,
                 KatmedConfigResponse *response)
{
    (void)context;
    assert(request->operation==KATMED_CONFIG_GET);
    ++executions;
    response->status=KATMED_CONFIG_OK;
    strcpy(response->snapshot.medication_hours,"12");
}
static void slow(void *context, const KatmedConfigRequest *request,
                 KatmedConfigResponse *response)
{
    pause_ms(80);
    fast(context,request,response);
}
static void unavailable(void *context, const KatmedConfigRequest *request,
                        KatmedConfigResponse *response)
{
    (void)context;
    assert(request->operation==KATMED_CONFIG_GET);
    ++executions;
    response->status=KATMED_CONFIG_UNAVAILABLE;
}
int main(void)
{
    assert(katmed_config_bridge_start());
    assert(!katmed_config_bridge_start());
    KatmedConfigBridgeDiagnostic precondition=KATMED_CONFIG_BRIDGE_DIAG_RESPONSE;
    KatmedConfigRequest invalid={.operation=KATMED_CONFIG_GET};
    KatmedConfigResponse ignored={0};
    KatmedConfigBridgeTrace invalid_trace={0};
    assert(katmed_config_exchange_diagnostic(&invalid,&ignored,0,&precondition,&invalid_trace)==
           KATMED_CONFIG_UNAVAILABLE);
    assert(precondition==KATMED_CONFIG_BRIDGE_DIAG_PRECONDITION);
    assert(invalid_trace.valid==0);
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_PRE_POLL);
    Client first={.request.operation=KATMED_CONFIG_GET,.timeout_ms=1000};
    pthread_t thread;
    assert(pthread_create(&thread,NULL,call,&first)==0);
    pause_ms(10);
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_POLL);
    katmed_config_poll(fast,NULL);
    pthread_join(thread,NULL);
    assert(first.result==KATMED_CONFIG_OK &&
           first.diagnostic==KATMED_CONFIG_BRIDGE_DIAG_RESPONSE &&
           strcmp(first.response.snapshot.medication_hours,"12")==0 && executions==1);
    assert((first.trace.valid & (KATMED_CONFIG_TRACE_GATE_START |
            KATMED_CONFIG_TRACE_GATE_RETURN | KATMED_CONFIG_TRACE_ENQUEUE |
            KATMED_CONFIG_TRACE_POLL_ENTER | KATMED_CONFIG_TRACE_POLL_COMPLETE |
            KATMED_CONFIG_TRACE_WAIT_RETURN | KATMED_CONFIG_TRACE_DECISION)) ==
           (KATMED_CONFIG_TRACE_GATE_START | KATMED_CONFIG_TRACE_GATE_RETURN |
            KATMED_CONFIG_TRACE_ENQUEUE | KATMED_CONFIG_TRACE_POLL_ENTER |
            KATMED_CONFIG_TRACE_POLL_COMPLETE | KATMED_CONFIG_TRACE_WAIT_RETURN |
            KATMED_CONFIG_TRACE_DECISION));
    assert(first.trace.gate_acquired && first.trace.wait_signalled &&
           first.trace.slot_at_decision==KATMED_CONFIG_TRACE_SLOT_DONE);
    assert(first.trace.tick_period_ms==1 &&
           first.trace.owner_phase_at_enqueue==KATMED_CONFIG_OWNER_PHASE_PRE_POLL &&
           (first.trace.owner_phase_seen_mask &
            (1u << KATMED_CONFIG_OWNER_PHASE_POLL)));

    Client backend_unavailable={.request.operation=KATMED_CONFIG_GET,.timeout_ms=1000};
    assert(pthread_create(&thread,NULL,call,&backend_unavailable)==0);
    pause_ms(10);
    katmed_config_poll(unavailable,NULL);
    pthread_join(thread,NULL);
    assert(backend_unavailable.result==KATMED_CONFIG_UNAVAILABLE &&
           backend_unavailable.response.status==KATMED_CONFIG_UNAVAILABLE &&
           backend_unavailable.diagnostic==KATMED_CONFIG_BRIDGE_DIAG_RESPONSE &&
           executions==2);

    /* A request that times out before the loop takes it is cancelled. */
    Client queued={.request.operation=KATMED_CONFIG_GET,.timeout_ms=100};
    assert(pthread_create(&thread,NULL,call,&queued)==0);
    pause_ms(10);
    Client admission={.request.operation=KATMED_CONFIG_GET,.timeout_ms=10};
    assert(katmed_config_exchange_diagnostic(&admission.request,&admission.response,
        admission.timeout_ms,&admission.diagnostic,&admission.trace)==KATMED_CONFIG_UNAVAILABLE);
    assert(admission.diagnostic==KATMED_CONFIG_BRIDGE_DIAG_ADMISSION_TIMEOUT);
    assert((admission.trace.valid & KATMED_CONFIG_TRACE_GATE_RETURN) &&
           !(admission.trace.valid & KATMED_CONFIG_TRACE_ENQUEUE) &&
           !admission.trace.gate_acquired &&
           admission.trace.tick_period_ms==1);
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_RENDER_CALL);
    pthread_join(thread,NULL);
    assert(queued.result==KATMED_CONFIG_UNAVAILABLE &&
           queued.diagnostic==KATMED_CONFIG_BRIDGE_DIAG_QUEUED_CANCELLED);
    assert((queued.trace.valid & KATMED_CONFIG_TRACE_WAIT_RETURN) &&
           !(queued.trace.valid & KATMED_CONFIG_TRACE_POLL_ENTER) &&
           queued.trace.slot_at_decision==KATMED_CONFIG_TRACE_SLOT_QUEUED &&
           !queued.trace.wait_signalled &&
           queued.trace.wait_elapsed_ms>0 &&
           queued.trace.owner_phase_at_decision==KATMED_CONFIG_OWNER_PHASE_RENDER_CALL &&
           (queued.trace.owner_phase_seen_mask &
            (1u << KATMED_CONFIG_OWNER_PHASE_RENDER_CALL)) &&
           queued.trace.render_overlap_ms>0);
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_OTHER);
    katmed_config_poll(fast,NULL);
    assert(executions==2);

    /* After the loop takes a write, timeout is uncertain and must never be
     * reported as a successful save or leave a reused response slot. */
    Client running={.request.operation=KATMED_CONFIG_GET,.timeout_ms=30};
    assert(pthread_create(&thread,NULL,call,&running)==0);
    pause_ms(10);
    katmed_config_poll(slow,NULL);
    pthread_join(thread,NULL);
    assert(running.result==KATMED_CONFIG_COMMIT_UNKNOWN &&
           running.diagnostic==KATMED_CONFIG_BRIDGE_DIAG_PROCESSING_TIMEOUT && executions==3);
    assert((running.trace.valid & KATMED_CONFIG_TRACE_POLL_ENTER) &&
           !(running.trace.valid & KATMED_CONFIG_TRACE_POLL_COMPLETE) &&
           running.trace.slot_at_decision==KATMED_CONFIG_TRACE_SLOT_PROCESSING &&
           !running.trace.wait_signalled);
    Client last={.request.operation=KATMED_CONFIG_GET,.timeout_ms=1000};
    assert(pthread_create(&thread,NULL,call,&last)==0);
    pause_ms(10);
    katmed_config_poll(fast,NULL);
    pthread_join(thread,NULL);
    assert(last.result==KATMED_CONFIG_OK &&
           last.diagnostic==KATMED_CONFIG_BRIDGE_DIAG_RESPONSE && executions==4);
    assert(!(last.trace.owner_phase_seen_mask &
             (1u << KATMED_CONFIG_OWNER_PHASE_RENDER_CALL)) &&
           last.trace.slot_at_decision==KATMED_CONFIG_TRACE_SLOT_DONE);

    /* A render period already includes completed lock and image decisions at
     * enqueue. Their statuses belong to this period, not their old durations. */
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_RENDER_CALL);
    katmed_config_render_inner(KATMED_RENDER_INNER_LVGL_LOCK,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_LVGL_LOCK,false,1);
    katmed_config_render_inner(KATMED_RENDER_INNER_IMAGE_SELECT,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_IMAGE_SELECT,false,7);
    katmed_config_render_inner(KATMED_RENDER_INNER_IMAGE_PRESENT,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_IMAGE_PRESENT,false,0);
    Client pre_enqueue={.request.operation=KATMED_CONFIG_GET,.timeout_ms=20};
    arm_done_wait();
    assert(pthread_create(&thread,NULL,call,&pre_enqueue)==0);
    await_done_wait();
    pthread_join(thread,NULL);
    assert(pre_enqueue.diagnostic==KATMED_CONFIG_BRIDGE_DIAG_QUEUED_CANCELLED &&
           (pre_enqueue.trace.inner_status_valid &
               (KATMED_CONFIG_RENDER_STATUS_LOCK |
                KATMED_CONFIG_RENDER_STATUS_SELECT |
                KATMED_CONFIG_RENDER_STATUS_PRESENT))==
               (KATMED_CONFIG_RENDER_STATUS_LOCK |
                KATMED_CONFIG_RENDER_STATUS_SELECT |
                KATMED_CONFIG_RENDER_STATUS_PRESENT) &&
           pre_enqueue.trace.inner_lock_success==1 &&
           pre_enqueue.trace.inner_select_status==7 &&
           pre_enqueue.trace.inner_present_status==0 &&
           pre_enqueue.trace.inner_seen_mask==0 &&
           pre_enqueue.trace.inner_pre_enqueue_mask==
               ((1u<<KATMED_RENDER_INNER_LVGL_LOCK) |
                (1u<<KATMED_RENDER_INNER_IMAGE_SELECT) |
                (1u<<KATMED_RENDER_INNER_IMAGE_PRESENT)) &&
           pre_enqueue.trace.inner_overlap_ms[KATMED_RENDER_INNER_LVGL_LOCK]==0 &&
           pre_enqueue.trace.inner_overlap_ms[KATMED_RENDER_INNER_IMAGE_SELECT]==0 &&
           pre_enqueue.trace.inner_overlap_ms[KATMED_RENDER_INNER_IMAGE_PRESENT]==0);
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_OTHER);

    /* Owner completion can freeze the inner view before Web decision. The
     * open flag and ACTIVE value describe that freeze, never a later state. */
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_RENDER_CALL);
    katmed_config_render_inner(KATMED_RENDER_INNER_UI_RENDER,true,0);
    Client completed_during_render={.request.operation=KATMED_CONFIG_GET,
                                    .timeout_ms=1000};
    arm_done_wait();
    assert(pthread_create(&thread,NULL,call,&completed_during_render)==0);
    await_done_wait();
    katmed_config_poll(fast,NULL);
    katmed_config_render_inner(KATMED_RENDER_INNER_UI_RENDER,false,0);
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_OTHER);
    pthread_join(thread,NULL);
    assert(completed_during_render.result==KATMED_CONFIG_OK &&
           completed_during_render.trace.inner_period_at_freeze==
               KATMED_CONFIG_RENDER_INNER_ACTIVE &&
           (completed_during_render.trace.inner_open_freeze_mask &
               (1u<<KATMED_RENDER_INNER_UI_RENDER)) &&
           (completed_during_render.trace.inner_seen_mask &
               (1u<<KATMED_RENDER_INNER_UI_RENDER)));

    /* The render call and LVGL wait already run when the HTTP request enters.
     * A forced host tick offset crosses uint32 wrap during the real wait. */
    katmed_test_tick_offset=UINT32_MAX-15u-xTaskGetTickCount();
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_RENDER_CALL);
    katmed_config_render_inner(KATMED_RENDER_INNER_LVGL_LOCK,true,0);
    Client render_in_flight={.request.operation=KATMED_CONFIG_GET,.timeout_ms=40};
    arm_done_wait();
    assert(pthread_create(&thread,NULL,call,&render_in_flight)==0);
    await_done_wait();
    pthread_t intruder;
    assert(pthread_create(&intruder,NULL,foreign_mark,NULL)==0);
    pthread_join(intruder,NULL);
    katmed_config_render_inner(KATMED_RENDER_INNER_UI_RENDER,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_LANGUAGE_REBUILD,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_IMAGE_PRESENT,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_IMAGE_SELECT,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_VARIANT_HASH,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_VALIDATION_WAIT,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_VALIDATION_WAIT,false,1);
    katmed_config_render_inner(KATMED_RENDER_INNER_VARIANT_HASH,false,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_IMAGE_SELECT,false,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_IMAGE_PRESENT,false,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_LANGUAGE_REBUILD,false,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_UI_RENDER,false,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_UI_RENDER,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_UI_RENDER,false,0);
    pthread_join(thread,NULL);
    assert(render_in_flight.diagnostic==KATMED_CONFIG_BRIDGE_DIAG_QUEUED_CANCELLED);
    const KatmedConfigBridgeTrace frozen=render_in_flight.trace;
    assert((frozen.valid & KATMED_CONFIG_TRACE_RENDER_INNER) &&
           frozen.inner_period_at_freeze==KATMED_CONFIG_RENDER_INNER_ACTIVE &&
           frozen.gate_start_tick>frozen.decision_tick &&
           (frozen.inner_open_enqueue_mask & (1u<<KATMED_RENDER_INNER_LVGL_LOCK)) &&
           (frozen.inner_open_freeze_mask & (1u<<KATMED_RENDER_INNER_LVGL_LOCK)) &&
           frozen.inner_overlap_ms[KATMED_RENDER_INNER_LVGL_LOCK]>=20u &&
           (frozen.inner_seen_mask & (1u<<KATMED_RENDER_INNER_IMAGE_SELECT)) &&
           !(frozen.inner_seen_mask & (1u<<KATMED_RENDER_INNER_JPEG_DECODE)) &&
           (frozen.inner_multi_mask & (1u<<KATMED_RENDER_INNER_UI_RENDER)) &&
           (frozen.inner_status_valid & (KATMED_CONFIG_RENDER_STATUS_SELECT |
                                        KATMED_CONFIG_RENDER_STATUS_PRESENT)) ==
               (KATMED_CONFIG_RENDER_STATUS_SELECT | KATMED_CONFIG_RENDER_STATUS_PRESENT) &&
           !(frozen.inner_status_valid & KATMED_CONFIG_RENDER_STATUS_LOCK) &&
           frozen.inner_select_status==0 && frozen.inner_present_status==0);
    katmed_config_render_inner(KATMED_RENDER_INNER_LVGL_LOCK,false,1);
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_OTHER);
    assert(memcmp(&frozen,&render_in_flight.trace,sizeof(frozen))==0);
    katmed_test_tick_offset=0;

    /* A completed earlier period is never a global last-render result. */
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_RENDER_CALL);
    katmed_config_render_inner(KATMED_RENDER_INNER_IMAGE_PRESENT,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_IMAGE_PRESENT,false,0);
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_OTHER);
    Client stale={.request.operation=KATMED_CONFIG_GET,.timeout_ms=20};
    assert(pthread_create(&thread,NULL,call,&stale)==0);
    pthread_join(thread,NULL);
    assert(!(stale.trace.valid & KATMED_CONFIG_TRACE_RENDER_INNER));

    /* Two periods in one request are marked MULTI, without mixed details. */
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_RENDER_CALL);
    Client multiple={.request.operation=KATMED_CONFIG_GET,.timeout_ms=80};
    arm_done_wait();
    assert(pthread_create(&thread,NULL,call,&multiple)==0);
    await_done_wait();
    katmed_config_render_inner(KATMED_RENDER_INNER_UI_RENDER,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_UI_RENDER,false,0);
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_OTHER);
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_RENDER_CALL);
    katmed_config_render_inner(KATMED_RENDER_INNER_IMAGE_PRESENT,true,0);
    katmed_config_render_inner(KATMED_RENDER_INNER_IMAGE_PRESENT,false,0);
    pthread_join(thread,NULL);
    assert(multiple.trace.inner_period_at_freeze==KATMED_CONFIG_RENDER_INNER_MULTI &&
           multiple.trace.inner_seen_mask==0 && multiple.trace.inner_multi_mask==0 &&
           multiple.trace.inner_pre_enqueue_mask==0 &&
           multiple.trace.inner_status_valid==0);
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_OTHER);

    /* A later successful request must not inherit the cancelled period. */
    Client after={.request.operation=KATMED_CONFIG_GET,.timeout_ms=1000};
    arm_done_wait();
    assert(pthread_create(&thread,NULL,call,&after)==0);
    await_done_wait();
    katmed_config_owner_phase(KATMED_CONFIG_OWNER_PHASE_POLL);
    katmed_config_poll(fast,NULL);
    pthread_join(thread,NULL);
    assert(after.result==KATMED_CONFIG_OK &&
           !(after.trace.valid & KATMED_CONFIG_TRACE_RENDER_INNER));
    puts("bounded config bridge passed");
}
