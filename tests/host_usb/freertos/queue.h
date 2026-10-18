#ifndef KATMED_TEST_QUEUE_H
#define KATMED_TEST_QUEUE_H

#include "FreeRTOS.h"

typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned int length, unsigned int item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, unsigned int ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, unsigned int ticks);
void vQueueDelete(QueueHandle_t queue);

#endif
