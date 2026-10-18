#ifndef KATMED_TEST_FREERTOS_TASK_H
#define KATMED_TEST_FREERTOS_TASK_H
typedef void *TaskHandle_t;
typedef unsigned int UBaseType_t;
TaskHandle_t xTaskGetHandle(const char *name);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t handle);
#endif
