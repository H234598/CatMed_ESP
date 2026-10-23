/* Exercise actual UI objects, LVGL rasterizer and partial RGB565 flush. */
#define katmed_images_present unused_alarm_synthetic_present
#define main existing_surface_main
#include "check_ui_surface.c"
#undef main
#undef katmed_images_present
#include "katmed_images_internal.h"
#include "src/image/lv_image_decoder_private.h"
static bool fail_decoder_read;
static bool alarm_pack_read(void *ctx,uint32_t off,uint8_t *out,size_t want,size_t *got){
    FILE *f=ctx; *got=0;
    if(fail_decoder_read) return false;
    if(fseek(f,off,SEEK_SET)) return false;
    *got=fread(out,1,want,f);return *got==want;
}

/* The real TJpgD callback must finish once the requested partial-refresh rows
 * are decoded. Prefix MCUs remain necessary for the JPEG stream. */
static void check_decoder_clip(int width, int height)
{
    KatmedImageSelection selection;
    Orientation orientation = width > height ? ORIENTATION_90 : ORIENTATION_0;
    assert(katmed_images_select(0, orientation, &selection) == KATMED_IMAGE_OK);
    /* This target intentionally uses the C3 JPEG fixture for all UI layouts. */
    width = selection.width;
    height = selection.height;
    const int bottoms[] = {15, height / 2 - 1, height - 1};
    unsigned full_calls = 0, first_calls = 0;
    for(unsigned test = 0; test < 3; ++test) {
        lv_image_decoder_dsc_t decoder;
        assert(lv_image_decoder_open(&decoder, selection.lvgl_path, NULL) == LV_RESULT_OK);
        lv_area_t requested = {0, test == 0 ? 0 : bottoms[test] - 15,
                               width - 1, bottoms[test]};
        lv_area_t area = {LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN};
        unsigned calls = 0; bool covered = false;
        while(lv_image_decoder_get_area(&decoder, &requested, &area) == LV_RESULT_OK) {
            /* No successfully returned tile may be wholly below the request. */
            if(area.y1 > requested.y2) {
                fprintf(stderr, "clip_tail request_bottom=%d returned_top=%ld\n",
                        requested.y2, (long)area.y1);
                assert(area.y1 <= requested.y2);
            }
            assert(++calls <= 1280u);
            covered |= area.x2 == width - 1 && area.y2 >= requested.y2;
        }
        assert(covered && katmed_images_last_error() == KATMED_IMAGE_OK);
        if(test == 0) first_calls = calls;
        if(test == 2) full_calls = calls;
        lv_image_decoder_close(&decoder);
        assert(katmed_images_open_file_count() == 0);
    }
    assert(first_calls < full_calls);
    /* A read error before completion is still an error, never normal EOF. */
    lv_image_decoder_dsc_t decoder;
    assert(lv_image_decoder_open(&decoder, selection.lvgl_path, NULL) == LV_RESULT_OK);
    lv_area_t requested = {0, 0, width - 1, height - 1};
    lv_area_t area = {LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN};
    fail_decoder_read = true;
    unsigned calls = 0;
    while(lv_image_decoder_get_area(&decoder, &requested, &area) == LV_RESULT_OK)
        assert(++calls <= 1280u);
    assert(katmed_images_last_error() == KATMED_IMAGE_IO_ERROR);
    fail_decoder_read = false;
    lv_image_decoder_close(&decoder);
    katmed_images_clear_error();
    assert(katmed_images_open_file_count() == 0);
    printf("decoder_clip first_calls=%u full_calls=%u read_failure_preserved=1\n",
           first_calls, full_calls);
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    Host host = {.width = atoi(argv[1]), .height = atoi(argv[2]),
                 .result = UI_SUBMIT_COMMITTED};
    size_t count = (size_t)host.width * host.height;
    host.frame = calloc(count, sizeof(*host.frame));
    uint16_t *buffer = calloc((size_t)host.width * 16, sizeof(*buffer));
    uint16_t *baseline = calloc(count, sizeof(*baseline));
    uint16_t *incremental = calloc(count, sizeof(*incremental));
    assert(host.frame && buffer && baseline && incremental);
    lv_init();
    lv_display_t *display = lv_display_create(host.width, host.height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, (uint32_t)host.width * 32,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_user_data(display, &host);
    katmed_images_register_lvgl_fs();
    FILE *pack=fopen(argv[3],"rb");assert(pack);
    KatmedStorage storage={pack,katmed_generated_catalog.pack_bytes,alarm_pack_read,NULL};
    assert(katmed_images_mount_storage(storage)==KATMED_IMAGE_OK);
    check_decoder_clip(host.width, host.height);
    UiServices services = {.context = &host, .submit = submit, .connected = connected};
    ui_set_services(&services);
    ClockSample clock = {.monotonic_ms = 1000, .domain = 17,
                         .quality = CLOCK_UNAVAILABLE};
    SharedSettings settings = app_default_settings();
    const uint8_t catalog[KATMED_CATALOG_BYTES] = {1};
    AppState state = {0};
    assert(app_init(&state, &settings, &clock, (EventId){1,17,1}, 1, catalog) == APP_OK);
    LocalSettings local = {.clock_style = DIGITAL_LARGE, .orientation = ORIENTATION_0};
    AppState quiet = state; quiet.alarm_active = false;
    ui_render(&quiet, &local, BACKGROUND, &clock); draw();
    memcpy(baseline, host.frame, count * sizeof(*baseline));
    const int top = host.height == 240 ? 30 : host.height == 320 ? 40 : 22;
    const int end = host.height == 240 ? 210 : host.height == 320 ? 280 : 151;
    /* Hand-selected full/partial/full/erase/full phases. Yellow's rational
     * 1.5x clock and red's 2Hz ramp endpoint yield full pixel coverage at
     * the stated samples (4998/5001 and4992 respectively). */
    const uint64_t ages[][5] = {
        {1300,2750,3900,4450,6500},
        {1803800,1804800,1805533,1805967,1807267},
        {7200365,7200665,7200865,7200990,7201365}
    };
    const uint32_t colors[] = {0x1565c0,0xffc107,0xd50000};
    int result=0;
    for(unsigned stage=0;stage<3;++stage) {
        for(unsigned frame=0;frame<5;++frame) {
            clock.monotonic_ms=1000+ages[stage][frame];
            ui_render(&state,&local,ALARM,&clock);
            /* Native setters queue layout. Multiple legitimate model
             * updates may be coalesced before the next renderer runs. */
            if(frame==1 || frame==3) continue;
            draw();
            lv_obj_t *bar=find_alarm_bar(lv_screen_active());assert(bar);
            assert(lv_color_eq(lv_obj_get_style_bg_color(bar,0),lv_color_hex(colors[stage])));
            unsigned holes=0,leaked=0;
            for(int y=0;y<host.height;++y) {
                bool changed=host.frame[y*host.width+5]!=baseline[y*host.width+5];
                if(y>=top && y<end && !changed) ++holes;
                if((y<top || y>=end) && changed) ++leaked;
            }
            printf("stage=%u frame=%u full_height=%d missing_rows=%u outside_rows=%u\n",
                   stage,frame,end-top,holes,leaked);
            if(holes||leaked) result=1;
            /* The normal partial output must equal a fresh rasterization;
             * force-invalidation is a test oracle, never a production fix. */
            memcpy(incremental,host.frame,count*sizeof(*incremental));
            lv_obj_invalidate(lv_screen_active());draw();
            assert(memcmp(incremental,host.frame,count*sizeof(*incremental))==0);
        }
    }
    /* Known blue zero phase clears every interior row, preserving margins. */
    clock.monotonic_ms=1000+7800;
    ui_render(&state,&local,ALARM,&clock);draw();
    for(int y=0;y<host.height;++y)
        assert(host.frame[y*host.width+5]==baseline[y*host.width+5]);
    assert(katmed_images_last_error()==KATMED_IMAGE_OK);
    lv_display_delete(display);katmed_images_unmount();assert(fclose(pack)==0);lv_deinit();
    free(incremental);free(baseline);free(buffer);free(host.frame);
    return result;
}
