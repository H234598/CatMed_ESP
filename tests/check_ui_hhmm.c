/* Actual LVGL button events with the existing Main/Core/Store fixture. */
#define UI_REFINEMENT_ENTRYPOINT ui_refinement_reference_main
#include "check_ui_refinement.c"
#undef UI_REFINEMENT_ENTRYPOINT
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"

static lv_obj_t *hhmm_keys(lv_obj_t *screen)
{
    lv_obj_t *keys=find_matrix_with_first_button(screen,"1");assert(keys);return keys;
}
static void hhmm_press(lv_obj_t *screen,unsigned index)
{
    lv_obj_t *keys=hhmm_keys(screen);
    assert(lv_buttonmatrix_get_button_text(keys,index));
    lv_buttonmatrix_set_selected_button(keys,index);
    lv_obj_send_event(keys,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_update_layout(screen);
}
static void hhmm_clear(lv_obj_t *screen)
{
    for(unsigned i=0;i<20;++i)hhmm_press(screen,3);
    assert(find_label(screen,"_"));
}
static void hhmm_type(lv_obj_t *screen,const char *digits)
{
    hhmm_clear(screen);
    for(const char *p=digits;*p;++p) {
        unsigned key=*p=='0'?12:(unsigned)(*p-'1')/3*4+(unsigned)(*p-'1')%3;
        hhmm_press(screen,key);
    }
}
static void hhmm_expect(lv_obj_t *screen,const char *value)
{
    assert(find_label(screen,value)&&"actual visible entry must match HH:MM digit editing");
}
static void hhmm_coordinate_press(lv_obj_t *screen,lv_indev_t *pointer,unsigned index,int width,int height)
{
    lv_obj_t *keys=hhmm_keys(screen);lv_obj_update_layout(keys);
    lv_area_t outer;lv_obj_get_coords(keys,&outer);
    const lv_area_t *a=&((lv_buttonmatrix_t *)keys)->button_areas[index];
    pointer_point.x=outer.x1+(a->x1+a->x2)/2;
    pointer_point.y=outer.y1+(a->y1+a->y2)/2;
    assert(pointer_point.x>=0&&pointer_point.x<width&&pointer_point.y>=0&&pointer_point.y<height);
    pointer_down=false;lv_indev_read(pointer);
    pointer_down=true;lv_indev_read(pointer);
    pointer_down=false;lv_indev_read(pointer);
    lv_obj_update_layout(screen);
    printf("coordinate_button=%u x=%d y=%d\n",index,(int)pointer_point.x,(int)pointer_point.y);
}
static void hhmm_layout(lv_obj_t *screen,int width,int height)
{
    const char *expected[]={"1","2","3","<","4","5","6","<",
        "7","8","9","<","0","0","0","<"};
    lv_obj_t *keys=hhmm_keys(screen);assert(lv_buttonmatrix_get_button_text(keys,15)&&!lv_buttonmatrix_get_button_text(keys,16));
    for(unsigned i=0;i<16;++i) {
        const char *actual=lv_buttonmatrix_get_button_text(keys,i);
        assert(actual&&strcmp(actual,expected[i])==0&&"HHMM keypad must have three zeros/four backspaces");
    }
    lv_obj_update_layout(screen);lv_area_t a;lv_obj_get_coords(keys,&a);
    assert(a.x1>=0&&a.y1>=0&&a.x2<width&&a.y2<height);
    lv_indev_t *pointer=lv_indev_create();assert(pointer);
    lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer,lv_display_get_default());
    lv_indev_set_read_cb(pointer,pointer_read);
    const unsigned zeros[]={12,13,14},backspaces[]={3,7,11,15};
    for(unsigned i=0;i<3;++i){hhmm_clear(screen);hhmm_coordinate_press(screen,pointer,zeros[i],width,height);hhmm_expect(screen,"0");}
    for(unsigned i=0;i<4;++i){hhmm_type(screen,"1234");hhmm_coordinate_press(screen,pointer,backspaces[i],width,height);hhmm_expect(screen,"12:3");}
    lv_indev_delete(pointer);
}
static void hhmm_edit(lv_obj_t *screen)
{
    unsigned writes=refinement_writes();
    hhmm_type(screen,"1111");hhmm_expect(screen,"11:11");
    hhmm_press(screen,0);hhmm_expect(screen,"11:11"); /* fifth digit is bounded */
    hhmm_press(screen,3);hhmm_expect(screen,"11:1");
    hhmm_press(screen,3);hhmm_expect(screen,"11");
    hhmm_press(screen,3);hhmm_expect(screen,"1");
    hhmm_press(screen,3);hhmm_expect(screen,"_");
    const char *partials[]={"","1","12","123","2400","1260"};
    for(unsigned i=0;i<sizeof(partials)/sizeof(partials[0]);++i) {
        hhmm_type(screen,partials[i]);click_label(screen,"Speichern");
        assert(hhmm_keys(screen)&&refinement_writes()==writes);
    }
    hhmm_type(screen,"0005");hhmm_expect(screen,"00:05");
    hhmm_type(screen,"0000");hhmm_expect(screen,"00:00");
    click_label(screen,"Speichern");assert(!find_label(screen,"Nächste Zeit HH:MM")&&!find_label(screen,"HH:MM"));
    click_label(screen,"Nächste Fälligkeit HH:MM");
    hhmm_type(screen,"2359");hhmm_expect(screen,"23:59");
    click_label(screen,"Speichern");assert(!find_label(screen,"Nächste Zeit HH:MM")&&!find_label(screen,"HH:MM"));
    assert(refinement_writes()>writes);
}
static void hhmm_status(lv_obj_t *screen,const char *expected)
{
    click_label(screen,"Status"); /* Actual edge gesture, no ui_render before check. */
    lv_obj_t *row=find_label_prefix(screen,"Nächster Alarm:");
    assert(row&&strstr(lv_label_get_text(row),expected));
    refresh();row=find_label_prefix(screen,"Nächster Alarm:");
    assert(row&&strstr(lv_label_get_text(row),expected));
    click_label(screen,"Zurück");
}
static void hhmm_flow(lv_obj_t *screen)
{
    hhmm_type(screen,"1111");click_label(screen,"Speichern");
    LocalDateTime local;
    assert(app_berlin_local_time(refinement_state()->deadline.effective_utc.ms,&local)==APP_OK);
    assert(local.hour==11&&local.minute==11);
    hhmm_status(screen,"11:11");
    unsigned writes=refinement_writes();
    click_label(screen,"Nächste Fälligkeit HH:MM");hhmm_expect(screen,"11:11");
    assert(refinement_writes()==writes); /* Opening neither submits nor writes. */
    click_label(screen,"Speichern"); /* Existing prefilled HH:MM remains accepted. */
    hhmm_status(screen,"11:11");
    click_label(screen,"Nächste Fälligkeit HH:MM");
    hhmm_type(screen,"1222");click_label(screen,"Speichern");hhmm_status(screen,"12:22");
    click_label(screen,"Nächste Fälligkeit HH:MM");hhmm_expect(screen,"12:22");
    hhmm_type(screen,"2359");refinement_fail_commit(true);
    click_label(screen,"Speichern");hhmm_expect(screen,"23:59");
    assert(!find_label(screen,"Gespeichert"));
    assert(app_berlin_local_time(refinement_state()->deadline.effective_utc.ms,&local)==APP_OK);
    assert(local.hour==12&&local.minute==22);
    right_swipe();hhmm_status(screen,"12:22");
}
static void hhmm_decimal(lv_obj_t *screen)
{
    click_label(screen,"Abbrechen");
    const char *fields[]={"Bilder: Stunden","Medikamente: Stunden"};
    for(unsigned i=0;i<2;++i) {
        click_label(screen,fields[i]);
        enter_keys(screen,"1.5");hhmm_expect(screen,"1.5");
        click_label(screen,"Speichern");
        assert((i?refinement_state()->settings.medication_seconds:refinement_state()->settings.image_seconds)==5400);
        click_label(screen,fields[i]);enter_keys(screen,"2,5");hhmm_expect(screen,"2,5");
        click_label(screen,"Speichern");
        assert((i?refinement_state()->settings.medication_seconds:refinement_state()->settings.image_seconds)==9000);
    }
    click_label(screen,"Nächste Fälligkeit HH:MM");
    const unsigned writes=refinement_writes();hhmm_type(screen,"1020");right_swipe();
    assert(ui_current_view()==SETTINGS&&refinement_writes()==writes);
}
int main(int argc,char **argv)
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
    KatmedStorage storage={pack,katmed_generated_catalog.pack_bytes,pack_read,NULL};
    assert(katmed_images_mount_storage(storage)==KATMED_IMAGE_OK);
    refinement_init(katmed_images_catalog_sha256(),katmed_images_count());
    UiServices services=refinement_services();ui_set_services(&services);
    katmed_i18n_select(KATMED_LANG_DE); /* HH:MM flow uses German control labels. */
    ClockSample now=refinement_clock();
    ui_render(refinement_state(),refinement_local(),SETTINGS,&now);draw();
    lv_obj_t *screen=lv_screen_active();
    click_label(screen,"Zeiten");

    unsigned writes=refinement_writes();
    AppState opening=*refinement_state();
    LocalDateTime configured;
    assert(app_berlin_local_time(opening.deadline.effective_utc.ms,&configured)==APP_OK);
    char expected[32];snprintf(expected,sizeof(expected),"%02u:%02u",configured.hour,configured.minute);
    click_label(screen,"Nächste Fälligkeit HH:MM");
    assert(refinement_writes()==writes&&memcmp(&opening,refinement_state(),sizeof(opening))==0);
    hhmm_expect(screen,expected);
    if(!strcmp(argv[1],"layout"))hhmm_layout(screen,host.width,host.height);
    else if(!strcmp(argv[1],"editing"))hhmm_edit(screen);
    else if(!strcmp(argv[1],"flow"))hhmm_flow(screen);
    else if(!strcmp(argv[1],"decimal"))hhmm_decimal(screen);
    else assert(!"unknown test mode");
    printf("HHMM %s %dx%d PASS\n",argv[1],host.width,host.height);
    fclose(pack);lv_display_delete(display);lv_deinit();free(buffer);free(host.frame);
    return 0;
}
