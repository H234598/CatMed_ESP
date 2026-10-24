/* Exercise actual UI objects, LVGL rasterizer and partial RGB565 flush. */
#define katmed_images_present unused_alarm_synthetic_present
#define main existing_surface_main
#include "check_ui_surface.c"
#undef main
#undef katmed_images_present
KatmedImageStatus katmed_images_present(lv_obj_t *, lv_obj_t *, uint32_t, Orientation);
#include "katmed_images_internal.h"
#include "katmed_images_hash.h"
#include "src/image/lv_image_decoder_private.h"

/* Mirror actual cache writes/seeks into the same native LittleFS core as C3.
 * RAM block device measures work, never host elapsed time or real flash. */
#include "lfs.h"
static uint8_t io_flash[512u * 4096u];
static uint64_t io_read_bytes, io_prog_bytes, io_erases;
static bool io_mirror;
static uint8_t io_expected[172812];
static size_t io_expected_size;
static lfs_t io_fs;
static lfs_file_t io_file;
static int io_read(const struct lfs_config *c,lfs_block_t b,lfs_off_t o,void *p,lfs_size_t n) {
    (void)c;assert(b<512 && o+n<=4096);memcpy(p,io_flash+b*4096+o,n);io_read_bytes+=n;return 0;
}
static int io_prog(const struct lfs_config *c,lfs_block_t b,lfs_off_t o,const void *p,lfs_size_t n) {
    (void)c;assert(b<512 && o+n<=4096);const uint8_t *in=p;
    for(unsigned i=0;i<n;++i) {assert((io_flash[b*4096+o+i]&in[i])==in[i]);io_flash[b*4096+o+i]&=in[i];}
    io_prog_bytes+=n;return 0;
}
static int io_erase(const struct lfs_config *c,lfs_block_t b) {(void)c;assert(b<512);memset(io_flash+b*4096,255,4096);++io_erases;return 0;}
static int io_sync(const struct lfs_config *c) {(void)c;return 0;}
static const struct lfs_config io_cfg={.read=io_read,.prog=io_prog,.erase=io_erase,.sync=io_sync,
 .read_size=128,.prog_size=128,.block_size=4096,.block_count=512,.block_cycles=512,.cache_size=512,.lookahead_size=128};
static void io_begin(void) {
 memset(io_flash,255,sizeof(io_flash));assert(lfs_format(&io_fs,&io_cfg)==0);assert(lfs_mount(&io_fs,&io_cfg)==0);
 assert(lfs_file_open(&io_fs,&io_file,"cache",LFS_O_RDWR|LFS_O_CREAT|LFS_O_TRUNC)==0);
 io_read_bytes=io_prog_bytes=io_erases=0;io_mirror=true;
}
static void io_end(unsigned width,unsigned height) {
 io_mirror=false;
 assert(lfs_file_seek(&io_fs,&io_file,0,LFS_SEEK_SET)==0);
 uint8_t actual[512];
 for(size_t pos=0;pos<io_expected_size;) {
  size_t n=io_expected_size-pos<sizeof(actual)?io_expected_size-pos:sizeof(actual);
  assert(lfs_file_read(&io_fs,&io_file,actual,n)==(lfs_ssize_t)n);
  assert(memcmp(io_expected+pos,actual,n)==0);pos+=n;
 }
 assert(lfs_file_close(&io_fs,&io_file)==0);assert(lfs_unmount(&io_fs)==0);
 printf("native_littlefs_cache_work read=%llu prog=%llu erase=%llu payload=%u\n",(unsigned long long)io_read_bytes,(unsigned long long)io_prog_bytes,(unsigned long long)io_erases,width*height*3);fflush(stdout);
 assert(io_prog_bytes < (uint64_t)width*height*3*4);
}

static bool fail_decoder_read;
static uint64_t pack_reads, cache_bytes, hash_starts;
bool __real_katmed_images_hash_start(KatmedImageHash *);
bool __wrap_katmed_images_hash_start(KatmedImageHash *hash) {
    ++hash_starts; return __real_katmed_images_hash_start(hash);
}
static FILE *source_file;
static bool fail_cache_write, corrupt_cache_read;
static bool fail_band_alloc, band_alloc_failed;
static bool constrained_cache_alloc;
static unsigned band_alloc_until_fail;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t bytes) {
    if(constrained_cache_alloc && bytes>10240u) return NULL;
    if(fail_band_alloc && bytes>=516u && bytes<=960u && band_alloc_until_fail && --band_alloc_until_fail==0) {
        band_alloc_failed=true; return NULL;
    }
    return __real_malloc(bytes);
}
enum BinFault { BIN_FAULT_NONE, BIN_OPEN_CONTEXT, BIN_OPEN_FILE, BIN_AREA_CONTEXT, BIN_AREA_DATA };
static enum BinFault bin_fault;
static bool bin_allocation_scope, bin_fault_hit;
static unsigned allocation_until_fail;
static bool jpeg_open_alloc_scope, jpeg_open_alloc_hit;
static uint64_t reads_in_failed_call;
void *__real_lv_malloc_core(size_t);
void *__wrap_lv_malloc_core(size_t bytes) {
    if(jpeg_open_alloc_scope && bytes==4096u) { jpeg_open_alloc_hit=true; return NULL; }
    if(bin_allocation_scope && allocation_until_fail && --allocation_until_fail==0) {
        bin_fault_hit=true; return NULL;
    }
    return __real_lv_malloc_core(bytes);
}
static lv_image_decoder_open_f_t actual_bin_open;
static lv_image_decoder_get_area_cb_t actual_bin_area;
static lv_result_t fault_bin_open(lv_image_decoder_t *dec,lv_image_decoder_dsc_t *dsc) {
    bool inject=!bin_fault_hit && (bin_fault==BIN_OPEN_CONTEXT || bin_fault==BIN_OPEN_FILE);
    if(inject) {allocation_until_fail=bin_fault==BIN_OPEN_CONTEXT?1:2; reads_in_failed_call=0;}
    bin_allocation_scope=inject;
    lv_result_t result=actual_bin_open(dec,dsc);
    bin_allocation_scope=false;
    return result;
}
static lv_result_t fault_bin_area(lv_image_decoder_t *dec,lv_image_decoder_dsc_t *dsc,const lv_area_t *full,lv_area_t *area) {
    bool inject=!bin_fault_hit && (bin_fault==BIN_AREA_CONTEXT || bin_fault==BIN_AREA_DATA);
    if(inject) {allocation_until_fail=bin_fault==BIN_AREA_CONTEXT?1:2; reads_in_failed_call=0;}
    bin_allocation_scope=inject;
    lv_result_t result=actual_bin_area(dec,dsc,full,area);
    bin_allocation_scope=false;
    return result;
}
size_t __real_fread(void *, size_t, size_t, FILE *);
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
size_t __wrap_fread(void *out, size_t size, size_t count, FILE *file) {
    if(bin_allocation_scope) ++reads_in_failed_call;
    size_t got=__real_fread(out,size,count,file);
    if(file!=source_file) {
        cache_bytes+=got*size;
        if(corrupt_cache_read && got != 0 && size != 0) { ((uint8_t *)out)[0]^=1; corrupt_cache_read=false; }
    }
    return got;
}
size_t __wrap_fwrite(const void *in, size_t size, size_t count, FILE *file) {
    if(fail_cache_write) return 0;
    if(io_mirror && size && count) {
        long pos=ftell(file);assert(pos>=0);
        assert((size_t)pos+size*count<=sizeof(io_expected));
        memcpy(io_expected+pos,in,size*count);
        if((size_t)pos+size*count>io_expected_size) io_expected_size=(size_t)pos+size*count;
        assert(lfs_file_seek(&io_fs,&io_file,pos,LFS_SEEK_SET)==pos);
        assert(lfs_file_write(&io_fs,&io_file,in,size*count)==(lfs_ssize_t)(size*count));
    }
    return __real_fwrite(in,size,count,file);
}
static lv_image_decoder_get_area_cb_t upstream_jpeg;
static uint64_t jpeg_areas;
static unsigned clipped_tail_mcus;
static lv_result_t count_jpeg(lv_image_decoder_t *dec,lv_image_decoder_dsc_t *dsc,const lv_area_t *full,lv_area_t *area) {
    ++jpeg_areas;
    lv_result_t result=upstream_jpeg(dec,dsc,full,area);
    if(result==LV_RESULT_OK && dsc->header.h==172 && area->y2==171 &&
       lv_area_get_height(area)%8!=0) ++clipped_tail_mcus;
    return result;
}
static lv_obj_t *background(lv_display_t *display) {
    lv_obj_t *screen=lv_display_get_screen_active(display);
    for(uint32_t i=0;i<lv_obj_get_child_count(screen);++i) {
        lv_obj_t *obj=lv_obj_get_child(screen,i);
        if(lv_obj_check_type(obj,&lv_image_class)) return obj;
    }
    return NULL;
}
static bool alarm_pack_read(void *ctx,uint32_t off,uint8_t *out,size_t want,size_t *got){
    FILE *f=ctx; *got=0; ++pack_reads;
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
    /* Derive geometry from the actual native JPEG, never UI layout alone. */
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

static lv_image_decoder_open_f_t diag_original_open;
static lv_image_decoder_get_area_cb_t diag_original_area;
static unsigned jpeg_fault, diag_successful_areas;
static lv_result_t diag_open(lv_image_decoder_t *dec,lv_image_decoder_dsc_t *dsc) {
    jpeg_open_alloc_scope=jpeg_fault==1;
    lv_result_t result=diag_original_open(dec,dsc);
    jpeg_open_alloc_scope=false;return result;
}
static lv_result_t diag_area(lv_image_decoder_t *dec,lv_image_decoder_dsc_t *dsc,
                             const lv_area_t *full,lv_area_t *area) {
    fail_decoder_read=jpeg_fault==2 && diag_successful_areas>=5;
    lv_result_t result=diag_original_area(dec,dsc,full,area);
    if(result==LV_RESULT_OK) ++diag_successful_areas;
    fail_decoder_read=false;return result;
}
static void check_jpeg_diagnostics(Orientation orientation) {
    lv_image_decoder_t *dec=NULL;
    while((dec=lv_image_decoder_get_next(dec))) if(dec->name && strcmp(dec->name,"TJPGD")==0) break;
    assert(dec);diag_original_open=dec->open_cb;diag_original_area=dec->get_area_cb;
    lv_image_decoder_set_open_cb(dec,diag_open);lv_image_decoder_set_get_area_cb(dec,diag_area);
    lv_obj_t *image=lv_image_create(lv_screen_active()),*fallback=lv_label_create(lv_screen_active());
    uint32_t previous=0;
    for(unsigned fault=0;fault<3;++fault) {
        jpeg_fault=fault;diag_successful_areas=0;assert(katmed_images_present(image,fallback,0,orientation)==
            (fault?KATMED_IMAGE_IO_ERROR:KATMED_IMAGE_OK));
        KatmedImageTimingSample sample=katmed_images_jpeg_decode_timing();
        assert(sample.measured && sample.sequence && sample.sequence!=previous);previous=sample.sequence;
        assert(sample.decode_stage==(fault==1?KATMED_JPEG_OPEN_FAILED: fault==2?KATMED_JPEG_AREA_FAILED:KATMED_JPEG_COMPLETE));
        assert(fault==1 ? sample.decoded_areas==0 : sample.decoded_areas>0);
        assert(sample.decoded_areas<=1280 && katmed_images_open_file_count()==0);
        if(fault==1) assert(jpeg_open_alloc_hit);
        printf("jpeg_diagnostic_fault=%u stage=%u mcus=%u closed=1\n",fault,sample.decode_stage,sample.decoded_areas);
    }
    jpeg_fault=0;
    assert(katmed_images_present(image,fallback,KATMED_IMAGE_COUNT,orientation)==KATMED_IMAGE_BAD_ID);
    assert(!katmed_images_jpeg_decode_timing().measured && katmed_images_jpeg_decode_timing().decode_stage==KATMED_JPEG_NONE);
    assert(katmed_images_present(image,fallback,0,orientation)==KATMED_IMAGE_OK);
    assert(katmed_images_jpeg_decode_timing().decode_stage==KATMED_JPEG_COMPLETE);
    lv_image_decoder_set_open_cb(dec,diag_original_open);lv_image_decoder_set_get_area_cb(dec,diag_original_area);
    lv_obj_delete(image);lv_obj_delete(fallback);
}

int main(int argc, char **argv)
{
    assert(argc == 4 || argc == 5);
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
    FILE *pack=fopen(argv[3],"rb");assert(pack);source_file=pack;
    KatmedStorage storage={pack,katmed_generated_catalog.pack_bytes,alarm_pack_read,NULL};
    assert(katmed_images_mount_storage(storage)==KATMED_IMAGE_OK);
    if(argc==5) {
        check_jpeg_diagnostics(host.width>host.height?ORIENTATION_90:ORIENTATION_0);
        lv_display_delete(display);katmed_images_unmount();fclose(pack);lv_deinit();
        free(incremental);free(baseline);free(buffer);free(host.frame);return 0;
    }
    check_decoder_clip(host.width, host.height);
    lv_image_decoder_t *dec=NULL;
    while((dec=lv_image_decoder_get_next(dec))) if(dec->name && strcmp(dec->name,"TJPGD")==0) {
        upstream_jpeg=dec->get_area_cb;lv_image_decoder_set_get_area_cb(dec,count_jpeg);break;
    }
    assert(upstream_jpeg);
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
    io_begin(); constrained_cache_alloc=true;
    ui_render(&quiet, &local, BACKGROUND, &clock);
    constrained_cache_alloc=false;
    io_end(host.width,host.height); draw();
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
            pack_reads=cache_bytes=jpeg_areas=hash_starts=0;
            draw();
            printf("render_pack_reads=%llu cache_bytes=%llu jpeg_areas=%llu\n",(unsigned long long)pack_reads,(unsigned long long)cache_bytes,(unsigned long long)jpeg_areas);
            assert(jpeg_areas==0 && hash_starts==1);
            assert(pack_reads==0); /* A refresh must not reopen/redecode the JPEG per DMA slice. */
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
            /* Independent original-JPEG rasterization, not another cache oracle. */
            lv_obj_t *bg=background(display);assert(bg);
            char cached_path[64];snprintf(cached_path,sizeof(cached_path),"%s",(const char *)lv_image_get_src(bg));
            assert(strncmp(cached_path,"K:/active-",10)==0);
            KatmedImageSelection original;assert(katmed_images_select(state.image_id,local.orientation,&original)==KATMED_IMAGE_OK);
            lv_image_set_src(bg,original.lvgl_path);lv_obj_invalidate(lv_screen_active());draw();
            assert(memcmp(incremental,host.frame,count*sizeof(*incremental))==0);
            lv_image_set_src(bg,cached_path);lv_obj_invalidate(lv_screen_active());draw();
        }
    }
    /* Known blue zero phase clears every interior row, preserving margins. */
    clock.monotonic_ms=1000+7800;
    ui_render(&state,&local,ALARM,&clock);draw();
    for(int y=0;y<host.height;++y)
        assert(host.frame[y*host.width+5]==baseline[y*host.width+5]);
    assert(katmed_images_last_error()==KATMED_IMAGE_OK);
    lv_obj_t *bg=background(display);assert(bg);
    lv_obj_t *fallback=NULL;
    for(uint32_t i=0;i<lv_obj_get_child_count(lv_screen_active());++i) {
        lv_obj_t *obj=lv_obj_get_child(lv_screen_active(),i);
        if(lv_obj_check_type(obj,&lv_label_class) && lv_obj_get_width(obj)==host.width) {fallback=obj;break;}
    }
    assert(fallback);
    /* Orientation changes rebuild the one scene; compare to original JPEG. */
    assert(katmed_images_present(bg,fallback,0,ORIENTATION_90)==KATMED_IMAGE_OK);draw();
#ifdef KATMED_ALARM_C6_EDGE_TEST
    assert(strncmp((const char *)lv_image_get_src(bg),"K:/active-",10)==0);
    assert(clipped_tail_mcus>1);
    printf("native172_clipped_tail_mcus=%u cache_retained=1\n",clipped_tail_mcus);
#endif
    memcpy(incremental,host.frame,count*sizeof(*incremental));
    KatmedImageSelection turned;assert(katmed_images_select(0,ORIENTATION_90,&turned)==KATMED_IMAGE_OK);
#ifdef KATMED_ALARM_C6_EDGE_TEST
    assert(turned.width==320 && turned.height==172);
#endif
    lv_image_set_src(bg,turned.lvgl_path);lv_obj_invalidate(lv_screen_active());draw();
    assert(memcmp(incremental,host.frame,count*sizeof(*incremental))==0);
    /* Optional bounded MCU-band allocation must not block first presentation. */
    for(unsigned fail_at=1;fail_at<=8;fail_at+=7) {
    fail_band_alloc=true; band_alloc_failed=false; band_alloc_until_fail=fail_at;
    assert(katmed_images_present(bg,fallback,0,ORIENTATION_0)==KATMED_IMAGE_OK);
    fail_band_alloc=false;
    assert(band_alloc_failed && !lv_obj_is_hidden(bg));
    assert(strncmp((const char *)lv_image_get_src(bg),"K:/active-",10)!=0);
    assert(katmed_images_open_file_count()==0);
    puts("cache_band_allocation_failure_original_JPEG=1");
    }
    /* Optional cache failure preserves the supported original JPEG path. */
    fail_cache_write=true;
    assert(katmed_images_present(bg,fallback,0,local.orientation)==KATMED_IMAGE_OK);
    fail_cache_write=false;
    assert(strncmp(lv_image_get_src(bg),"K:/active-",10)!=0);
    draw();assert(katmed_images_last_error()==KATMED_IMAGE_OK);
    assert(katmed_images_present(bg,fallback,0,local.orientation)==KATMED_IMAGE_OK);
    assert(strncmp(lv_image_get_src(bg),"K:/active-",10)==0);
    /* A damaged derived file is rejected, not trusted via the old success flag. */
    corrupt_cache_read=true;lv_obj_invalidate(lv_screen_active());draw();
    assert(!corrupt_cache_read && lv_obj_is_hidden(bg) && !lv_obj_is_hidden(fallback));
    assert(katmed_images_last_error()!=KATMED_IMAGE_OK);
    assert(katmed_images_present(bg,fallback,1,local.orientation)==KATMED_IMAGE_OK);draw();
    assert(!lv_obj_is_hidden(bg) && katmed_images_last_error()==KATMED_IMAGE_OK);
    /* Remounting invalidates even the same ID and virtual filename. */
    assert(katmed_images_mount_storage(storage)==KATMED_IMAGE_OK);
    lv_obj_invalidate(lv_screen_active());draw();assert(lv_obj_is_hidden(bg));
    assert(katmed_images_present(bg,fallback,0,local.orientation)==KATMED_IMAGE_OK);draw();
    assert(!lv_obj_is_hidden(bg) && katmed_images_open_file_count()==0);
    /* Exercise actual native allocation boundaries, after valid cache publication.
     * The scope wrapper delegates through the production monitors to unmodified
     * BIN code; only its allocator is failed. No FS-read failure is substituted. */
    lv_image_decoder_t *bin=NULL;
    while((bin=lv_image_decoder_get_next(bin))) if(bin->name && strcmp(bin->name,"BIN")==0) break;
    assert(bin);
    actual_bin_open=bin->open_cb;actual_bin_area=bin->get_area_cb;
    lv_image_decoder_set_open_cb(bin,fault_bin_open);
    lv_image_decoder_set_get_area_cb(bin,fault_bin_area);
    for(enum BinFault fault=BIN_OPEN_CONTEXT;fault<=BIN_AREA_DATA;++fault) {
        bin_fault=BIN_FAULT_NONE;
        assert(katmed_images_present(bg,fallback,0,local.orientation)==KATMED_IMAGE_OK);draw();
        assert(katmed_images_last_error()==KATMED_IMAGE_OK && !lv_obj_is_hidden(bg));
        bin_fault=fault;bin_fault_hit=false;
        lv_obj_invalidate(lv_screen_active());draw();
        assert(bin_fault_hit && reads_in_failed_call==0);
        assert(katmed_images_last_error()==KATMED_IMAGE_IO_ERROR);
        assert(lv_obj_is_hidden(bg) && !lv_obj_is_hidden(fallback));
        assert(lv_image_get_src(bg)==NULL && katmed_images_open_file_count()==0);
        printf("native_bin_fault=%d hit=1 pre_read=1 neutral_fallback=1 closed=1\n",fault);
    }
    bin_fault=BIN_FAULT_NONE;
    assert(katmed_images_present(bg,fallback,0,local.orientation)==KATMED_IMAGE_OK);draw();
    lv_image_decoder_dsc_t normal;
    assert(lv_image_decoder_open(&normal,lv_image_get_src(bg),NULL)==LV_RESULT_OK);
    lv_area_t clip={0,0,(int32_t)normal.header.w-1,15};
    lv_area_t part={LV_COORD_MIN,LV_COORD_MIN,LV_COORD_MIN,LV_COORD_MIN};unsigned rows=0;
    while(lv_image_decoder_get_area(&normal,&clip,&part)==LV_RESULT_OK) ++rows;
    assert(rows==16 && katmed_images_last_error()==KATMED_IMAGE_OK);
    lv_image_decoder_close(&normal);
    assert(katmed_images_open_file_count()==0 && !lv_obj_is_hidden(bg));
    printf("native_bin_normal_eof=1 rows=16 no_false_error=1 recovery=1\n");
    printf("cache_write_failure_fallback=1 corruption_rejected=1 image_change=1 remount_rejected=1\n");
    lv_display_delete(display);katmed_images_unmount();assert(fclose(pack)==0);lv_deinit();
    free(incremental);free(baseline);free(buffer);free(host.frame);
    return result;
}
