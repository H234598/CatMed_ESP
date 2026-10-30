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
    }
    portEXIT_CRITICAL(&slot_lock);

    const TickType_t wait_start=xTaskGetTickCount();
    const BaseType_t signalled = xSemaphoreTake(done, pdMS_TO_TICKS(timeout_ms));
    const TickType_t wait_return=xTaskGetTickCount();
    portENTER_CRITICAL(&slot_lock);
    if (slot_trace_enabled) {
        const TickType_t decision=xTaskGetTickCount();
        account_owner_phase(decision);
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
