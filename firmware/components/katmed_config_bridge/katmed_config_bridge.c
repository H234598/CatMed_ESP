#include "katmed_config_bridge.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static void clear_request(KatmedConfigRequest *r) { volatile unsigned char *p=(volatile unsigned char *)r; for(size_t i=0;i<sizeof(*r);++i)p[i]=0; }

typedef enum { SLOT_IDLE, SLOT_QUEUED, SLOT_PROCESSING, SLOT_DONE } SlotState;

static StaticSemaphore_t gate_storage, done_storage;
static SemaphoreHandle_t gate, done;
static portMUX_TYPE slot_lock = portMUX_INITIALIZER_UNLOCKED;
static SlotState slot_state;
static bool abandoned;
static KatmedConfigRequest slot_request;
static KatmedConfigResponse slot_response;

bool katmed_config_bridge_start(void)
{
    if (gate || done) return false;
    gate = xSemaphoreCreateBinaryStatic(&gate_storage);
    done = xSemaphoreCreateBinaryStatic(&done_storage);
    if (!gate || !done) return false;
    slot_state = SLOT_IDLE;
    xSemaphoreGive(gate);
    return true;
}

KatmedConfigStatus katmed_config_exchange_diagnostic(
    const KatmedConfigRequest *request, KatmedConfigResponse *response,
    uint32_t timeout_ms, KatmedConfigBridgeDiagnostic *diagnostic)
{
    if (!gate || !done || !request || !response || timeout_ms == 0) {
        if (diagnostic) *diagnostic=KATMED_CONFIG_BRIDGE_DIAG_PRECONDITION;
        return KATMED_CONFIG_UNAVAILABLE;
    }
    if (xSemaphoreTake(gate, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
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
    portEXIT_CRITICAL(&slot_lock);

    const BaseType_t signalled = xSemaphoreTake(done, pdMS_TO_TICKS(timeout_ms));
    portENTER_CRITICAL(&slot_lock);
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
    return katmed_config_exchange_diagnostic(request,response,timeout_ms,NULL);
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
    slot_state = SLOT_PROCESSING;
    portEXIT_CRITICAL(&slot_lock);

    KatmedConfigResponse response = {.status = KATMED_CONFIG_UNAVAILABLE};
    execute(context, &request, &response);
    clear_request(&request);

    portENTER_CRITICAL(&slot_lock);
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
