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
    client->result=katmed_config_exchange(&client->request,&client->response,
                                          client->timeout_ms);
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
int main(void)
{
    assert(katmed_config_bridge_start());
    assert(!katmed_config_bridge_start());
    Client first={.request.operation=KATMED_CONFIG_GET,.timeout_ms=1000};
    pthread_t thread;
    assert(pthread_create(&thread,NULL,call,&first)==0);
    pause_ms(10);
    katmed_config_poll(fast,NULL);
    pthread_join(thread,NULL);
    assert(first.result==KATMED_CONFIG_OK &&
           strcmp(first.response.snapshot.medication_hours,"12")==0 && executions==1);

    /* A request that times out before the loop takes it is cancelled. */
    Client queued={.request.operation=KATMED_CONFIG_GET,.timeout_ms=20};
    assert(katmed_config_exchange(&queued.request,&queued.response,
                                  queued.timeout_ms)==KATMED_CONFIG_UNAVAILABLE);
    katmed_config_poll(fast,NULL);
    assert(executions==1);

    /* After the loop takes a write, timeout is uncertain and must never be
     * reported as a successful save or leave a reused response slot. */
    Client running={.request.operation=KATMED_CONFIG_GET,.timeout_ms=30};
    assert(pthread_create(&thread,NULL,call,&running)==0);
    pause_ms(10);
    katmed_config_poll(slow,NULL);
    pthread_join(thread,NULL);
    assert(running.result==KATMED_CONFIG_COMMIT_UNKNOWN && executions==2);
    Client last={.request.operation=KATMED_CONFIG_GET,.timeout_ms=1000};
    assert(pthread_create(&thread,NULL,call,&last)==0);
    pause_ms(10);
    katmed_config_poll(fast,NULL);
    pthread_join(thread,NULL);
    assert(last.result==KATMED_CONFIG_OK && executions==3);
    puts("bounded config bridge passed");
}
