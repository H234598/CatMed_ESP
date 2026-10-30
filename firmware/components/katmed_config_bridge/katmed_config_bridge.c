#include "katmed_config_bridge.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static void clear_request(KatmedConfigRequest *r) { volatile unsigned char *p=(volatile unsigned char *)r; for(size_t i=0;i<sizeof(*r);++i)p[i]=0; }

typedef enum {
    SLOT_IDLE = KATMED_CONFIG_TRACE_SLOT_IDLE,
    SLOT_QUEUED = KATMED_CONFIG_TRACE_SLOT_QUEUED,
    SLOT_PROCESSING = KATMED_CONFIG_TRACE_SLOT_PROCESSING,
    SLOT_DONE = KATMED_CONFIG_TRACE_SLOT_DONE
} SlotState;

static StaticSemaphore_t gate_storage, done_storage;
static SemaphoreHandle_t gate, done;
static portMUX_TYPE slot_lock = portMUX_INITIALIZER_UNLOCKED;
static SlotState slot_state;
static bool abandoned;
static KatmedConfigRequest slot_request;
static KatmedConfigResponse slot_response;
static KatmedConfigBridgeTrace slot_trace;
static bool slot_trace_enabled, slot_trace_open;
static TickType_t slot_phase_tick;
static KatmedConfigOwnerPhase owner_phase;
static bool owner_phase_valid;

/* A current-period scratch, not a last-render result. Marker calls can occur
 * while LVGL or image locks are held, so they never take slot_lock. The bridge
 * copies only under slot_lock -> render_lock, never the reverse order. */
typedef struct {
    bool active;
    TaskHandle_t owner_task;
    uint32_t entered[KATMED_RENDER_INNER_COUNT];
    uint32_t completed[KATMED_RENDER_INNER_COUNT];
    uint32_t total_ticks[KATMED_RENDER_INNER_COUNT];
    TickType_t start_tick[KATMED_RENDER_INNER_COUNT];
    uint32_t depth[KATMED_RENDER_INNER_COUNT];
    uint8_t last_outcome[KATMED_RENDER_INNER_COUNT];
} RenderScratch;
static portMUX_TYPE render_lock = portMUX_INITIALIZER_UNLOCKED;
static RenderScratch render_scratch;
static bool slot_render_bound, slot_render_multi;
static uint32_t slot_render_base_ticks[KATMED_RENDER_INNER_COUNT];
static uint32_t slot_render_base_entered[KATMED_RENDER_INNER_COUNT];
static uint32_t slot_render_base_depth[KATMED_RENDER_INNER_COUNT];

static uint32_t render_total_ticks(unsigned phase, TickType_t now)
{
    return render_scratch.total_ticks[phase] +
        (render_scratch.depth[phase] ?
            (uint32_t)(TickType_t)(now-render_scratch.start_tick[phase]) : 0u);
}

/* Called with both slot_lock and render_lock held. The baseline is taken at
 * enqueue even if this render call began earlier. Tick arithmetic wraps. */
static void bind_render(TickType_t now)
{
    slot_render_bound=true;
    for (unsigned i=0;i<KATMED_RENDER_INNER_COUNT;++i) {
        slot_render_base_ticks[i]=render_total_ticks(i,now);
        slot_render_base_entered[i]=render_scratch.entered[i];
        slot_render_base_depth[i]=render_scratch.depth[i];
        if (render_scratch.completed[i])
            slot_trace.inner_pre_enqueue_mask|=1u<<i;
    }
    slot_trace.valid|=KATMED_CONFIG_TRACE_RENDER_INNER;
    slot_trace.inner_period_at_freeze=KATMED_CONFIG_RENDER_INNER_ACTIVE;
}

static void render_multiple_periods(void)
{
    slot_render_multi=true;
    slot_trace.valid|=KATMED_CONFIG_TRACE_RENDER_INNER;
    slot_trace.inner_period_at_freeze=KATMED_CONFIG_RENDER_INNER_MULTI;
    slot_trace.inner_seen_mask=slot_trace.inner_multi_mask=0;
    slot_trace.inner_pre_enqueue_mask=0;
    slot_trace.inner_open_enqueue_mask=slot_trace.inner_open_freeze_mask=0;
    memset(slot_trace.inner_overlap_ms,0,sizeof(slot_trace.inner_overlap_ms));
    slot_trace.inner_status_valid=slot_trace.inner_lock_success=0;
    slot_trace.inner_select_status=slot_trace.inner_present_status=0;
}

/* One bounded snapshot of the bound period. Durations are intersections with
 * this request and are inclusive of nested work and task descheduling. */
static void snapshot_render(TickType_t now, bool ended)
{
    if (!slot_render_bound || slot_render_multi || !slot_trace_open) return;
    slot_trace.inner_period_at_freeze=ended ? KATMED_CONFIG_RENDER_INNER_ENDED :
                                         KATMED_CONFIG_RENDER_INNER_ACTIVE;
    slot_trace.inner_seen_mask=slot_trace.inner_multi_mask=0;
    slot_trace.inner_open_enqueue_mask=slot_trace.inner_open_freeze_mask=0;
    slot_trace.inner_status_valid=0;
    for (unsigned i=0;i<KATMED_RENDER_INNER_COUNT;++i) {
        const uint32_t bit=1u<<i;
        const uint32_t entered=render_scratch.entered[i]-slot_render_base_entered[i];
        if (slot_render_base_depth[i]) slot_trace.inner_open_enqueue_mask|=bit;
        if (!ended && render_scratch.depth[i]) slot_trace.inner_open_freeze_mask|=bit;
        if (render_scratch.entered[i]>1)
            slot_trace.inner_multi_mask|=bit;
        if (!slot_render_base_depth[i] && entered==0) {
            slot_trace.inner_overlap_ms[i]=0;
            continue;
        }
        slot_trace.inner_seen_mask|=bit;
        const uint32_t ticks=render_total_ticks(i,now)-slot_render_base_ticks[i];
        const uint64_t ms=(uint64_t)ticks*portTICK_PERIOD_MS;
        slot_trace.inner_overlap_ms[i]=ms>UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
    }
    if (render_scratch.completed[KATMED_RENDER_INNER_LVGL_LOCK]) {
        slot_trace.inner_status_valid|=KATMED_CONFIG_RENDER_STATUS_LOCK;
        slot_trace.inner_lock_success=
            render_scratch.last_outcome[KATMED_RENDER_INNER_LVGL_LOCK];
    }
    if (render_scratch.completed[KATMED_RENDER_INNER_IMAGE_SELECT]) {
        slot_trace.inner_status_valid|=KATMED_CONFIG_RENDER_STATUS_SELECT;
        slot_trace.inner_select_status=
            render_scratch.last_outcome[KATMED_RENDER_INNER_IMAGE_SELECT];
    }
    if (render_scratch.completed[KATMED_RENDER_INNER_IMAGE_PRESENT]) {
        slot_trace.inner_status_valid|=KATMED_CONFIG_RENDER_STATUS_PRESENT;
        slot_trace.inner_present_status=
            render_scratch.last_outcome[KATMED_RENDER_INNER_IMAGE_PRESENT];
    }
}

void katmed_config_render_inner(KatmedRenderInnerPhase phase, bool begin,
                                uint8_t outcome)
{
    if ((unsigned)phase>=KATMED_RENDER_INNER_COUNT) return;
    const TaskHandle_t current=xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&render_lock);
    if (render_scratch.active && current==render_scratch.owner_task) {
        const TickType_t now=xTaskGetTickCount();
        const unsigned i=(unsigned)phase;
        if (begin) {
            if (render_scratch.depth[i]==0) render_scratch.start_tick[i]=now;
            if (render_scratch.depth[i]!=UINT32_MAX) ++render_scratch.depth[i];
            ++render_scratch.entered[i];
        } else if (render_scratch.depth[i]) {
            if (--render_scratch.depth[i]==0) {
                render_scratch.total_ticks[i]+=
                    (uint32_t)(TickType_t)(now-render_scratch.start_tick[i]);
                render_scratch.last_outcome[i]=outcome;
                ++render_scratch.completed[i];
            }
        }
    }
    portEXIT_CRITICAL(&render_lock);
}

static uint32_t elapsed_ms(TickType_t start, TickType_t end)
{
    const uint64_t ms=(uint64_t)(TickType_t)(end-start)*portTICK_PERIOD_MS;
    return ms>UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

/* Called only with slot_lock held. Attribute phase time solely to this live
 * request; a completed or cancelled request can never inherit later work. */
static void account_owner_phase(TickType_t now)
{
    if (!slot_trace_open || (slot_state!=SLOT_QUEUED && slot_state!=SLOT_PROCESSING))
        return;
    if (owner_phase_valid && owner_phase==KATMED_CONFIG_OWNER_PHASE_RENDER_CALL) {
        const uint32_t elapsed=elapsed_ms(slot_phase_tick,now);
        slot_trace.render_overlap_ms=UINT32_MAX-slot_trace.render_overlap_ms<elapsed ?
            UINT32_MAX : slot_trace.render_overlap_ms+elapsed;
    }
    slot_phase_tick=now;
}

void katmed_config_owner_phase(KatmedConfigOwnerPhase phase)
{
    if ((unsigned)phase>KATMED_CONFIG_OWNER_PHASE_SLEEP) return;
    portENTER_CRITICAL(&slot_lock);
    const TickType_t now=xTaskGetTickCount();
    account_owner_phase(now);
    const bool leaving=owner_phase==KATMED_CONFIG_OWNER_PHASE_RENDER_CALL &&
                       phase!=KATMED_CONFIG_OWNER_PHASE_RENDER_CALL;
    const bool entering=owner_phase!=KATMED_CONFIG_OWNER_PHASE_RENDER_CALL &&
                        phase==KATMED_CONFIG_OWNER_PHASE_RENDER_CALL;
    if (leaving || entering) {
        portENTER_CRITICAL(&render_lock);
        if (leaving) {
            if (slot_trace_open && slot_render_bound && !slot_render_multi)
                snapshot_render(now,true);
            render_scratch.active=false;
        }
        if (entering) {
            memset(&render_scratch,0,sizeof(render_scratch));
            render_scratch.active=true;
            render_scratch.owner_task=xTaskGetCurrentTaskHandle();
            if (slot_trace_open && (slot_state==SLOT_QUEUED ||
                                    slot_state==SLOT_PROCESSING)) {
                if (slot_render_bound) render_multiple_periods();
                else bind_render(now);
            }
        }
        portEXIT_CRITICAL(&render_lock);
    }
    owner_phase=phase;
    owner_phase_valid=phase!=KATMED_CONFIG_OWNER_PHASE_UNKNOWN;
    if (slot_trace_open && (slot_state==SLOT_QUEUED || slot_state==SLOT_PROCESSING) &&
        owner_phase_valid) {
        slot_trace.owner_phase_seen_mask|=1u<<(unsigned)phase;
        slot_trace.valid|=KATMED_CONFIG_TRACE_PHASE_SEEN;
    }
    portEXIT_CRITICAL(&slot_lock);
}

bool katmed_config_bridge_start(void)
{
    if (gate || done) return false;
    gate = xSemaphoreCreateBinaryStatic(&gate_storage);
    done = xSemaphoreCreateBinaryStatic(&done_storage);
    if (!gate || !done) return false;
    slot_state = SLOT_IDLE;
    slot_trace_enabled = slot_trace_open = false;
    slot_render_bound=slot_render_multi=false;
    owner_phase_valid = false;
    xSemaphoreGive(gate);
    return true;
}

KatmedConfigStatus katmed_config_exchange_diagnostic(
    const KatmedConfigRequest *request, KatmedConfigResponse *response,
    uint32_t timeout_ms, KatmedConfigBridgeDiagnostic *diagnostic,
    KatmedConfigBridgeTrace *trace)
{
    KatmedConfigBridgeTrace local_trace={0};
    if (trace) *trace=local_trace;
    if (!gate || !done || !request || !response || timeout_ms == 0) {
        if (diagnostic) *diagnostic=KATMED_CONFIG_BRIDGE_DIAG_PRECONDITION;
        return KATMED_CONFIG_UNAVAILABLE;
    }
    if (trace) {
        local_trace.tick_period_ms=portTICK_PERIOD_MS;
        local_trace.gate_start_tick=(uint32_t)xTaskGetTickCount();
        local_trace.valid|=KATMED_CONFIG_TRACE_GATE_START;
    }
    const BaseType_t admitted=xSemaphoreTake(gate,pdMS_TO_TICKS(timeout_ms));
    if (trace) {
        local_trace.gate_return_tick=(uint32_t)xTaskGetTickCount();
        local_trace.gate_acquired=admitted==pdTRUE;
        local_trace.valid|=KATMED_CONFIG_TRACE_GATE_RETURN;
    }
    if (admitted != pdTRUE) {
        if (trace) *trace=local_trace;
        if (diagnostic) *diagnostic=KATMED_CONFIG_BRIDGE_DIAG_ADMISSION_TIMEOUT;
        return KATMED_CONFIG_UNAVAILABLE;
    }
    /* A previous abandoned request can never leave a stale completion token. */
    (void)xSemaphoreTake(done, 0);
    portENTER_CRITICAL(&slot_lock);
    slot_request = *request;
    slot_response = (KatmedConfigResponse){.status = KATMED_CONFIG_UNAVAILABLE};
    slot_state = SLOT_QUEUED;
    abandoned = false;
    slot_trace=local_trace;
    slot_trace_enabled=slot_trace_open=trace!=NULL;
    slot_render_bound=slot_render_multi=false;
    if (slot_trace_enabled) {
        const TickType_t now=xTaskGetTickCount();
        slot_trace.enqueue_tick=(uint32_t)now;
        slot_trace.valid|=KATMED_CONFIG_TRACE_ENQUEUE;
        slot_phase_tick=now;
        if (owner_phase_valid) {
            slot_trace.owner_phase_at_enqueue=(uint8_t)owner_phase;
            slot_trace.owner_phase_seen_mask|=1u<<(unsigned)owner_phase;
            slot_trace.valid|=KATMED_CONFIG_TRACE_PHASE_ENQUEUE |
                              KATMED_CONFIG_TRACE_PHASE_SEEN;
        }
        portENTER_CRITICAL(&render_lock);
        if (render_scratch.active && owner_phase==KATMED_CONFIG_OWNER_PHASE_RENDER_CALL)
            bind_render(xTaskGetTickCount());
        portEXIT_CRITICAL(&render_lock);
    }
    portEXIT_CRITICAL(&slot_lock);

    const TickType_t wait_start=xTaskGetTickCount();
    const BaseType_t signalled = xSemaphoreTake(done, pdMS_TO_TICKS(timeout_ms));
    const TickType_t wait_return=xTaskGetTickCount();
    portENTER_CRITICAL(&slot_lock);
    if (slot_trace_enabled) {
        const TickType_t decision=xTaskGetTickCount();
        account_owner_phase(decision);
        portENTER_CRITICAL(&render_lock);
        if (render_scratch.active && slot_render_bound && !slot_render_multi)
            snapshot_render(xTaskGetTickCount(),false);
        portEXIT_CRITICAL(&render_lock);
        slot_trace.wait_return_tick=(uint32_t)wait_return;
        slot_trace.wait_elapsed_ms=elapsed_ms(wait_start,wait_return);
        slot_trace.wait_signalled=signalled==pdTRUE;
        slot_trace.slot_at_decision=(uint8_t)slot_state;
        slot_trace.decision_tick=(uint32_t)decision;
        slot_trace.valid|=KATMED_CONFIG_TRACE_WAIT_RETURN |
                          KATMED_CONFIG_TRACE_DECISION;
        if (owner_phase_valid) {
            slot_trace.owner_phase_at_decision=(uint8_t)owner_phase;
            slot_trace.valid|=KATMED_CONFIG_TRACE_PHASE_DECISION;
        }
        *trace=slot_trace;
        slot_trace_enabled=slot_trace_open=false;
    }
    if (slot_state == SLOT_DONE) {
        *response = slot_response;
        slot_state = SLOT_IDLE;
        portEXIT_CRITICAL(&slot_lock);
        xSemaphoreGive(gate);
        if (diagnostic) *diagnostic=KATMED_CONFIG_BRIDGE_DIAG_RESPONSE;
        return response->status;
    }
    /* A queued request is cancelled without side effects. An executing
     * request can have committed, so its outcome must remain uncertain. */
    const bool processing = slot_state == SLOT_PROCESSING;
    if (processing) abandoned = true;
    else {slot_state = SLOT_IDLE; clear_request(&slot_request);}
    portEXIT_CRITICAL(&slot_lock);
    if (!processing) xSemaphoreGive(gate);
    (void)signalled;
    if (diagnostic) *diagnostic=processing ?
        KATMED_CONFIG_BRIDGE_DIAG_PROCESSING_TIMEOUT :
        KATMED_CONFIG_BRIDGE_DIAG_QUEUED_CANCELLED;
    return processing ? KATMED_CONFIG_COMMIT_UNKNOWN : KATMED_CONFIG_UNAVAILABLE;
}

KatmedConfigStatus katmed_config_exchange(const KatmedConfigRequest *request,
                                          KatmedConfigResponse *response,
                                          uint32_t timeout_ms)
{
    return katmed_config_exchange_diagnostic(request,response,timeout_ms,NULL,NULL);
}

void katmed_config_poll(KatmedConfigExecute execute, void *context)
{
    if (!gate || !done || !execute) return;
    KatmedConfigRequest request;
    portENTER_CRITICAL(&slot_lock);
    if (slot_state != SLOT_QUEUED) {
        portEXIT_CRITICAL(&slot_lock);
        return;
    }
    request = slot_request;
    clear_request(&slot_request);
    if (slot_trace_open) {
        slot_trace.poll_enter_tick=(uint32_t)xTaskGetTickCount();
        slot_trace.valid|=KATMED_CONFIG_TRACE_POLL_ENTER;
    }
    slot_state = SLOT_PROCESSING;
    portEXIT_CRITICAL(&slot_lock);

    KatmedConfigResponse response = {.status = KATMED_CONFIG_UNAVAILABLE};
    execute(context, &request, &response);
    clear_request(&request);

    portENTER_CRITICAL(&slot_lock);
    if (slot_trace_open) {
        const TickType_t now=xTaskGetTickCount();
        account_owner_phase(now);
        portENTER_CRITICAL(&render_lock);
        if (render_scratch.active && slot_render_bound && !slot_render_multi)
            snapshot_render(xTaskGetTickCount(),false);
        portEXIT_CRITICAL(&render_lock);
        slot_trace.poll_complete_tick=(uint32_t)now;
        slot_trace.valid|=KATMED_CONFIG_TRACE_POLL_COMPLETE;
        slot_trace_open=false;
    }
    if (abandoned) {
        slot_state = SLOT_IDLE;
        abandoned = false;
        portEXIT_CRITICAL(&slot_lock);
        xSemaphoreGive(gate);
        return;
    }
    slot_response = response;
    slot_state = SLOT_DONE;
    portEXIT_CRITICAL(&slot_lock);
    xSemaphoreGive(done);
}
