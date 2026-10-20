#define katmed_images_present unused_synthetic_present
#define main prior_surface_test
#include "check_ui_surface.c"
#undef main
#undef katmed_images_present
#include "check_ui_refinement_main.h"
#include "katmed_images_internal.h"
#include "katmed_font.h"

static uint64_t reads;
static KatmedInteractionStatus interaction_model;
static KatmedInteractionInput interaction_inputs[32];
static unsigned interaction_input_count;
static bool interaction_capture(void *context,const KatmedInteractionInput *input,uint64_t now)
{
    (void)context;(void)now;assert(interaction_input_count<32);
    interaction_inputs[interaction_input_count++]=*input;
    if(input->kind==KATMED_INTERACTION_ENTER) {
        interaction_model.menu_entry=input->menu_entry;interaction_model.menu_open=true;
    }
    if(input->kind==KATMED_INTERACTION_DOWN) interaction_model.key_down=true;
    if(input->kind==KATMED_INTERACTION_UP || input->kind==KATMED_INTERACTION_CANCEL)
        interaction_model.key_down=false;
    if(input->kind==KATMED_INTERACTION_LEAVE) {
        interaction_model.menu_open=interaction_model.owns_token=interaction_model.key_down=false;
        interaction_model.finish_prompt=false;
    }
    return true;
}
static void interaction_project(void *context,KatmedInteractionStatus *out,uint64_t now)
{(void)context;(void)now;*out=interaction_model;}
static bool pack_read(void *context,uint32_t offset,uint8_t *out,size_t wanted,size_t *actual)
{
    ++reads; FILE *file=context; *actual=0;
    if (offset>katmed_generated_catalog.pack_bytes || wanted>katmed_generated_catalog.pack_bytes-offset || fseek(file,offset,SEEK_SET)) return false;
    *actual=fread(out,1,wanted,file); return *actual==wanted;
}
static void refresh(void)
{
    ClockSample now=refinement_clock();
    ui_render(refinement_state(),refinement_local(),ui_current_view(),&now);
}
static void right_swipe(void)
{
    static uint64_t tick=10000;
    tick+=1000;
    ui_touch_sample(false,0,0,true,tick);
    ui_touch_sample(true,35,115,true,tick+50);
    ui_touch_sample(true,185,115,true,tick+150);
    ui_touch_sample(false,0,0,true,tick+250);
}
static void grid_swipe(bool vertical,bool positive)
{
    static uint64_t tick=500000;
    tick+=1000;
    int x0=vertical?120:positive?185:35;
    int x1=vertical?120:positive?35:185;
    int y0=vertical?(positive?238:1):110;
    int y1=vertical?(positive?110:130):110;
    ui_touch_sample(false,0,0,true,tick);
    ui_touch_sample(true,x0,y0,true,tick+50);
    ui_touch_sample(true,x1,y1,true,tick+150);
    ui_touch_sample(false,0,0,true,tick+250);
}
#ifndef UI_REFINEMENT_ENTRYPOINT
#define UI_REFINEMENT_ENTRYPOINT main
#endif
int UI_REFINEMENT_ENTRYPOINT(int argc,char **argv)
{
    assert(argc==3||argc==5);
    Host host={.width=argc==5?atoi(argv[3]):240,.height=argc==5?atoi(argv[4]):240};
    host.frame=calloc((size_t)host.width*host.height,2);uint16_t *buffer=calloc((size_t)host.width*16,2);
    assert(host.frame&&buffer);lv_init();
    lv_display_t *display=lv_display_create(host.width,host.height);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,buffer,NULL,(uint32_t)host.width*16*2,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);lv_display_set_user_data(display,&host);
    katmed_images_register_lvgl_fs();FILE *pack=fopen(argv[2],"rb");assert(pack);
    KatmedStorage storage={pack,katmed_generated_catalog.pack_bytes,pack_read};
    assert(katmed_images_mount_storage(storage)==KATMED_IMAGE_OK);
    refinement_init(katmed_images_catalog_sha256(),katmed_images_count());
    UiServices services=refinement_services();ui_set_services(&services);
    ClockSample now=refinement_clock();
    ui_render(refinement_state(),refinement_local(),SETTINGS,&now);draw();
    lv_obj_t *screen=lv_screen_active();
    if (!strcmp(argv[1],"clock-menu")) {
        ui_render(refinement_state(),refinement_local(),CLOCK,&now);draw();
        grid_swipe(false,true);assert(ui_current_view()==BACKGROUND);
        grid_swipe(false,true);assert(ui_current_view()==SETTINGS);
        assert(find_label(screen,"Einstellungen"));
    }
    if (!strcmp(argv[1],"tick")) refinement_tick_contract();
    if (!strcmp(argv[1],"indices")) refinement_index_contract();
    if (!strcmp(argv[1],"random")) {
        const uint32_t medication=refinement_state()->settings.medication_seconds;
        const int64_t deadline=refinement_state()->deadline.effective_utc.ms;
        lv_mem_monitor_t before,after;lv_mem_monitor(&before);
        for(unsigned i=0;i<150;++i) {
            click_label(screen,i%3==0?"Zufall":i%3==1?"Aus":"Nach Namen");
            refresh();draw();
            assert(refinement_state()->settings.medication_seconds==medication);
            assert(refinement_state()->deadline.effective_utc.ms==deadline);
            assert(!refinement_state()->alarm_active);
            if(i%3==2) {
                assert(refinement_image((i/3+1)%katmed_images_count())==APP_OK);
                refresh();draw();
                assert(katmed_images_last_error()==KATMED_IMAGE_OK);
            }
        }
        lv_mem_monitor(&after);
        printf("original_catalog_count=%u random_callbacks=150 real_decodes=50 free_before=%zu free_after=%zu reads=%llu\n",katmed_images_count(),before.free_size,after.free_size,(unsigned long long)reads);
        assert(after.free_size>=before.free_size);
    }
    if (!strcmp(argv[1],"save")) {
        click_label(screen,"Advanced");
        click_label(screen,"Nächste Fälligkeit HH:MM");
        enter_keys(screen,"1111");click_label(screen,"Speichern");
        assert(refinement_state()->deadline.effective_utc.valid);
        LocalDateTime expected;
        assert(app_berlin_local_time(refinement_state()->deadline.effective_utc.ms,&expected)==APP_OK);
        assert(expected.hour==11&&expected.minute==11);
        /* Critical check: no model refresh or state replacement before Status. */
        click_label(screen,"Status");
        lv_obj_t *row=find_label_prefix(screen,"Nächster Alarm:");assert(row);
        assert(strstr(lv_label_get_text(row),"11:11")&&"first committed time must be shown before periodic refresh");
        refresh();
        row=find_label_prefix(screen,"Nächster Alarm:");assert(strstr(lv_label_get_text(row),"11:11"));
        click_label(screen,"Zurück");
        click_label(screen,"Nächste Fälligkeit HH:MM");
        enter_keys(screen,"1222");click_label(screen,"Speichern");
        click_label(screen,"Status");
        row=find_label_prefix(screen,"Nächster Alarm:");assert(strstr(lv_label_get_text(row),"12:22"));
        const AppState committed=*refinement_state();
        click_label(screen,"Zurück");click_label(screen,"Medikamente: Stunden");
        enter_keys(screen,"5");refinement_fail_commit(true);click_label(screen,"Speichern");
        assert(find_label(screen,"Speichern derzeit nicht verfügbar"));
        assert(refinement_state()->deadline.effective_utc.ms==committed.deadline.effective_utc.ms);
        assert(refinement_state()->settings.medication_seconds==committed.settings.medication_seconds);
        right_swipe();click_label(screen,"Status");
        row=find_label_prefix(screen,"Nächster Alarm:");assert(strstr(lv_label_get_text(row),"12:22"));
        refresh();
        row=find_label_prefix(screen,"Nächster Alarm:");assert(strstr(lv_label_get_text(row),"12:22"));
    }
    if (!strcmp(argv[1],"nav")) {
        click_label(screen,"Advanced");
        lv_obj_t *menu=lv_obj_get_parent(find_label(screen,"Einstellungen"));
        lv_obj_update_layout(menu);lv_obj_scroll_to_y(menu,140,LV_ANIM_OFF);
        const int saved=lv_obj_get_scroll_y(menu);
        unsigned writes=refinement_writes();
        click_label(screen,"Bilder: Stunden");
        click_label(screen,"3");
        right_swipe();
        menu=lv_obj_get_parent(find_label(screen,"Einstellungen"));
        assert(menu&&"detail right swipe must cancel and return to settings");
        lv_obj_update_layout(menu);
        assert(lv_obj_get_scroll_y(menu)==saved);
        assert(refinement_writes()==writes);
        assert(!find_label(menu,"Status")&&!find_label(menu,"Zurück"));
        const char *details[]={"Medikamente: Stunden","Nächste Fälligkeit HH:MM","Uhrdesign"};
        for(unsigned i=0;i<3;++i) {
            if(i<2) click_label(screen,"Advanced");
            click_label(screen,details[i]);right_swipe();
            menu=lv_obj_get_parent(find_label(screen,"Einstellungen"));assert(menu);
            assert(lv_obj_get_scroll_y(menu)==saved&&refinement_writes()==writes);
        }
        grid_swipe(false,false);assert(ui_current_view()==BACKGROUND);
        grid_swipe(false,true);assert(ui_current_view()==SETTINGS);
        menu=lv_obj_get_parent(find_label(screen,"Einstellungen"));assert(lv_obj_get_scroll_y(menu)==saved);
        grid_swipe(true,true);assert(ui_current_view()==UI_VIEW_STATUS);
        lv_obj_t *panel=lv_obj_get_parent(find_label(screen,"Status"));
        lv_obj_scroll_to_y(panel,80,LV_ANIM_OFF);
        for(int pos=40;pos>=0;pos-=20) {
            grid_swipe(false,false);assert(ui_current_view()==UI_VIEW_STATUS);
            panel=lv_obj_get_parent(find_label(screen,"Status"));
            if(pos)lv_obj_scroll_to_y(panel,pos,LV_ANIM_OFF);
        }
        panel=lv_obj_get_parent(find_label(screen,"Status"));assert(lv_obj_get_scroll_y(panel)==80);
        grid_swipe(true,false);assert(ui_current_view()==SETTINGS);
        menu=lv_obj_get_parent(find_label(screen,"Einstellungen"));assert(lv_obj_get_scroll_y(menu)==saved);
        services.interaction_input=interaction_capture;services.interaction_status=interaction_project;
        ui_set_services(&services);
        interaction_model=(KatmedInteractionStatus){.connected=true,.synchronized=true,
            .menu_open=true,.can_request=true,.authority_id=1,.authority_generation=1};
        click_label(screen,"Morsecode");refresh();right_swipe();
        assert(ui_current_view()==SETTINGS);
        assert(interaction_inputs[interaction_input_count-1].kind==KATMED_INTERACTION_LEAVE);
        assert(!interaction_model.menu_open);
        click_label(screen,"Morsecode");refresh();
        ui_touch_sample(false,0,0,true,700000);
        lv_obj_t *send=lv_obj_get_parent(find_label(screen,"Senden"));
        lv_obj_update_layout(send);
        interaction_model.owns_token=true;interaction_model.grant_id=11;
        refresh();
        ui_touch_sample(true,lv_obj_get_x(send)+lv_obj_get_width(send)/2,
                        lv_obj_get_y(send)+lv_obj_get_height(send)/2,true,700100);
        refresh();draw();
        assert(interaction_model.key_down&&host.frame[100*240+10]==0x867f);
        ui_touch_sample(false,0,0,false,700200); /* Cancel an interrupted held signal. */
        assert(interaction_inputs[interaction_input_count-1].kind==KATMED_INTERACTION_CANCEL);
        refresh();draw();assert(host.frame[100*240+10]!=0x867f);
        ui_touch_sample(false,0,0,true,700300);
        right_swipe();assert(ui_current_view()==SETTINGS);
        assert(interaction_inputs[interaction_input_count-1].kind==KATMED_INTERACTION_LEAVE);
        click_label(screen,"Morsecode");refresh();
        interaction_model.finish_prompt=true;interaction_model.prompt_id=7;
        refresh();assert(find_label(screen,"Fertig?"));right_swipe();
        assert(ui_current_view()==SETTINGS&&!find_label(screen,"Fertig?"));
        assert(!interaction_model.finish_prompt);
        click_label(screen,"Seltsam");
        ui_touch_sample(false,0,0,true,800000);
        ui_touch_sample(true,35,115,true,800050);
        ui_touch_sample(true,185,115,false,800100);
        ui_touch_sample(false,0,0,true,800200);
        assert(ui_current_view()==MORSE); /* Interrupted input is never an exit. */
        right_swipe();
        assert(ui_current_view()==SETTINGS);
        assert(lv_obj_get_scroll_y(lv_obj_get_parent(find_label(screen,"Einstellungen")))==saved);
    }
    if (!strncmp(argv[1],"scroll",6)) {
        lv_obj_t *panel;
        if (!strcmp(argv[1],"scroll-status")) {
            click_label(screen,"Status");panel=lv_obj_get_parent(find_label(screen,"Status"));
        } else panel=lv_obj_get_parent(find_label(screen,"Einstellungen"));
        lv_obj_update_layout(panel);
        lv_indev_t *indev=lv_indev_create();lv_indev_set_type(indev,LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(indev,display);lv_indev_set_read_cb(indev,pointer_read);
        const uint64_t before_scroll_reads=reads;
        pointer_point=(lv_point_t){120,170};pointer_down=true;
        for(int y=170;y>=65;y-=15) {
            pointer_point.y=y;lv_tick_inc(25);lv_indev_read(indev);
        }
        const int before_reverse=lv_obj_get_scroll_y(panel);
        pointer_point.y=105;lv_tick_inc(25);lv_indev_read(indev);
        const int after_reverse=lv_obj_get_scroll_y(panel);
        pointer_down=false;lv_tick_inc(25);lv_indev_read(indev);
        const int released=lv_obj_get_scroll_y(panel);
        draw();draw();
        const int settled=lv_obj_get_scroll_y(panel);
        printf("scroll_before_reverse=%d after_reverse=%d release=%d settled=%d tail=%d\n",before_reverse,after_reverse,released,settled,abs(settled-released));fflush(stdout);
        printf("scroll_background_pack_reads=%llu\n",(unsigned long long)(reads-before_scroll_reads));fflush(stdout);
        assert(after_reverse<before_reverse&&"direction reversal must follow the finger");
        assert(abs(settled-released)<=1&&"release must not produce inertial tail");
        assert(reads==before_scroll_reads&&"opaque list must not repeatedly decode hidden background");
        lv_indev_delete(indev);
    }
    if (!strcmp(argv[1],"clock")) {
        ClockSample future=now;future.utc_ms=INT64_C(4102441140000);
        LocalSettings local=*refinement_local();
        for(int style=DIGITAL_LARGE;style<=DIGITAL_DATE;++style) {
            local.clock_style=(ClockStyle)style;
            ui_render(refinement_state(),&local,CLOCK,&future);draw();
            lv_obj_t *time=find_label(screen,"23:59");
            if(!time)time=find_label(screen,"23:\n59");
            assert(time);
            const lv_font_t *font=lv_obj_get_style_text_font(time,0);
            lv_font_glyph_dsc_t old_glyph,new_glyph;
            assert(lv_font_get_glyph_dsc(&katmed_font_26,&old_glyph,'8',0));
            assert(lv_font_get_glyph_dsc(font,&new_glyph,'8',0));
            printf("clock_format=%dx%d style=%d old_time_height=%u new_time_height=%u\n",host.width,host.height,style,old_glyph.box_h,new_glyph.box_h);fflush(stdout);
            assert(new_glyph.box_h*2>=old_glyph.box_h*5);
            lv_area_t area;lv_obj_get_coords(time,&area);
            assert(area.x1>=0&&area.x2<host.width&&area.y1>=0&&area.y2<host.height);
            lv_point_t extent;
            lv_text_get_size(&extent,lv_label_get_text(time),font,0,0,10000,LV_TEXT_FLAG_NONE);
            printf("time_extent=%dx%d label=%dx%d\n",(int)extent.x,(int)extent.y,(int)lv_obj_get_width(time),(int)lv_obj_get_height(time));fflush(stdout);
            assert(extent.x<=lv_obj_get_width(time)&&extent.y<=lv_obj_get_height(time));
            if(style==DIGITAL_DATE) {
                lv_obj_t *date=find_label(screen,"31.12.2099");assert(date);
                const lv_font_t *datefont=lv_obj_get_style_text_font(date,0);
                assert(lv_font_get_glyph_dsc(&katmed_font_14,&old_glyph,'8',0));
                assert(lv_font_get_glyph_dsc(datefont,&new_glyph,'8',0));
                printf("old_date_height=%u new_date_height=%u\n",old_glyph.box_h,new_glyph.box_h);
                assert(new_glyph.box_h>=old_glyph.box_h*2);
                lv_obj_get_coords(date,&area);
                assert(area.x1>=0&&area.x2<host.width&&area.y1>=0&&area.y2<host.height);
                lv_text_get_size(&extent,lv_label_get_text(date),datefont,0,0,10000,LV_TEXT_FLAG_NONE);
                assert(extent.x<=lv_obj_get_width(date)&&extent.y<=lv_obj_get_height(date));
            }
        }
    }
    fclose(pack);lv_display_delete(display);lv_deinit();free(buffer);free(host.frame);
    return 0;
}
