/* Reuse the real main callback, durable in-memory Store backend and Mesh adapter
 * from the existing submit regression. Only its entry point is renamed. */
#define main existing_submit_regression
#include "check_main_submit.c"
#undef main
#include <stdlib.h>

KatmedImageStatus katmed_images_present(lv_obj_t *image, lv_obj_t *fallback,
                                       uint32_t id, Orientation orientation)
{
    (void)image; (void)fallback; (void)id; (void)orientation;
    return KATMED_IMAGE_OK;
}

static lv_obj_t *find_text(lv_obj_t *node, const char *text)
{
    if (lv_obj_check_type(node, &lv_label_class) &&
        strcmp(lv_label_get_text(node), text) == 0) return node;
    for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i) {
        lv_obj_t *found = find_text(lv_obj_get_child(node, i), text);
        if (found) return found;
    }
    return NULL;
}

static void click(const char *text)
{
    if ((strcmp(text,"Status")==0 && ui_current_view()==SETTINGS) ||
        (strcmp(text,"Zurück")==0 && ui_current_view()==UI_VIEW_STATUS)) {
        static uint64_t tick=10000000;
        tick+=1000;
        int height=lv_display_get_vertical_resolution(lv_display_get_default());
        bool back=ui_current_view()==UI_VIEW_STATUS;
        ui_touch_sample(false,0,0,true,tick);
        ui_touch_sample(true,80,back?1:height-2,true,tick+30);
        ui_touch_sample(true,80,back?100:height-100,true,tick+130);
        ui_touch_sample(false,0,0,true,tick+230);
        return;
    }
    lv_obj_t *caption = find_text(lv_screen_active(), text);
    assert(caption && "expected visible control");
    lv_obj_send_event(lv_obj_get_parent(caption), LV_EVENT_CLICKED, NULL);
}

static bool key_in(lv_obj_t *node, const char *text)
{
    if (lv_obj_check_type(node, &lv_buttonmatrix_class)) {
        for (uint32_t i = 0; i < 16; ++i) {
            const char *key = lv_buttonmatrix_get_button_text(node, i);
            if (key && strcmp(key, text) == 0) {
                lv_buttonmatrix_set_selected_button(node, i);
                lv_obj_send_event(node, LV_EVENT_VALUE_CHANGED, NULL);
                return true;
            }
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(node); ++i)
        if (key_in(lv_obj_get_child(node, i), text)) return true;
    return false;
}

static void keys(const char *text)
{
    for (; *text; ++text) {
        char key[2] = {*text, 0};
        assert(key_in(lv_screen_active(), key));
    }
}

static void render(void)
{
    ClockSample now = clock_sample(runtime.boot_domain);
    ui_render(&runtime.state, &runtime.local, SETTINGS, &now);
    lv_obj_update_layout(lv_screen_active());
}

static void expect_text(const char *text)
{
    lv_obj_t *label = find_text(lv_screen_active(), text);
    if (!label) fprintf(stderr, "missing visible model value: %s\n", text);
    assert(label && "configured model value must be drawn");
    assert(!lv_obj_is_hidden(label));
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    const int width = atoi(argv[1]), height = atoi(argv[2]);
    fake_microseconds = 1000000;
    initialize_runtime();
    runtime.mesh_started = true;
    fake_mesh_coordinator = true;
    fake_mesh_connected = false;
    fake_mesh_result = MESH_ACCEPTED;
    katmed_time_init(&runtime.time, runtime.boot_domain);
    assert(katmed_time_accept_utc(&runtime.time, 1000, INT64_C(1792400400000),
                                  CLOCK_SYNCHRONIZED));
    assert(submit_due_confirm() == UI_SUBMIT_PENDING);
    UiRequest image = {.kind=UI_REQUEST_SET_IMAGE_SECONDS};
    image.value.seconds = 5400;
    assert(submit_ui(&runtime, &image) == UI_SUBMIT_COMMITTED);
    assert(runtime.store.record.shared.image_seconds == 5400);
    initialize_runtime(); /* Decode the committed blob before the UI consumes it. */
    assert(runtime.state.settings.image_seconds == 5400);
    runtime.mesh_started = true;
    katmed_time_init(&runtime.time, runtime.boot_domain);
    assert(katmed_time_accept_utc(&runtime.time, 1000, INT64_C(1792400400000),
                                  CLOCK_SYNCHRONIZED));
    assert(submit_due_confirm() == UI_SUBMIT_PENDING);
    UiRequest medication = {.kind=UI_REQUEST_SET_MEDICATION_SECONDS};
    medication.value.seconds = 7200;
    assert(submit_ui(&runtime, &medication) == UI_SUBMIT_COMMITTED);
    assert(runtime.state.settings.medication_seconds == 7200 &&
           runtime.state.settings.image_seconds == 5400);

    lv_init();
    lv_display_t *display = lv_display_create(width, height);
    assert(display);
    UiServices services = {.context=&runtime, .submit=submit_ui};
    ui_set_services(&services);
    render();
    click("Advanced");
    const AppState before_medication_open = runtime.state;
    const unsigned medication_open_writes = writes;
    const unsigned medication_open_submits = fake_shared_calls;
    click("Medikamente: Stunden");
    expect_text("2"); /* Medication interval, distinct from image interval 1.5h. */
    assert(writes == medication_open_writes &&
           fake_shared_calls == medication_open_submits &&
           memcmp(&before_medication_open, &runtime.state, sizeof(AppState)) == 0);
    keys("<3.5");
    click("Speichern");
    assert(runtime.state.settings.medication_seconds == 12600 &&
           runtime.store.record.shared.medication_seconds == 12600 &&
           runtime.state.settings.image_seconds == 5400);
    render();
    click("Medikamente: Stunden");
    expect_text("3.5");
    click("Abbrechen");
    const unsigned before_open = writes;
    const unsigned submissions_before_open = fake_shared_calls;
    const AppState state_before_open = runtime.state;
    click("Bilder: Stunden");
    expect_text("1.5"); /* Real Store -> AppState -> ui_render -> LVGL label. */
    assert(writes == before_open && fake_shared_calls == submissions_before_open &&
           memcmp(&state_before_open, &runtime.state, sizeof(AppState)) == 0);
    keys("<<<2.25");
    click("Speichern"); /* Actual production submit_ui + Core + durable Store. */
    assert(runtime.state.settings.image_seconds == 8100 &&
           runtime.store.record.shared.image_seconds == 8100);
    expect_text("Gespeichert");
    render(); /* Same producer transition as the board refresh. */
    click("Bilder: Stunden");
    expect_text("2.25");
    click("Abbrechen");

    /* Arbitrary second-resolution values must round-trip through hours. */
    image.value.seconds = 3661;
    assert(submit_ui(&runtime, &image) == UI_SUBMIT_COMMITTED);
    render();
    click("Bilder: Stunden");
    expect_text("1.016944");
    click("Speichern");
    assert(runtime.state.settings.image_seconds == 3661);

    image.value.seconds = UINT32_MAX;
    assert(submit_ui(&runtime, &image) == UI_SUBMIT_COMMITTED);
    render();
    click("Bilder: Stunden");
    expect_text("1193046.470833");
    lv_obj_update_layout(lv_screen_active());
    lv_area_t input_area, preview_area;
    lv_obj_get_coords(find_text(lv_screen_active(), "1193046.470833"), &input_area);
    lv_obj_get_coords(find_text(lv_screen_active(), "Bildwechsel: 4294967295 Sekunden"),
                      &preview_area);
    assert(input_area.y2 < preview_area.y1 && "prefilled value must not cover preview");
    click("Speichern");
    assert(runtime.state.settings.image_seconds == UINT32_MAX);
    image.value.seconds = 3661;
    assert(submit_ui(&runtime, &image) == UI_SUBMIT_COMMITTED);

    ClockSample now = clock_sample(runtime.boot_domain);
    ManualTimePreview preview;
    assert(app_preview_local_time(&now, 14, 30, &preview) == APP_OK);
    UiRequest next = {.kind=UI_REQUEST_SET_NEXT_LOCAL_TIME};
    next.value.local_time.hour = 14;
    next.value.local_time.minute = 30;
    next.value.local_time.preview = preview;
    assert(submit_ui(&runtime, &next) == UI_SUBMIT_COMMITTED);
    render();
    const unsigned before_time_open = writes;
    click("Nächste Fälligkeit HH:MM");
    expect_text("14:30");
    assert(writes == before_time_open);
    keys("<<<<<1545");
    click("Speichern");
    render();
    click("Nächste Fälligkeit HH:MM");
    expect_text("15:45");
    click("Abbrechen");
    click("Status");
    expect_text("Bilderabstand: 1.016944 Stunden");
    expect_text("Nächster Alarm: 19.10.2026 15:45 CEST");
    expect_text("WLAN: unbekannt");
    expect_text("IP: nicht verfügbar");
    expect_text("Gateway: nicht verfügbar");
    expect_text("DNS-IP: unbekannt");
    expect_text("Zeitserver-IP (letzter lokaler Sync): unbekannt");
    expect_text("Mesh aktuell (inkl. Gerät): inaktiv");
    expect_text("Mesh bisher erfasst (ab Einführung): inaktiv");
    click("Zurück");

    runtime.state.deadline.effective_utc.valid = false;
    render();
    const unsigned before_unknown_open = writes;
    click("Nächste Fälligkeit HH:MM");
    expect_text("_");
    assert(writes == before_unknown_open);
    click("Abbrechen");
    click("Status");
    expect_text("Nächster Alarm: Zeit nicht verfügbar");
    click("Zurück");
    runtime.state.deadline.effective_utc = (UtcInstant){true, INT64_MAX};
    render();
    click("Nächste Fälligkeit HH:MM");
    expect_text("_");
    click("Abbrechen");
    click("Status");
    expect_text("Nächster Alarm: Zeit nicht verfügbar");
    click("Zurück");
    runtime.state.deadline.effective_utc = (UtcInstant){true, INT64_C(1792891800000)};
    render();
    click("Status");
    expect_text("Nächster Alarm: 25.10.2026 02:30 CET");
    lv_obj_t *panel = lv_obj_get_parent(find_text(lv_screen_active(), "Status"));
    assert(lv_obj_is_scrollable(panel));
    assert(!find_text(panel, "Zurück"));
    lv_obj_t *last = find_text(panel, "Mesh bisher erfasst (ab Einführung): inaktiv");
    assert(last);
    lv_obj_scroll_to_view(last, LV_ANIM_OFF);
    lv_obj_update_layout(panel);
    lv_area_t visible_last, panel_bounds;
    lv_obj_get_coords(last,&visible_last);
    lv_obj_get_coords(panel,&panel_bounds);
    assert(visible_last.y1>=panel_bounds.y1&&visible_last.y2<=panel_bounds.y2);
    click("Zurück");
    printf("settings values: real Store/save/reopen/status/unknown %dx%d passed\n",
           width, height);
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
