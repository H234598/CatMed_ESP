#define _POSIX_C_SOURCE 200809L
#define katmed_images_present review_unused_synthetic_present
#define main original_surface_main
#include "../check_ui_surface.c"
#undef main
#undef katmed_images_present
#include "katmed_images_internal.h"
#include "katmed_core.h"
#include "katmed_font.h"
#include <time.h>
#ifndef DRAW_LINES
#define DRAW_LINES 16u
#endif
static unsigned render_begin[KATMED_RENDER_INNER_COUNT];
static unsigned render_end[KATMED_RENDER_INNER_COUNT];
static uint8_t render_last[KATMED_RENDER_INNER_COUNT];
static void render_observer(void *context, KatmedRenderInnerPhase phase,
                            bool begin, uint8_t outcome)
{
 (void)context;assert((unsigned)phase<KATMED_RENDER_INNER_COUNT);
 if(begin)++render_begin[phase];
 else {++render_end[phase];render_last[phase]=outcome;}
}
static uint64_t pack_reads;
static lv_indev_state_t probe_touch_state=LV_INDEV_STATE_RELEASED;
static lv_point_t probe_touch_point={200,190};
static unsigned probe_scroll_events,probe_scroll_end_events;
static void probe_read(lv_indev_t *indev,lv_indev_data_t *data){(void)indev;data->state=probe_touch_state;data->point=probe_touch_point;}
static void probe_scroll_event(lv_event_t *event){if(lv_event_get_code(event)==LV_EVENT_SCROLL)++probe_scroll_events;else if(lv_event_get_code(event)==LV_EVENT_SCROLL_END)++probe_scroll_end_events;}
static void probe_sample(lv_indev_t *indev,lv_indev_state_t state,int y){probe_touch_state=state;probe_touch_point.y=y;lv_tick_inc(40);lv_indev_read(indev);}
static uint64_t now_us(void){struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;}
static size_t monitor(const char *name) {
 lv_mem_monitor_t m;lv_mem_monitor(&m);size_t used=m.total_size-m.free_size;
 printf("heap %s total=%zu used=%zu free=%zu biggest=%zu peak=%zu fragments=%u image_error=%d\n",name,m.total_size,used,m.free_size,m.free_biggest_size,m.max_used,m.frag_pct,katmed_images_last_error());fflush(stdout);return used;
}
static bool read_image_pack(void *ctx, uint32_t offset, uint8_t *out, size_t wanted, size_t *actual) {
 ++pack_reads;
 FILE *file = ctx; *actual=0;
 if (offset > katmed_generated_catalog.pack_bytes || wanted > katmed_generated_catalog.pack_bytes-offset || fseek(file,(long)offset,SEEK_SET)) return false;
 *actual=fread(out,1,wanted,file);return *actual==wanted;
}
static void input_text(lv_obj_t*s,const char*t){for(;*t;t++){char key[]={*t,0};click_label(s,key);}}
static unsigned frame_differences(const uint16_t *a, const uint16_t *b, size_t n) {
 unsigned different=0; for(size_t i=0;i<n;i++) different += a[i]!=b[i]; return different;
}
static bool white_near_segment(const uint16_t *frame,const uint16_t *background,
                               int width,int height,int ox,int oy,
                               const lv_point_precise_t *points,int *hit_x,int *hit_y) {
 for(int step=2;step<=10;step++) {
  int x=ox+(int)(points[0].x+(points[1].x-points[0].x)*step/10);
  int y=oy+(int)(points[0].y+(points[1].y-points[0].y)*step/10);
  for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++) {
   int px=x+dx,py=y+dy;
   if(px>=0&&py>=0&&px<width&&py<height&&
      frame[(size_t)py*width+px]==0xffffu&&
      (!background||background[(size_t)py*width+px]!=0xffffu)) {
    if(hit_x) *hit_x=px;
    if(hit_y) *hit_y=py;
    return true;
   }
  }
 }
 return false;
}
int main(int argc,char**argv){assert(argc==5);int width=atoi(argv[1]),height=atoi(argv[2]);const char *mode=argv[3];
 Host host={.width=width,.height=height,.result=UI_SUBMIT_COMMITTED};host.frame=calloc((size_t)width*height,2);uint16_t *buffer=calloc((size_t)width*DRAW_LINES,2);assert(host.frame&&buffer);lv_init();monitor("after_lv_init");
 lv_display_t*d=lv_display_create(width,height);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(d,buffer,NULL,(uint32_t)width*DRAW_LINES*2,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);lv_display_set_user_data(d,&host);monitor("before_ui_render");
 katmed_images_register_lvgl_fs();FILE *pack=fopen(argv[4],"rb");assert(pack);KatmedStorage storage={pack,katmed_generated_catalog.pack_bytes,read_image_pack,NULL};assert(katmed_images_mount_storage(storage)==KATMED_IMAGE_OK);
 katmed_images_set_render_trace(render_observer,NULL);
 UiServices services={.context=&host,.submit=submit,.connected=connected,.render_trace=render_observer};ui_set_services(&services);AppState state={0};state.round.origin=(EventId){1,1,1};state.round.ordinal=1;LocalSettings local={.clock_style=DIGITAL_LARGE,.orientation=width>height?ORIENTATION_90:ORIENTATION_0};ClockSample clock={.monotonic_ms=1000,.domain=1,.utc_ms=1791973800000LL,.utc_valid=true,.quality=CLOCK_SYNCHRONIZED};
 ui_render(&state,&local,BACKGROUND,&clock);draw();monitor("first_render_background");lv_obj_t*s=lv_screen_active();
 if(!strcmp(mode,"render_trace")){
  memset(render_begin,0,sizeof(render_begin));
  memset(render_end,0,sizeof(render_end));
  katmed_i18n_select(KATMED_LANG_FR);
  clock.monotonic_ms+=1000;
  ui_render(&state,&local,BACKGROUND,&clock);
  printf("render_trace_counts language=%u/%u present=%u/%u select=%u/%u hash=%u/%u jpeg=%u/%u status=%u\n",
         render_begin[KATMED_RENDER_INNER_LANGUAGE_REBUILD],render_end[KATMED_RENDER_INNER_LANGUAGE_REBUILD],
         render_begin[KATMED_RENDER_INNER_IMAGE_PRESENT],render_end[KATMED_RENDER_INNER_IMAGE_PRESENT],
         render_begin[KATMED_RENDER_INNER_IMAGE_SELECT],render_end[KATMED_RENDER_INNER_IMAGE_SELECT],
         render_begin[KATMED_RENDER_INNER_VARIANT_HASH],render_end[KATMED_RENDER_INNER_VARIANT_HASH],
         render_begin[KATMED_RENDER_INNER_JPEG_DECODE],render_end[KATMED_RENDER_INNER_JPEG_DECODE],
         render_last[KATMED_RENDER_INNER_IMAGE_PRESENT]);fflush(stdout);
  assert(render_begin[KATMED_RENDER_INNER_LANGUAGE_REBUILD]==1 &&
         render_end[KATMED_RENDER_INNER_LANGUAGE_REBUILD]==1 &&
         render_begin[KATMED_RENDER_INNER_IMAGE_PRESENT]==1 &&
         render_end[KATMED_RENDER_INNER_IMAGE_PRESENT]==1 &&
         render_begin[KATMED_RENDER_INNER_IMAGE_SELECT]==1 &&
         render_end[KATMED_RENDER_INNER_IMAGE_SELECT]==1 &&
         render_begin[KATMED_RENDER_INNER_VARIANT_HASH]>=1 &&
         render_begin[KATMED_RENDER_INNER_VARIANT_HASH]==
             render_end[KATMED_RENDER_INNER_VARIANT_HASH] &&
         render_begin[KATMED_RENDER_INNER_JPEG_DECODE]==1 &&
         render_end[KATMED_RENDER_INNER_JPEG_DECODE]==1 &&
         render_last[KATMED_RENDER_INNER_IMAGE_PRESENT]==KATMED_IMAGE_OK);
  ui_render(&state,&local,BACKGROUND,&clock);
  assert(render_begin[KATMED_RENDER_INNER_LANGUAGE_REBUILD]==1 &&
         render_begin[KATMED_RENDER_INNER_IMAGE_PRESENT]==1);
  puts("real language rebuild and image producer trace passed");
 }
 else if(!strcmp(mode,"heap")){
 ui_render(&state,&local,SETTINGS,&clock);draw();monitor("settings");click_label(s,"Uhrdesign");draw();monitor("style_dialog");click_label(s,"Abbrechen");draw();size_t stable=monitor("style_closed");click_label(s,"Advanced");
 for(int i=0;i<100;i++){click_label(s,"Uhrdesign");click_label(s,"Analog");click_label(s,"Abbrechen");}draw();assert(monitor("style_100_closed")<=stable+64);size_t warmed=monitor("style_warmed");for(int i=0;i<100;i++){click_label(s,"Uhrdesign");click_label(s,"Analog");click_label(s,"Abbrechen");}draw();assert(monitor("style_200_closed")==warmed);
 click_label(s,"Medikamente: Stunden");draw();monitor("hours_dialog");click_label(s,"Abbrechen");draw();size_t closed=monitor("hours_closed");for(int i=0;i<100;i++){click_label(s,"Medikamente: Stunden");click_label(s,"1");click_label(s,"Abbrechen");}draw();assert(monitor("hours_100_closed")<=closed+64);size_t warmed_hours=monitor("hours_warmed");for(int i=0;i<100;i++){click_label(s,"Medikamente: Stunden");click_label(s,"1");click_label(s,"Abbrechen");}draw();assert(monitor("hours_200_closed")==warmed_hours);
 ui_render(&state,&local,CLOCK,&clock);draw();monitor("clock_digital");local.clock_style=ANALOG;ui_render(&state,&local,CLOCK,&clock);draw();monitor("clock_analog");state.alarm_active=true;ui_render(&state,&local,CLOCK,&clock);draw();monitor("alarm");
 }
 else if(!strcmp(mode,"contracts")){
 ui_render(&state,&local,SETTINGS,&clock);click_label(s,"Uhrdesign");click_label(s,"Analog");assert(host.submit_calls==0);click_label(s,"Abbrechen");assert(host.submit_calls==0);assert(!find_label(s,"Uhrdesign wählen"));click_label(s,"Uhrdesign");click_label(s,"Digital mit Datum");host.result=UI_SUBMIT_REJECTED;click_label(s,"Speichern");assert(host.submit_calls==1&&find_label(s,"Änderung abgelehnt"));host.result=UI_SUBMIT_COMMITTED;click_label(s,"Speichern");assert(host.submit_calls==2&&host.last_request.kind==UI_REQUEST_SET_CLOCK_STYLE&&host.last_request.value.clock_style==DIGITAL_DATE);assert(!find_label(s,"Uhrdesign wählen"));click_label(s,"Advanced");
 const char*values[]={"1.5","1,5","0","999999999999999999999999","1..2"};for(unsigned i=0;i<5;i++){int before=host.submit_calls;click_label(s,"Bilder: Stunden");input_text(s,values[i]);click_label(s,"Speichern");if(i<2){assert(host.submit_calls==before+1&&host.last_request.value.seconds==5400);}else {assert(host.submit_calls==before);click_label(s,"Abbrechen");}}
 click_label(s,"Nächste Fälligkeit HH:MM");input_text(s,"12:34");host.result=UI_SUBMIT_STALE;int before=host.submit_calls;click_label(s,"Speichern");assert(host.submit_calls==before+1);click_label(s,"Speichern");assert(host.submit_calls==before+1);click_label(s,"<");click_label(s,"4");host.result=UI_SUBMIT_COMMITTED;click_label(s,"Speichern");assert(host.submit_calls==before+2);assert(host.last_request.kind==UI_REQUEST_SET_NEXT_LOCAL_TIME);
 const uint32_t chars[]={0xe4,0xf6,0xfc,0xc4,0xd6,0xdc,0xdf,0xb0};const lv_font_t*fonts[]={&katmed_font_10,&katmed_font_12,&katmed_font_14,&katmed_font_26};for(unsigned f=0;f<4;f++)for(unsigned c=0;c<8;c++){lv_font_glyph_dsc_t glyph;assert(lv_font_get_glyph_dsc(fonts[f],&glyph,chars[c],0));assert(!glyph.is_placeholder);}
 printf("contracts=PASS calls=%d\n",host.submit_calls);
 }
 else if(!strcmp(mode,"settings_latency")){
  ui_render(&state,&local,SETTINGS,&clock);draw();
  lv_obj_t *title=find_label(s,"Einstellungen");assert(title);
  lv_obj_t *settings=lv_obj_get_parent(title);assert(settings&&lv_obj_is_scrollable(settings));
  uint64_t before_reads=pack_reads, before_us=now_us();
  lv_obj_scroll_by(settings,0,-80,LV_ANIM_OFF);draw();
  printf("settings_scroll_reads=%llu settings_scroll_us=%llu buffer_lines=%u scroll_y=%ld\n",
   (unsigned long long)(pack_reads-before_reads),(unsigned long long)(now_us()-before_us),
   DRAW_LINES,(long)lv_obj_get_scroll_y(settings));
 }
 else if(!strcmp(mode,"indev_release")){
  ui_render(&state,&local,SETTINGS,&clock);draw();
  lv_obj_t *title=find_label(s,"Einstellungen");assert(title);
  lv_obj_t *settings=lv_obj_get_parent(title);assert(settings);
  lv_obj_add_event_cb(settings,probe_scroll_event,LV_EVENT_ALL,NULL);
  lv_indev_t *indev=lv_indev_create();assert(indev);
  lv_indev_set_type(indev,LV_INDEV_TYPE_POINTER);lv_indev_set_display(indev,d);
  lv_indev_set_read_cb(indev,probe_read);
  probe_sample(indev,LV_INDEV_STATE_PRESSED,190);
  probe_sample(indev,LV_INDEV_STATE_PRESSED,150);
  probe_sample(indev,LV_INDEV_STATE_PRESSED,100);
  probe_sample(indev,LV_INDEV_STATE_PRESSED,60);
  long at_release=(long)lv_obj_get_scroll_y(settings);
  probe_sample(indev,LV_INDEV_STATE_RELEASED,60);
  long immediate=(long)lv_obj_get_scroll_y(settings);
  unsigned events_at_release=probe_scroll_events;
  for(int i=0;i<8;i++){lv_tick_inc(40);lv_timer_handler();}
  long after_throw=(long)lv_obj_get_scroll_y(settings);
  unsigned events_after_release=probe_scroll_events-events_at_release;
  probe_sample(indev,LV_INDEV_STATE_PRESSED,60);
  probe_sample(indev,LV_INDEV_STATE_PRESSED,105);
  probe_sample(indev,LV_INDEV_STATE_PRESSED,150);
  long after_reverse=(long)lv_obj_get_scroll_y(settings);
  probe_sample(indev,LV_INDEV_STATE_RELEASED,150);
  printf("settings_indev_release at=%ld immediate=%ld after_throw=%ld reverse=%ld postrelease_scroll_events=%u scroll_end=%u momentum=%d\n",
   at_release,immediate,after_throw,after_reverse,events_after_release,probe_scroll_end_events,
   lv_obj_is_scroll_momentum(settings));
  fflush(stdout);
  assert(at_release>0);
  assert(after_reverse<after_throw);
  assert(after_throw==immediate && events_after_release==0);
  lv_indev_delete(indev);
 }
 else if(!strcmp(mode,"alarm_geometry")){
  assert(width==240&&height==240);
  uint16_t before_29=host.frame[29*width+120],before_30=host.frame[30*width+120];
  uint16_t before_209=host.frame[209*width+120],before_210=host.frame[210*width+120];
  SharedSettings shared=app_default_settings();
  const uint8_t catalog[KATMED_CATALOG_BYTES]={1};
  assert(app_init(&state,&shared,&clock,(EventId){1,1,1},1,catalog)==APP_OK);
  clock.monotonic_ms+=1300;
  AlarmAnimation phase;
  assert(app_alarm_animation(&state,&clock,&phase)==APP_OK);
  assert(phase.color==ALARM_COLOR_BLUE&&phase.phase_permyriad==5000);
  ui_render(&state,&local,ALARM,&clock);draw();
  assert(host.frame[29*width+120]==before_29);
  assert(host.frame[30*width+120]!=before_30);
  assert(host.frame[209*width+120]!=before_209);
  assert(host.frame[210*width+120]==before_210);
  printf("alarm_geometry=PASS phase=%u top=30 bottom=209 margin_rows=29,210 lines=%u\n",
   phase.phase_permyriad,DRAW_LINES);
 }
 else if(!strcmp(mode,"analog_overlay")){
  assert(width==240&&height==240&&katmed_images_count()>=2);
  size_t pixels=(size_t)width*height;
  uint16_t *background0=malloc(pixels*sizeof(*background0));
  uint16_t *background1=malloc(pixels*sizeof(*background1));
  assert(background0&&background1);
  memcpy(background0,host.frame,pixels*sizeof(*background0));
  state.image_id=1;ui_render(&state,&local,BACKGROUND,&clock);draw();
  memcpy(background1,host.frame,pixels*sizeof(*background1));
  unsigned image_delta=frame_differences(background0,background1,pixels);
  assert(image_delta>1000);
  state.image_id=0;ui_render(&state,&local,BACKGROUND,&clock);draw();
  local.clock_style=ANALOG;ui_render(&state,&local,CLOCK,&clock);
  for(int i=0;i<8;i++)draw();
  lv_obj_t *view=find_clock_view(s);assert(view);
  lv_obj_t *plate=lv_obj_get_child(view,0);assert(plate);
  lv_area_t plate_area,dial_area;lv_obj_get_coords(plate,&plate_area);
  lv_obj_t *dial=lv_obj_get_child(plate,3);assert(dial);lv_obj_get_coords(dial,&dial_area);
  int plate_probe_x=(plate_area.x1+plate_area.x2)/2;
  int plate_probe_y=plate_area.y1+2;
  size_t plate_probe=(size_t)plate_probe_y*width+plate_probe_x;
  printf("analog_baseline images=0,1 image_pixels_changed=%u plate_probe=(%d,%d) actual=0x%04x background=0x%04x\n",
         image_delta,plate_probe_x,plate_probe_y,host.frame[plate_probe],background0[plate_probe]);
  fflush(stdout);
  assert(host.frame[plate_probe]==background0[plate_probe]);
  int dial_x=dial_area.x2-1;
  int dial_y=(dial_area.y1+dial_area.y2)/2;
  size_t ring_probe=(size_t)dial_y*width+dial_x;
  assert(background0[ring_probe]!=0xffffu);
  assert(host.frame[ring_probe]==background0[ring_probe]);
  lv_obj_t *tick=lv_obj_get_child(dial,0);assert(tick);
  const lv_point_precise_t *tick_points=lv_line_get_points(tick);assert(tick_points);
  int tick_x=dial_area.x1+(int)((tick_points[0].x+tick_points[1].x)/2);
  int tick_y=dial_area.y1+(int)((tick_points[0].y+tick_points[1].y)/2);
  size_t tick_probe=(size_t)tick_y*width+tick_x;
  assert(background0[tick_probe]!=0xffffu);
  assert(host.frame[tick_probe]==background0[tick_probe]);
  lv_obj_t *hour_hand=lv_obj_get_child(dial,12),*minute_hand=lv_obj_get_child(dial,13);
  assert(hour_hand&&minute_hand&&!lv_obj_is_hidden(hour_hand)&&
         !lv_obj_is_hidden(minute_hand));
  const lv_point_precise_t *hour_points=lv_line_get_points(hour_hand);
  const lv_point_precise_t *minute_points=lv_line_get_points(minute_hand);
  assert(hour_points&&minute_points);
  assert(white_near_segment(host.frame,background0,width,height,dial_area.x1,dial_area.y1,
                            hour_points,NULL,NULL));
  assert(white_near_segment(host.frame,background0,width,height,dial_area.x1,dial_area.y1,
                            minute_points,NULL,NULL));
  lv_point_precise_t old_minute_tip=minute_points[1];
  state.image_id=1;ui_render(&state,&local,CLOCK,&clock);draw();
  size_t image_probe=(size_t)plate_probe_y*width+plate_probe_x;
  assert(host.frame[image_probe]==background1[image_probe]);
  int old_ray_x=-1,old_ray_y=-1;
  minute_points=lv_line_get_points(minute_hand);assert(minute_points);
  assert(white_near_segment(host.frame,background1,width,height,dial_area.x1,dial_area.y1,
                            minute_points,&old_ray_x,&old_ray_y));
  int hour_before=host.submit_calls;
  clock.utc_ms=1792026000000LL;ui_render(&state,&local,CLOCK,&clock);draw();
  assert(host.submit_calls==hour_before);
  minute_points=lv_line_get_points(minute_hand);assert(minute_points);
  assert(minute_points[1].x!=old_minute_tip.x||minute_points[1].y!=old_minute_tip.y);
  assert(white_near_segment(host.frame,background1,width,height,dial_area.x1,dial_area.y1,
                            minute_points,NULL,NULL));
  size_t stale_probe=(size_t)old_ray_y*width+old_ray_x;
  assert(host.frame[stale_probe]==background1[stale_probe]);
  local.clock_style=DIGITAL_DATE;clock.utc_ms=1791973800000LL;
  ui_render(&state,&local,CLOCK,&clock);draw();
  assert(find_label(s,"12:30")&&find_label(s,"14.10.2026"));
  clock.utc_valid=false;clock.quality=CLOCK_UNAVAILABLE;
  local.clock_style=ANALOG;ui_render(&state,&local,CLOCK,&clock);draw();
  assert(find_label(s,"--:--")&&find_label(s,"Uhrzeit nicht verfügbar"));
  assert(host.frame[image_probe]==background1[image_probe]);
  swipe(220,20,5000);ui_render(&state,&local,BACKGROUND,&clock);for(int i=0;i<8;i++)draw();
  ui_render(&state,&local,BACKGROUND,&clock);draw();
  assert(!find_clock_view(s));
  assert(frame_differences(host.frame,background1,pixels)==0);
  printf("analog_overlay=PASS images=0,1 image_pixels_changed=%u no_overlay_probe=(%d,%d)\n",
         image_delta,plate_probe_x,plate_probe_y);
  free(background0);free(background1);
 }
 else assert(0);
 KatmedImageStatus image_error=katmed_images_last_error();
 if(image_error!=KATMED_IMAGE_OK) fprintf(stderr,"image_error=%d\n",image_error);
 assert(image_error==KATMED_IMAGE_OK);lv_display_delete(d);assert(katmed_images_open_file_count()==0);katmed_images_set_render_trace(NULL,NULL);katmed_images_unmount();assert(fclose(pack)==0);monitor("after_display_delete");lv_deinit();free(buffer);free(host.frame);return 0;
}
