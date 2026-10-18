#define main original_surface_main
#include "../check_ui_surface.c"
#undef main

static bool journal_divergent(void *context)
{
    (void)context;
    return true;
}

static uint8_t journal_count(void *context)
{
    (void)context;
    return 2;
}

static bool journal_at(void *context, size_t index, UiConflict *entry)
{
    (void)context;
    if (!entry || index >= 2) return false;
    *entry=(UiConflict){.id={index+99u,5,2},.round={{11,3,1},7},
                        .occurred_at={1500,3,0,false,CLOCK_UNAVAILABLE}};
    if (index==0) {
        entry->occurred_at.utc_ms=1791973800000LL;
        entry->occurred_at.utc_valid=true;
        entry->occurred_at.quality=CLOCK_SYNCHRONIZED;
    }
    return true;
}

int main(void)
{
    Host host={.width=240,.height=240,.result=UI_SUBMIT_PENDING_SHARED};
    host.frame=calloc((size_t)host.width*host.height,sizeof(*host.frame));
    uint16_t *buffer=calloc((size_t)host.width*16,sizeof(*buffer));
    assert(host.frame && buffer);
    lv_init();
    lv_display_t *display=lv_display_create(host.width,host.height);
    assert(display);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,buffer,NULL,(uint32_t)host.width*16u*2u,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    lv_display_set_user_data(display,&host);
    UiServices services={.context=&host,.submit=submit,.connected=connected,
                         .durable_divergence=journal_divergent,
                         .conflict_count=journal_count,.conflict_at=journal_at};
    ui_set_services(&services);
    AppState state={0};
    state.round.origin=(EventId){1,1,1}; state.round.ordinal=1;
    LocalSettings local={.orientation=ORIENTATION_0,.clock_style=DIGITAL_LARGE};
    ClockSample clock={.monotonic_ms=1000,.domain=1,.utc_ms=1791973800000LL,
                       .utc_valid=true,.quality=CLOCK_SYNCHRONIZED};
    ui_render(&state,&local,SETTINGS,&clock);
    lv_obj_t *screen=lv_screen_active();
    click_label(screen,"Medikamente: Stunden");
    enter_keys(screen,"2");
    click_label(screen,"Speichern");
    assert(host.submit_calls==1 && host.last_request.kind==UI_REQUEST_SET_MEDICATION_SECONDS);
    assert(find_label(screen,"Koordinator prüft Änderung"));
    assert(find_label(screen,"Speichern")); /* Entry remains until ACK. */
    ui_shared_result(false);
    assert(find_label(screen,"Koordinator hat Änderung nicht übernommen"));
    assert(find_label(screen,"Speichern"));
    click_label(screen,"Speichern");
    assert(host.submit_calls==2 && find_label(screen,"Koordinator prüft Änderung"));
    ui_shared_result(true);
    assert(find_label(screen,"Gemeinsame Änderung gespeichert"));
    assert(!find_label(screen,"Speichern"));
    click_label(screen,"Medikamente: Stunden");
    enter_keys(screen,"2");
    click_label(screen,"Speichern");
    assert(host.submit_calls==3);
    state.alarm_active=true;
    state.round.ordinal++;
    ui_render(&state,&local,ALARM,&clock);
    ui_shared_result(true);
    lv_obj_t *feedback=find_label(screen,"Koordinator prüft Änderung");
    assert(!feedback || lv_obj_has_flag(feedback,LV_OBJ_FLAG_HIDDEN));
    feedback=find_label(screen,"Gemeinsame Änderung gespeichert");
    assert(!feedback || lv_obj_has_flag(feedback,LV_OBJ_FLAG_HIDDEN));
    state.alarm_active=false;
    ui_render(&state,&local,SETTINGS,&clock);
    ui_render(&state,&local,SETTINGS,&clock);
    click_label(screen,"Status");
    assert(find_label(screen,"Gruppenzustand weicht ab"));
    assert(find_label_prefix(screen,"Nicht übernommen: Runde 11/7, Ereignis 99/2"));
    assert(find_label_prefix(screen,"Nicht übernommen: Runde 11/7, Ereignis 100/2"));
    assert(find_label_prefix(screen,"Ereigniszeit: 14.10.2026"));
    assert(find_label(screen,"Ereigniszeit: UTC unbekannt; Domäne 3, ms 1500"));
    lv_display_delete(display);
    lv_deinit();
    free(buffer);
    free(host.frame);
    puts("UI shared pending/ACK feedback passed");
    return 0;
}
