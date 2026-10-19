#ifndef HOST_STORE_SEMPHR_H
#define HOST_STORE_SEMPHR_H
#include "freertos/FreeRTOS.h"
typedef struct { int available; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    storage->available = 1;
    return storage;
}
static inline int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned timeout)
{
    (void)timeout;
    if (!semaphore || !semaphore->available) return 0;
    semaphore->available = 0;
    return 1;
}
static inline int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (!semaphore) return 0;
    semaphore->available = 1;
    return 1;
}
#endif
