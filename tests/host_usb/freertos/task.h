#ifndef KATMED_TEST_TASK_H
#define KATMED_TEST_TASK_H

#include "FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreate(TaskFunction_t task, const char *name, unsigned int stack,
                       void *arg, unsigned int priority, void *task_handle);
void vTaskDelay(unsigned int ticks);

#endif
