#ifndef CHECK_UI_REFINEMENT_MAIN_H
#define CHECK_UI_REFINEMENT_MAIN_H
#include "katmed_ui.h"
#include "katmed_web_wifi.h"
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
void refinement_wifi_prepare(void);
void refinement_language_commit(uint8_t language);
KatmedWebWifiServices refinement_wifi_services(void);
void refinement_render_owner(void);
void refinement_wifi_snapshot_contract(void);
void refinement_poll_owner(void);
#endif

void refinement_tick_contract(void);
