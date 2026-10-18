#ifndef KATMED_TEST_LVGL_PORT_H
#define KATMED_TEST_LVGL_PORT_H
#include <stdbool.h>
#include <stdint.h>
bool lvgl_port_lock(uint32_t timeout);
void lvgl_port_unlock(void);
#endif
