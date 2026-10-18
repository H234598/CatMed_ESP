#ifndef TEST_FREERTOS_QUEUE_H
#define TEST_FREERTOS_QUEUE_H
#include <stddef.h>
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned length, unsigned item_size);
int xQueueOverwrite(QueueHandle_t queue, const void *item);
int xQueueReceive(QueueHandle_t queue, void *item, unsigned ticks);
int xQueueReset(QueueHandle_t queue);
#endif
