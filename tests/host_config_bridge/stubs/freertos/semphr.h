#ifndef HOST_CONFIG_SEMPHR_H
#define HOST_CONFIG_SEMPHR_H
#include <errno.h>
#include <time.h>
#include "FreeRTOS.h"
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    unsigned count;
} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    pthread_mutex_init(&storage->mutex,NULL);
    pthread_cond_init(&storage->changed,NULL);
    storage->count=0;
    return storage;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    pthread_mutex_lock(&sem->mutex);
    sem->count=1;
    pthread_cond_signal(&sem->changed);
    pthread_mutex_unlock(&sem->mutex);
    return pdTRUE;
}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ms)
{
    pthread_mutex_lock(&sem->mutex);
    if (!sem->count && ms) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME,&deadline);
        deadline.tv_sec+=ms/1000u;
        deadline.tv_nsec+=(long)(ms%1000u)*1000000L;
        if (deadline.tv_nsec>=1000000000L) {
            deadline.tv_nsec-=1000000000L;
            ++deadline.tv_sec;
        }
        while (!sem->count) {
            const int result=pthread_cond_timedwait(&sem->changed,&sem->mutex,&deadline);
            if (result==ETIMEDOUT) break;
        }
    }
    const BaseType_t result=sem->count ? pdTRUE : pdFALSE;
    if (sem->count) sem->count=0;
    pthread_mutex_unlock(&sem->mutex);
    return result;
}
#endif
