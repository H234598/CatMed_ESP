/* Execute the production Main tick with the real Mesh adapter in the C++ peer fixture. */
#define KATMED_FULL_STACK 1
#define KATMED_WITH_CORE 1
#include "../firmware/main/main.c"
uint32_t esp_random(void) { static uint32_t counter=13;return ++counter; }
void refinement_actual_tick(Store *store, AppState *state, const ClockSample *now)
{
    runtime.store=*store;runtime.state=*state;
    runtime.store_ready=true;runtime.mesh_started=true;
    tick_runtime(&runtime,now);
    *store=runtime.store;*state=runtime.state;
}
