/* Existing actual LVGL/UI/Main/Core/Store path; only storage and Mesh I/O are doubles. */
#define SETTINGS_VALUES_ENTRYPOINT existing_settings_values_regression
#include "check_settings_values.c"
#undef SETTINGS_VALUES_ENTRYPOINT

static void dismiss_start(void)
{
    unsigned before=writes, notifications=fake_mesh_notifications;
    UiRequest dismiss={.kind=UI_REQUEST_DISMISS_BOOT};
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_DISMISSED);
    assert(writes==before && fake_mesh_notifications==notifications);
    assert(!runtime.state.has_last_confirmation &&
           runtime.state.deadline.effective_monotonic_ms==0);
}

int main(int argc,char **argv)
{
    assert(argc==4);
    int width=atoi(argv[1]),height=atoi(argv[2]);
    fake_microseconds=1000000;
    initialize_runtime();
    assert(!runtime.mesh_started && !runtime.store.record.provision.configured);
    assert(runtime.store.record.group_origin==STORE_GROUP_ORIGIN_FRESH_UNBOUND);
    assert(!clock_sample(runtime.boot_domain).utc_valid);
    dismiss_start();
    lv_init();lv_display_t *display=lv_display_create(width,height);assert(display);
    UiServices services={.context=&runtime,.submit=submit_ui,.read_model=read_ui_model};
    ui_set_services(&services);katmed_i18n_select(KATMED_LANG_EN);
    render();click("Advanced");
    if(!strcmp(argv[3],"order")) {
        const char *texts[]={"Advanced","Medication: hours","Images: hours",
                             "Next due date HH:MM","Language","English"};
        lv_obj_update_layout(lv_screen_active());
        int previous=INT32_MIN;
        for(unsigned i=0;i<sizeof(texts)/sizeof(texts[0]);++i) {
            lv_obj_t *label=find_text(lv_screen_active(),texts[i]);assert(label);
            lv_area_t area;lv_obj_get_coords(label,&area);
            printf("category_order[%u]=%s y=%ld\n",i,texts[i],(long)area.y1);
            if(area.y1<=previous) {
                fprintf(stderr,"Advanced controls must precede the separate Language section\n");
                return 1;
            }
            previous=area.y1;
        }
    } else {
        assert(!strcmp(argv[3],"save"));
        click("Images: hours");expect_text("6");
        const unsigned before=writes, before_mesh=fake_shared_calls;
        const Deadline deadline=runtime.state.deadline;
        const uint32_t medication=runtime.state.settings.medication_seconds;
        keys("<8");click("Save");
        printf("offline_save requested=28800 active=%lu persisted=%lu writes=%u mesh=%u\n",
            (unsigned long)runtime.state.settings.image_seconds,
            (unsigned long)runtime.store.record.shared.image_seconds,writes-before,
            fake_shared_calls-before_mesh);fflush(stdout);
        if(runtime.state.settings.image_seconds!=28800 ||
           runtime.store.record.shared.image_seconds!=28800 || writes<=before) return 1;
        expect_text("Saved");
        assert(runtime.state.settings.medication_seconds==medication &&
               memcmp(&runtime.state.deadline,&deadline,sizeof(deadline))==0 &&
               !runtime.state.has_last_confirmation &&
               !runtime.store.record.last_confirmation.valid &&
               runtime.store.record.pending_count==0 &&
               fake_shared_calls==before_mesh && fake_mesh_notifications==0);
        render();click("Images: hours");expect_text("8");click("Cancel");
        initialize_runtime(); /* Actual committed blob decode, new boot identity. */
        assert(runtime.state.settings.image_seconds==28800 &&
               !runtime.mesh_started && !clock_sample(runtime.boot_domain).utc_valid);
        dismiss_start();render();click("Images: hours");expect_text("8");click("Cancel");

        /* The same rejected pre-Mesh path serves local interval/rotation
         * requests. Keep the genuine confirmation and no-UTC time guards. */
        UiRequest medication_request={.kind=UI_REQUEST_SET_MEDICATION_SECONDS,.value.seconds=3600};
        assert(submit_ui(&runtime,&medication_request)==UI_SUBMIT_COMMITTED);
        assert(runtime.state.settings.medication_seconds==3600 &&
               runtime.store.record.shared.medication_seconds==3600 &&
               !runtime.state.has_last_confirmation &&
               !runtime.store.record.last_confirmation.valid &&
               runtime.store.record.pending_count==0);
        UiRequest rotation={.kind=UI_REQUEST_SET_ROTATION_MODE,.value.rotation=ROTATION_OFF};
        assert(submit_ui(&runtime,&rotation)==UI_SUBMIT_COMMITTED);
        assert(runtime.store.record.shared.rotation==ROTATION_OFF);
        UiRequest no_time={.kind=UI_REQUEST_SET_NEXT_LOCAL_TIME};
        const AppState before_invalid=runtime.state;
        const StoreManualIntent intent_before=runtime.store.record.manual_intent;
        assert(submit_ui(&runtime,&no_time)==UI_SUBMIT_UNAVAILABLE);
        /* The existing Store may reserve event-ID metadata before rejection;
         * no invalid due time or medication state may be committed. */
        assert(memcmp(&before_invalid,&runtime.state,sizeof(before_invalid))==0 &&
               memcmp(&intent_before,&runtime.store.record.manual_intent,sizeof(intent_before))==0);
        assert(fake_shared_calls==before_mesh && fake_mesh_notifications==0);

        /* No offline override for a bound/reset or provenance-unknown group,
         * an in-flight shared change, or unavailable/uncertain storage. */
        UiRequest image={.kind=UI_REQUEST_SET_IMAGE_SECONDS,.value.seconds=32400};
        const unsigned saved_writes=writes;
        runtime.shared_pending=true;
        assert(submit_ui(&runtime,&image)==UI_SUBMIT_UNAVAILABLE);runtime.shared_pending=false;
        const uint64_t self=runtime.state.revision.coordinator;
        runtime.state.revision.coordinator=self+1;
        assert(submit_ui(&runtime,&image)==UI_SUBMIT_UNAVAILABLE);
        runtime.state.revision.coordinator=self;
        runtime.store.record.group_origin=STORE_GROUP_ORIGIN_UNKNOWN;
        assert(submit_ui(&runtime,&image)==UI_SUBMIT_UNAVAILABLE);
        runtime.store.record.group_origin=STORE_GROUP_ORIGIN_FIRST_BOUND;
        runtime.store.record.bound_group[0]=1;
        assert(submit_ui(&runtime,&image)==UI_SUBMIT_UNAVAILABLE);
        runtime.store.record.bound_group[0]=0;
        runtime.store.record.group_origin=STORE_GROUP_ORIGIN_FRESH_UNBOUND;
        runtime.store.record.provision.configured=true;
        assert(submit_ui(&runtime,&image)==UI_SUBMIT_UNAVAILABLE);
        runtime.store.record.provision.configured=false;
        assert(writes==saved_writes && fake_shared_calls==before_mesh);
        fail_commit=true;
        const AppState active_before=runtime.state;
        assert(submit_ui(&runtime,&image)==UI_SUBMIT_UNAVAILABLE);
        assert(memcmp(&active_before,&runtime.state,sizeof(active_before))==0);
        assert(submit_ui(&runtime,&image)==UI_SUBMIT_UNAVAILABLE);
        fail_commit=false;initialize_runtime();
        assert(runtime.state.settings.image_seconds==28800);
        assert(!runtime.store.record.last_confirmation.valid &&
               runtime.store.record.pending_count==0 && fake_mesh_notifications==0);
        /* Valid UTC still must not turn this duration-only route into a new
         * unbound manual-deadline/restore authority. */
        dismiss_start();
        katmed_time_init(&runtime.time,runtime.boot_domain);
        assert(katmed_time_accept_utc(&runtime.time,1000,INT64_C(1792400400000),CLOCK_SYNCHRONIZED));
        ClockSample known=clock_sample(runtime.boot_domain);ManualTimePreview preview;
        assert(app_preview_local_time(&known,14,30,&preview)==APP_OK);
        no_time.value.local_time.hour=14;no_time.value.local_time.minute=30;
        no_time.value.local_time.preview=preview;
        unsigned before_manual=writes;
        assert(submit_ui(&runtime,&no_time)==UI_SUBMIT_UNAVAILABLE && writes==before_manual);
        assert(!runtime.store.record.manual_intent.pending);
    }
    lv_display_delete(display);lv_deinit();
    return 0;
}
