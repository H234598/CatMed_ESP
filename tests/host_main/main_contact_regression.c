#include <assert.h>
#include <stdio.h>
#include <string.h>
#define KATMED_FULL_STACK 1
#define KATMED_WITH_CORE 1
#include "../../firmware/main/main.c"
static KatmedInteractionInput transient_inputs[40];
static uint64_t transient_times[40];
static unsigned transient_count;
#ifndef KATMED_REAL_INTERACTION_ADAPTER
bool katmed_mesh_interaction_input(const KatmedInteractionInput *input, uint64_t now) {
    assert(transient_count < 40);
    transient_inputs[transient_count] = *input;
    transient_times[transient_count++] = now;
    return true;
}
void katmed_mesh_interaction_status(KatmedInteractionStatus *out, uint64_t now) {
    (void)now;
    *out = (KatmedInteractionStatus){.menu_open=true, .menu_entry=7, .grant_id=11};
}
#endif
#ifdef KATMED_REAL_INTERACTION_ADAPTER
void main_engine_start(void);
bool main_engine_entered(void);
void main_engine_prepare_grant(void);
bool main_engine_received_180ms(void);
bool main_engine_closed(void);
#endif
static bool lock_fails;
static unsigned commits;
static AppState test_state;
static LocalSettings test_local;
bool lvgl_port_lock(uint32_t timeout) { (void)timeout; return !lock_fails; }
void lvgl_port_unlock(void) {}
static UiSubmitResult submit(void *ctx, const UiRequest *req) {
    (void)ctx;
    assert(req->kind == UI_REQUEST_CONFIRM);
    ++commits;
    return UI_SUBMIT_COMMITTED;
}
KatmedImageStatus katmed_images_present(lv_obj_t *image, lv_obj_t *fallback,
                                        uint32_t id, Orientation orientation) {
    (void)id; (void)orientation;
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(fallback, LV_OBJ_FLAG_HIDDEN);
    return KATMED_IMAGE_OK;
}
static void dispatch(bool down, int x, bool valid, uint64_t ms,
                     bool fail, bool render_due) {
    ClockSample clock = {.monotonic_ms = ms, .domain = 1};
    BoardTouch touch = {.down = down, .x = (int16_t)x, .y = 100,
                        .monotonic_ms = ms};
    lock_fails = fail;
    assert(render_ui(&test_state, &test_local, &clock, &touch,
                     valid, render_due) == !fail);
}
int main(int argc, char **argv) {
    assert(argc == 2);
#ifdef KATMED_REAL_INTERACTION_ADAPTER
    if (strcmp(argv[1], "interaction_real_overflow") == 0) {
        main_engine_start();
        KatmedInteractionInput input={.kind=KATMED_INTERACTION_ENTER,.menu_entry=7};
        assert(interaction_submit(NULL,&input,190));
        interaction_drain(500);
        assert(main_engine_entered());
        main_engine_prepare_grant();
        input.kind=KATMED_INTERACTION_ACK_FINISHED;
        for(unsigned n=0;n<31;++n)assert(interaction_submit(NULL,&input,3150+n));
        input.kind=KATMED_INTERACTION_DOWN;input.contact_id=2;
        assert(interaction_submit(NULL,&input,3200));
        input.kind=KATMED_INTERACTION_UP;
        assert(!interaction_submit(NULL,&input,3380));
        interaction_drain(3500);
        assert(main_engine_closed());
        input.kind=KATMED_INTERACTION_DOWN;input.contact_id=3;
        assert(!interaction_submit(NULL,&input,3510));
        KatmedInteractionStatus status;
        interaction_status(NULL,&status,3525);assert(status.overflow);
        puts("interaction_real_overflow: actual engine closed; poisoned Down blocked; visible loss");
        return 0;
    }
    if (strcmp(argv[1], "interaction_real_adapter") == 0) {
        main_engine_start();
        KatmedInteractionInput input={.kind=KATMED_INTERACTION_ENTER,.menu_entry=7};
        assert(interaction_submit(NULL,&input,190));
        interaction_drain(500);
        assert(main_engine_entered());
        main_engine_prepare_grant();
        input.kind=KATMED_INTERACTION_DOWN;input.contact_id=2;
        assert(interaction_submit(NULL,&input,3200));
        input.kind=KATMED_INTERACTION_UP;
        assert(interaction_submit(NULL,&input,3380));
        interaction_drain(3500);
        assert(main_engine_received_180ms());
        puts("interaction_real_adapter: old queued ENTER accepted at current time; original 180ms delivered");
        return 0;
    }
#endif
    if (strcmp(argv[1], "interaction_overflow") == 0) {
        KatmedInteractionInput enter = {.kind=KATMED_INTERACTION_ENTER, .menu_entry=7};
        assert(interaction_submit(NULL, &enter, 10));
        for (unsigned n=0; n<30; ++n) {
            KatmedInteractionInput ack = {.kind=KATMED_INTERACTION_ACK_FINISHED, .menu_entry=7};
            assert(interaction_submit(NULL, &ack, 20+n));
        }
        KatmedInteractionInput down = {.kind=KATMED_INTERACTION_DOWN, .menu_entry=7, .contact_id=1};
        assert(interaction_submit(NULL, &down, 100));
        KatmedInteractionInput up = down; up.kind=KATMED_INTERACTION_UP;
        assert(!interaction_submit(NULL, &up, 280));
        assert(!interaction_submit(NULL, &down, 290)); /* poisoned old entry stays closed */
        interaction_drain(500);
        assert(transient_count==1);
        assert(transient_inputs[0].kind==KATMED_INTERACTION_LEAVE);
        assert(transient_inputs[0].menu_entry==7);
        KatmedInteractionStatus status;
        runtime.state.alarm_active=true;
        interaction_status(NULL, &status, 500);
        assert(status.overflow);
        runtime.state.alarm_active=false;
        interaction_status(NULL, &status, 500);
        assert(status.overflow); /* retained until a genuine non-alarm UI projection */
        interaction_status(NULL, &status, 500);
        assert(!status.overflow);
        enter.menu_entry=8;
        assert(interaction_submit(NULL, &enter, 510));
        interaction_drain(525);
        assert(transient_count==2 && transient_inputs[1].kind==KATMED_INTERACTION_ENTER);
        puts("interaction_overflow: lost Up closes entry; old Down discarded; visible loss; fresh entry recovers");
        return 0;
    }
    if (strcmp(argv[1], "interaction_queue") == 0) {
        const uint64_t edges[] = {10,190,210,240,260,300,330,350};
        for (unsigned n = 0; n < 8; ++n) {
            KatmedInteractionInput input = {.kind = n%2 ?
                KATMED_INTERACTION_UP : KATMED_INTERACTION_DOWN,
                .menu_entry = 7, .contact_id = n/2+1};
            assert(interaction_submit(NULL, &input, edges[n]));
        }
        assert(transient_count == 0); /* LVGL callbacks never invoke Mesh. */
        interaction_drain(500);
        assert(transient_count == 8);
        for (unsigned n = 0; n < 8; ++n) {
            assert(transient_times[n] == 500);
            assert(transient_inputs[n].contact_time_valid);
            assert(transient_inputs[n].contact_time_ms == edges[n]);
            assert(transient_inputs[n].contact_id == n/2+1);
        }
        KatmedInteractionInput down = {.kind = KATMED_INTERACTION_DOWN,
            .menu_entry = 7, .contact_id = 5};
        assert(interaction_submit(NULL, &down, 600));
        runtime.state.alarm_active = true;
        interaction_drain(625);
        assert(transient_count == 9);
        assert(transient_inputs[8].kind == KATMED_INTERACTION_ALARM);
        assert(transient_inputs[8].active);
        puts("interaction_queue: four contacts/eight edges retained; application-loop-only; alarm before old Down");
        return 0;
    }
    bool movement = strcmp(argv[1], "lost_move") == 0;
    bool round_switch = strcmp(argv[1], "round_switch") == 0;
    bool round_switch_due = strcmp(argv[1], "round_switch_due") == 0;
    assert(movement || round_switch || round_switch_due ||
           strcmp(argv[1], "lost_error") == 0);
    lv_init();
    lv_display_t *display = lv_display_create(240, 240);
    assert(display);
    UiServices services = {.submit = submit};
    ui_set_services(&services);
    test_state.alarm_active = true;
    test_state.round.origin = (EventId){1, 1, 1};
    test_state.round.ordinal = 1;
    test_local.clock_style = DIGITAL_LARGE;
    ClockSample clock = {.domain = 1};
    ui_render(&test_state, &test_local, ALARM, &clock);
    dispatch(false, 90, true, 0, false, false);
    dispatch(true, 90, true, 10, false, false);
    if (round_switch || round_switch_due) {
        test_state.round.ordinal = 2;
        dispatch(false, 90, true, 110, false, round_switch_due);
        assert(commits == 0);
        dispatch(true, 90, true, 200, false, false);
        dispatch(false, 90, true, 300, false, false);
        assert(commits == 1);
        printf("%s: old_round=0 fresh_round=1\n", argv[1]);
        lv_display_delete(display);
        lv_deinit();
        return 0;
    }
    dispatch(movement, 170, movement, 50, true, false);
    dispatch(true, 90, true, 80, false, false);
    dispatch(false, 90, true, 110, false, false);
    assert(commits == 0);
    dispatch(true, 90, true, 200, false, false);
    dispatch(false, 90, true, 300, false, false);
    assert(commits == 1);
    dispatch(true, 90, true, 400, false, false);
    dispatch(false, 90, true, 500, false, false);
    assert(commits == 1);
    printf("%s: old_contact=0 fresh_contact=1 repeated_contact=1\n", argv[1]);
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
