#ifndef HOST_CONFIG_TASK_H
#define HOST_CONFIG_TASK_H
#include <time.h>
#include "FreeRTOS.h"
typedef pthread_t TaskHandle_t;
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return pthread_self(); }
static inline TickType_t xTaskGetTickCount(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC,&now)!=0) return 0;
    TickType_t tick=(TickType_t)((uint64_t)now.tv_sec*1000u+
                                 (uint64_t)now.tv_nsec/1000000u);
#ifdef KATMED_CONFIG_BRIDGE_TEST_TICK
    extern TickType_t katmed_test_tick_offset;
    tick+=katmed_test_tick_offset;
#endif
    return tick;
}
#endif
