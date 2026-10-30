#ifndef HOST_CONFIG_TASK_H
#define HOST_CONFIG_TASK_H
#include <time.h>
#include "FreeRTOS.h"
static inline TickType_t xTaskGetTickCount(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC,&now)!=0) return 0;
    return (TickType_t)((uint64_t)now.tv_sec*1000u+(uint64_t)now.tv_nsec/1000000u);
}
#endif
