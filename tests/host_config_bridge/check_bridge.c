#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "katmed_config_bridge.h"

typedef struct {
    KatmedConfigRequest request;
    KatmedConfigResponse response;
    uint32_t timeout_ms;
    KatmedConfigStatus result;
    KatmedConfigBridgeDiagnostic diagnostic;
} Client;
static unsigned executions;

static void pause_ms(long ms)
{
    const struct timespec time={.tv_sec=ms/1000,.tv_nsec=(ms%1000)*1000000L};
    nanosleep(&time,NULL);
}
static void *call(void *context)
{
    Client *client=context;
    client->result=katmed_config_exchange_diagnostic(&client->request,&client->response,
                                                    client->timeout_ms,&client->diagnostic);
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
    assert(katmed_config_exchange_diagnostic(&invalid,&ignored,0,&precondition)==
           KATMED_CONFIG_UNAVAILABLE);
    assert(precondition==KATMED_CONFIG_BRIDGE_DIAG_PRECONDITION);
    Client first={.request.operation=KATMED_CONFIG_GET,.timeout_ms=1000};
    pthread_t thread;
    assert(pthread_create(&thread,NULL,call,&first)==0);
    pause_ms(10);
    katmed_config_poll(fast,NULL);
    pthread_join(thread,NULL);
    assert(first.result==KATMED_CONFIG_OK &&
           first.diagnostic==KATMED_CONFIG_BRIDGE_DIAG_RESPONSE &&
           strcmp(first.response.snapshot.medication_hours,"12")==0 && executions==1);

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
        admission.timeout_ms,&admission.diagnostic)==KATMED_CONFIG_UNAVAILABLE);
    assert(admission.diagnostic==KATMED_CONFIG_BRIDGE_DIAG_ADMISSION_TIMEOUT);
    pthread_join(thread,NULL);
    assert(queued.result==KATMED_CONFIG_UNAVAILABLE &&
           queued.diagnostic==KATMED_CONFIG_BRIDGE_DIAG_QUEUED_CANCELLED);
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
    Client last={.request.operation=KATMED_CONFIG_GET,.timeout_ms=1000};
    assert(pthread_create(&thread,NULL,call,&last)==0);
    pause_ms(10);
    katmed_config_poll(fast,NULL);
    pthread_join(thread,NULL);
    assert(last.result==KATMED_CONFIG_OK &&
           last.diagnostic==KATMED_CONFIG_BRIDGE_DIAG_RESPONSE && executions==4);
    puts("bounded config bridge passed");
}
