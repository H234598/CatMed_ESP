#ifndef CHECK_UI_REFINEMENT_MAIN_H
#define CHECK_UI_REFINEMENT_MAIN_H
#include "katmed_ui.h"
void refinement_init(const uint8_t *catalog, uint32_t count);
UiServices refinement_services(void);
const AppState *refinement_state(void);
const LocalSettings *refinement_local(void);
ClockSample refinement_clock(void);
void refinement_advance(uint32_t ms);
AppResult refinement_image(uint32_t id);
unsigned refinement_writes(void);
void refinement_index_contract(void);
void refinement_fail_commit(bool fail);
#endif

void refinement_tick_contract(void);
