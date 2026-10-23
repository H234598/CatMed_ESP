/* Existing actual LVGL/UI/Main/Core/Store path; only storage and Mesh I/O are doubles. */
#define SETTINGS_VALUES_ENTRYPOINT existing_settings_values_regression
#include "check_settings_values.c"
#undef SETTINGS_VALUES_ENTRYPOINT
#include "src/misc/lv_text_ap.h"

void katmed_mesh_device_name(char *out,size_t capacity,bool *assigned,bool *uncertain) {
    if(capacity) {snprintf(out,capacity,"KatmedESP");}
    *assigned=false;*uncertain=false;
}
bool katmed_mesh_radio_channel(uint8_t *out){*out=0;return false;}
void katmed_mesh_counts(KatmedMeshCounts *out){memset(out,0,sizeof(*out));}
uint32_t katmed_time_idf_sync_server_ipv4(void){return 0;}

static void dismiss_start(void)
{
    unsigned before=writes, notifications=fake_mesh_notifications;
    UiRequest dismiss={.kind=UI_REQUEST_DISMISS_BOOT};
    assert(submit_ui(&runtime,&dismiss)==UI_SUBMIT_DISMISSED);
    assert(writes==before && fake_mesh_notifications==notifications);
    assert(!runtime.state.has_last_confirmation &&
           runtime.state.deadline.effective_monotonic_ms==0);
}

static void menu_right_swipe(void)
{
    static uint64_t tick=UINT64_C(10000000);tick+=1000;
    ui_touch_sample(false,0,0,true,tick);
    ui_touch_sample(true,60,90,true,tick+30);
    ui_touch_sample(true,155,90,true,tick+130);
    ui_touch_sample(false,0,0,true,tick+230);
    lv_obj_update_layout(lv_screen_active());
}
static lv_obj_t *menu_matrix(lv_obj_t *node)
{
    if(lv_obj_check_type(node,&lv_buttonmatrix_class))return node;
    for(uint32_t i=0;i<lv_obj_get_child_count(node);++i){
        lv_obj_t *found=menu_matrix(lv_obj_get_child(node,i));if(found)return found;
    }
    return NULL;
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
    UiServices services={.context=&runtime,.submit=submit_ui,.read_model=read_ui_model,.network_status=network_status};
    ui_set_services(&services);katmed_i18n_select(KATMED_LANG_EN);
    render();
    if(!strcmp(argv[3],"manual-clock")) {
        katmed_time_init(&runtime.time,runtime.boot_domain);
        StoreProvision offline={.configured=true,.network_mode=STORE_NETWORK_OFFLINE};
        assert(store_save_setup(&runtime.store,&offline)==STORE_OK);
        const bool had_dose=runtime.store.record.last_confirmation.valid;
        render(); // actual production network_status -> automatic missing-clock prompt
        expect_text("YYYYMMDDHHMM Berlin");
        const unsigned before=writes;
        keys("202603290230");click("Save");
        assert(!clock_sample(runtime.boot_domain).utc_valid && writes==before && !fake_wall_sets);
        keys("<<<<<<<<<<<<202610231425");expect_text("2026-10-23T14:25");click("Save");
        ClockSample manual=clock_sample(runtime.boot_domain);
        assert(manual.utc_valid && manual.quality==CLOCK_HOLDOVER && runtime.utc_source==3 && fake_wall_sets==1);
        assert(runtime.store.record.last_confirmation.valid==had_dose && !runtime.state.has_last_confirmation && !runtime.store.record.setup_requested && !runtime.setup_ap);
        assert(!time_shareable(&runtime));
        return 0;
    }
    if(!strcmp(argv[3],"order")) {
        const char *root_labels[]={"Intervalls","Times","Languages","Rotation",
                                  "Slideshow / playback order","Setup","Date and time","Open setup","Reset","Factory reset"};
        int previous=INT32_MIN;
        for(unsigned i=0;i<sizeof(root_labels)/sizeof(root_labels[0]);++i) {
            lv_obj_t *label=find_text(lv_screen_active(),root_labels[i]);
            if(!label) {fprintf(stderr,"missing new menu: %s\n",root_labels[i]);return 1;}
            lv_area_t area;lv_obj_get_coords(label,&area);
            if(area.y1<=previous)return 1;
            previous=area.y1;
        }
        /* Each existing root control owns exactly one immediately preceding
         * direct-child category; captions inside buttons are not headings. */
        lv_obj_t *categories=lv_obj_get_parent(find_text(lv_screen_active(),"Intervalls"));
        const char *headings[]={"Intervalls","Languages","Rotation",
            "Slideshow / playback order","Clock design","Images","Reset"};
        const char *controls[]={"Times","Languages","Rotation",
            "Slideshow / playback order","Clock design",
            "Load images from URL/Github\nAvailable later","Factory reset"};
        unsigned section=0;
        for(uint32_t i=0;i<lv_obj_get_child_count(categories);++i) {
            lv_obj_t *heading=lv_obj_get_child(categories,i);
            if(!lv_obj_check_type(heading,&lv_label_class) ||
               strcmp(lv_label_get_text(heading),"Intervalls"))continue;
            for(;section<6;++section,i+=2) {
                heading=lv_obj_get_child(categories,i);
                assert(lv_obj_check_type(heading,&lv_label_class));
                assert(!strcmp(lv_label_get_text(heading),headings[section]));
                lv_obj_t *control=lv_obj_get_child(categories,i+1);
                assert(lv_obj_check_type(control,&lv_button_class));
                assert(find_text(control,controls[section]));
                assert(lv_obj_has_state(control,LV_STATE_DISABLED)==(section==5));
            }
            assert(find_text(lv_obj_get_child(categories,i),"Setup"));
            assert(find_text(lv_obj_get_child(categories,i+1),"Date and time"));
            assert(find_text(lv_obj_get_child(categories,i+2),"Open setup"));
            i+=3;
            assert(find_text(lv_obj_get_child(categories,i),"Reset"));
            assert(find_text(lv_obj_get_child(categories,i+1),"Factory reset"));
            i+=2;section=7;
            assert(i==lv_obj_get_child_count(categories));
            break;
        }
        assert(section==7);
        UiRequest image={.kind=UI_REQUEST_SET_IMAGE_SECONDS,.value.seconds=28800};
        assert(submit_ui(&runtime,&image)==UI_SUBMIT_COMMITTED);render();
        const unsigned before=writes,mesh_before=fake_shared_calls;
        const AppState state_before=runtime.state;
        const StoreRecord record_before=runtime.store.record;
        lv_obj_t *main_menu=lv_obj_get_parent(find_text(lv_screen_active(),"Intervalls"));
        lv_obj_update_layout(main_menu);lv_obj_scroll_to_y(main_menu,70,LV_ANIM_OFF);
        int main_scroll=lv_obj_get_scroll_y(main_menu);
        click("Times");lv_obj_update_layout(lv_screen_active());
        const char *times[]={"Times","Medication: hours","Images: hours","Next due date HH:MM"};
        previous=INT32_MIN;
        for(unsigned i=0;i<4;++i) {
            lv_obj_t *label=find_text(lv_screen_active(),times[i]);assert(label);
            lv_area_t area;lv_obj_get_coords(label,&area);assert(area.y1>previous);previous=area.y1;
        }
        assert(!find_text(lv_screen_active(),"English")&&!find_text(lv_screen_active(),"0°"));
        click("Images: hours");expect_text("8");menu_right_swipe();
        assert(find_text(lv_screen_active(),"Images: hours")&&!find_text(lv_screen_active(),"Save"));
        menu_right_swipe();
        main_menu=lv_obj_get_parent(find_text(lv_screen_active(),"Intervalls"));
        assert(lv_obj_get_scroll_y(main_menu)==main_scroll);
        click("Languages");
        const char *languages[]={"English","Deutsch","Français","Nederlands","Українська",
            "العربية","简体中文","Español","日本語","Ostfränkisch","Esperanto","tlhIngan Hol"};
        for(unsigned i=0;i<12;++i) {
            char shaped[128];const char *text=languages[i];
            if(i==5){lv_text_ap_proc(text,shaped);text=shaped;}
            assert(find_text(lv_screen_active(),text));
        }
        assert(!find_text(lv_screen_active(),"Images: hours"));
        lv_obj_t *language_menu=lv_obj_get_parent(find_text(lv_screen_active(),"Languages"));
        lv_obj_update_layout(language_menu);lv_obj_scroll_to_y(language_menu,110,LV_ANIM_OFF);
        int language_scroll=lv_obj_get_scroll_y(language_menu);assert(language_scroll>0);
        menu_right_swipe();click("Rotation");
        lv_obj_t *matrix=menu_matrix(lv_screen_active());assert(matrix);
        assert(lv_buttonmatrix_get_selected_button(matrix)==runtime.local.orientation);
        assert(lv_buttonmatrix_has_button_ctrl(matrix,runtime.local.orientation,LV_BUTTONMATRIX_CTRL_CHECKED));
        assert(!find_text(lv_screen_active(),"English"));menu_right_swipe();
        click("Slideshow / playback order");matrix=menu_matrix(lv_screen_active());assert(matrix);
        assert(lv_buttonmatrix_get_selected_button(matrix)==runtime.state.settings.rotation);
        assert(lv_buttonmatrix_has_button_ctrl(matrix,runtime.state.settings.rotation,LV_BUTTONMATRIX_CTRL_CHECKED));
        menu_right_swipe();click("Languages");
        language_menu=lv_obj_get_parent(find_text(lv_screen_active(),"Languages"));
        assert(lv_obj_get_scroll_y(language_menu)==language_scroll);menu_right_swipe();
        main_menu=lv_obj_get_parent(find_text(lv_screen_active(),"Intervalls"));
        assert(lv_obj_get_scroll_y(main_menu)==main_scroll);
        lv_obj_t *reset=find_text(main_menu,"Factory reset");assert(reset);
        assert(lv_obj_get_index(lv_obj_get_parent(reset))==(int)lv_obj_get_child_count(main_menu)-1);
        assert(writes==before && fake_shared_calls==mesh_before &&
               memcmp(&runtime.state,&state_before,sizeof(state_before))==0 &&
               memcmp(&runtime.store.record,&record_before,sizeof(record_before))==0);
        katmed_i18n_select(KATMED_LANG_DE);render();
        assert(find_text(lv_screen_active(),"Sprachen"));
        katmed_i18n_select(KATMED_LANG_EN);
    } else {
        assert(!strcmp(argv[3],"save"));click("Times");
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
