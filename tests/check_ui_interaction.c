#define main original_surface_main
#include "check_ui_surface.c"
#undef main

static KatmedInteractionStatus interaction;
static KatmedInteractionInput inputs[128];
static unsigned input_count, consumed;
static bool fake_input(void *ctx, const KatmedInteractionInput *input, uint64_t now)
{
    (void)ctx; (void)now;
    assert(input_count < 128);
    inputs[input_count++] = *input;
    if (input->kind == KATMED_INTERACTION_ENTER) interaction.menu_entry = input->menu_entry;
    if (input->kind == KATMED_INTERACTION_DISMISS_GREETING && input->target_id == interaction.greeting_id)
        interaction.greeting_visible = false;
    if (input->kind == KATMED_INTERACTION_CONSUME_IMPULSE) {
        assert(interaction.impulse_count);
        assert(input->impulse_id == interaction.impulses[0].impulse_id);
        ++consumed;
        --interaction.impulse_count;
        memmove(interaction.impulses, interaction.impulses+1,
                interaction.impulse_count*sizeof(interaction.impulses[0]));
    }
    return true;
}
static void fake_status(void *ctx, KatmedInteractionStatus *out, uint64_t now)
{ (void)ctx; (void)now; *out = interaction; }
static void vertical(int y0, int y1, uint64_t t)
{
    ui_touch_sample(false, 0, 0, true, t);
    ui_touch_sample(true, 120, y0, true, t+10);
    ui_touch_sample(true, 120, y1, true, t+100);
    ui_touch_sample(false, 0, 0, true, t+150);
}
static void project(AppState *state, LocalSettings *local, ClockSample *clock, uint64_t at)
{ clock->monotonic_ms = at; ui_render(state, local, ui_current_view(), clock); draw(); }
static void tap(int x, int y, uint64_t at)
{
    ui_touch_sample(false, 0, 0, true, at);
    ui_touch_sample(true, x, y, true, at+10);
    ui_touch_sample(false, 0, 0, true, at+100);
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    Host host = {.width = atoi(argv[1]), .height = atoi(argv[2]), .result = UI_SUBMIT_COMMITTED};
    host.frame = calloc((size_t)host.width*host.height, 2);
    uint16_t *buffer = calloc((size_t)host.width*16, 2);
    lv_init();
    lv_display_t *display = lv_display_create(host.width, host.height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, host.width*16u*2u, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_user_data(display, &host);
    UiServices services = {.context = &host, .submit = submit,
        .interaction_input = fake_input, .interaction_status = fake_status};
    ui_set_services(&services);
    AppState state = {0};
    LocalSettings local = {.clock_style = ANALOG};
    ClockSample clock = {.domain = 1, .quality = CLOCK_UNAVAILABLE, .monotonic_ms = 1000};
    interaction = (KatmedInteractionStatus){.connected = true, .synchronized = true,
        .can_request = true, .authority_id = 9, .authority_generation = 1};
    ui_render(&state, &local, BACKGROUND, &clock);
    vertical(80, 140, 1100); /* inner vertical works on normal pages */
    assert(ui_current_view() == UI_VIEW_PLACEHOLDER);
    project(&state, &local, &clock, 1300);
    assert(find_label(lv_screen_active(), "Nur im Notfall. Drücken kostet!"));
    tap(host.width/2, host.height/2, 1400);
    assert(host.submit_calls == 0);
    ui_render(&state, &local, SETTINGS, &clock);
    vertical(80, 140, 1600); /* inner list scroll cannot change page */
    assert(ui_current_view() == SETTINGS);
    vertical(10, 80, 1800);
    assert(ui_current_view() == UI_VIEW_PLACEHOLDER);
    ui_render(&state, &local, SETTINGS, &clock);
    click_label(lv_screen_active(), "Morsecode");
    project(&state, &local, &clock, 2000);
    ui_touch_sample(false, 0, 0, true, 2010); /* consume menu opening release */
    lv_obj_t *send = lv_obj_get_parent(find_label(lv_screen_active(), "Senden"));
    int diameter = lv_obj_get_width(send), x = lv_obj_get_x(send), y = lv_obj_get_y(send);
    int edge = host.height/10; if (edge < 16) edge = 16; if (edge > 32) edge = 32;
    assert(y >= edge && y+diameter <= host.height-edge);
    if (host.width == 240 && host.height == 240) {
        int cx = x+diameter/2-120, cy = y+diameter/2-120;
        assert(sqrt((double)(cx*cx+cy*cy))+diameter/2 <= 120);
    }
    unsigned before = input_count;
    tap(x+diameter/2, y+diameter/2, 2100);
    assert(input_count == before+2);
    assert(inputs[before].kind == KATMED_INTERACTION_DOWN);
    assert(inputs[before+1].kind == KATMED_INTERACTION_UP);
    assert(inputs[before].contact_id == inputs[before+1].contact_id);
    tap(x+diameter/2, y+diameter/2, 2300);
    assert(inputs[before+2].contact_id != inputs[before].contact_id);
    assert(!ui_lvgl_touch_allowed(true, x+diameter/2, y+diameter/2, true, 2400));
    ui_lvgl_touch_allowed(false, 0, 0, true, 2410);
    /* A held underlying contact cannot dismiss a newly appearing greeting. */
    ui_touch_sample(true, host.width/2, 80, true, 2500);
    interaction.greeting_visible = true; interaction.greeting_id = 7;
    interaction.greeting_until_ms = 5000;
    project(&state, &local, &clock, 2550);
    before = input_count;
    ui_touch_sample(false, 0, 0, true, 2600);
    assert(input_count == before);
    assert(!ui_lvgl_touch_allowed(true, host.width/2, 10, true, 2650));
    ui_lvgl_touch_allowed(false, 0, 0, true, 2660);
    /* Even a queued Down older than the new overlay cannot acknowledge it. */
    before = input_count;
    ui_touch_sample(true, host.width/2, host.height/2, true, 2540);
    ui_touch_sample(false, 0, 0, true, 2690);
    assert(input_count == before);
    tap(host.width/2, host.height/2, 2700);
    assert(inputs[input_count-1].kind == KATMED_INTERACTION_DISMISS_GREETING);
    project(&state, &local, &clock, 2850);
    /* A greeting above Seltsam consumes its tap without the local exit. */
    ui_render(&state, &local, SETTINGS, &clock);
    click_label(lv_screen_active(), "Seltsam");
    project(&state, &local, &clock, 2870);
    ui_touch_sample(false, 0, 0, true, 2880);
    interaction.greeting_visible = true; interaction.greeting_id = 8;
    interaction.greeting_until_ms = 5000;
    project(&state, &local, &clock, 2890);
    tap(host.width/2, host.height/2, 2900);
    assert(ui_current_view() == MORSE);
    project(&state, &local, &clock, 3010);
    ui_touch_sample(true, host.width/2, host.height/2, true, 3020);
    assert(ui_current_view() == BACKGROUND);
    ui_touch_sample(false, 0, 0, true, 3030);
    /* Expiry consumes a held modal contact; it never turns into a swipe. */
    interaction.greeting_visible = true; interaction.greeting_id = 9;
    interaction.greeting_until_ms = 3200;
    project(&state, &local, &clock, 3100);
    ui_touch_sample(true, host.width/2, 10, true, 3110);
    project(&state, &local, &clock, 3210);
    ui_touch_sample(true, host.width/2, 80, true, 3220);
    ui_touch_sample(false, 0, 0, true, 3240);
    assert(ui_current_view() == BACKGROUND);
    interaction.greeting_visible = false;
    /* Four completed impulses survive a single 0/500ms projection gap. */
    ui_render(&state, &local, BACKGROUND, &clock);
    interaction.impulse_count = 4;
    for (unsigned n = 0; n < 4; ++n)
        interaction.impulses[n] = (KatmedInteractionImpulse){.grant_id = 4,
            .impulse_id = n+1, .duration_ms = 180, .valid_until_ms = 10000};
    for (unsigned n = 0; n < 4; ++n) {
        uint64_t at = 3300+n*250;
        project(&state, &local, &clock, at);
        assert(host.frame[host.width*(host.height/2)+host.width/2] == 0xffff);
        project(&state, &local, &clock, at+180);
        assert(host.frame[host.width*(host.height/2)+host.width/2] != 0xffff);
    }
    assert(consumed == 4);
    assert(ui_current_view() == BACKGROUND);
    interaction.impulse_count = 1;
    interaction.impulses[0] = (KatmedInteractionImpulse){.grant_id = 5,
        .impulse_id = 99, .duration_ms = 180, .valid_until_ms = 10000};
    project(&state, &local, &clock, 4400);
    assert(host.frame[host.width*(host.height/2)+host.width/2] == 0xffff);
    ++interaction.authority_generation;
    project(&state, &local, &clock, 4450);
    assert(host.frame[host.width*(host.height/2)+host.width/2] != 0xffff);
    /* Reception is visible on every other normal page, preserves its view. */
    UiView views[] = {CLOCK, SETTINGS, UI_VIEW_STATUS, UI_VIEW_PLACEHOLDER};
    for (unsigned n = 0; n < 4; ++n) {
        interaction.receive_white = true; interaction.receive_until_ms = 20000;
        ui_render(&state, &local, views[n], &clock); draw();
        assert(ui_current_view() == views[n]);
        assert(host.frame[host.width*(host.height/2)+host.width/2] == 0xffff);
        interaction.receive_white = false;
        project(&state, &local, &clock, 5000+n*100);
    }
    ui_render(&state, &local, SETTINGS, &clock);
    click_label(lv_screen_active(), "Morsecode");
    project(&state, &local, &clock, 5600);
    ui_touch_sample(false, 0, 0, true, 5610);
    send = lv_obj_get_parent(find_label(lv_screen_active(), "Senden"));
    x = lv_obj_get_x(send); y = lv_obj_get_y(send); diameter = lv_obj_get_width(send);
    ui_touch_sample(true, x+diameter/2, y+diameter/2, true, 5700);
    state.alarm_active = true;
    state.round.origin = (EventId){1,1,1}; state.round.ordinal = 1;
    interaction.receive_white = true; interaction.receive_until_ms = 20000;
    project(&state, &local, &clock, 6000);
    assert(ui_current_view() == ALARM);
    assert(host.frame[host.width*5+host.width/2] != 0xffff);
    assert(host.submit_calls == 0);
    assert(inputs[input_count-1].kind == KATMED_INTERACTION_CANCEL);
    ui_touch_sample(false, 0, 0, true, 6050);
    assert(host.submit_calls == 0); /* pre-alarm Down cannot confirm */
    state.alarm_active = false;
    project(&state, &local, &clock, 6100);
    assert(ui_current_view() == UI_VIEW_INTERACTION);
    before = input_count;
    ui_touch_sample(false, 0, 0, true, 6110);
    assert(input_count == before); /* return does not resume an old transmission */
    vertical(10, 80, 6200);
    assert(ui_current_view()==UI_VIEW_PLACEHOLDER);
    vertical(80, 10, 6400);
    assert(ui_current_view()==SETTINGS); /* logical menu column survives alarm */
    interaction.overflow=true;
    project(&state, &local, &clock, 6600);
    lv_obj_t *loss=find_label(lv_screen_active(), "Impulse: Speicher voll");
    assert(loss && !lv_obj_has_flag(loss, LV_OBJ_FLAG_HIDDEN));
    lv_display_delete(display); lv_deinit(); free(buffer); free(host.frame);
    return 0;
}
