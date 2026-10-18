#ifndef KATMED_CLOCK_VIEW_H
#define KATMED_CLOCK_VIEW_H

#include "katmed_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *clock_view_create(lv_obj_t *parent);
void clock_view_update(lv_obj_t *view, ClockStyle style, const ClockSample *clock);

#ifdef __cplusplus
}
#endif
#endif
