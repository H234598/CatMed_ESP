#define _POSIX_C_SOURCE 200809L
#include "katmed_image_store.h"
#include "katmed_images_internal.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const KatmedGeneratedCatalog katmed_generated_catalog = {.profile_count=1};
static unsigned char *jpeg[2];
static KatmedStorage captured_storage;
KatmedImageStatus katmed_images_mount_storage(KatmedStorage storage)
{ captured_storage=storage; return KATMED_IMAGE_OK; }
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label)
{
    static const esp_partition_t p={.size=0x1ca000};
    assert(type==1 && subtype==0x83 && !strcmp(label,"user_images"));return &p;
}
static size_t jpeg_size[2];
static size_t total_bytes = 1024u * 1024u;
static size_t used_bytes;
static int fail_validation;
static bool fail_mutex;
static size_t fail_read_after = (size_t)-1;
static size_t bytes_read;
static unsigned random_id = 0x12345678u;

void test_set_space(size_t total, size_t used) { total_bytes = total; used_bytes = used; }

const KatmedImageVariant *katmed_images_variant(uint32_t index, uint32_t profile)
{
    static KatmedImageVariant variants[2];
    if (index >= 2u || profile != 0u) return NULL;
    variants[index] = (KatmedImageVariant){.offset=index==0?0u:((uint32_t)jpeg_size[0]+3u)&~3u,
        .length=(uint32_t)jpeg_size[index], .width=240u, .height=240u};
    return &variants[index];
}

KatmedImageStatus katmed_images_validate_variant(uint32_t index, uint32_t profile)
{
    return index < 2u && profile == 0u && !fail_validation
        ? KATMED_IMAGE_OK : KATMED_IMAGE_BAD_CATALOG;
}

KatmedImageStatus katmed_images_read_variant(uint32_t index, uint32_t profile,
                                              uint32_t position, uint8_t *dst,
                                              size_t wanted, size_t *actual)
{
    if (index >= 2u || profile != 0u || actual == NULL || position > jpeg_size[index])
        return KATMED_IMAGE_BAD_RANGE;
    if (bytes_read >= fail_read_after) return KATMED_IMAGE_IO_ERROR;
    size_t available = jpeg_size[index] - position;
    if (wanted > available) wanted = available;
    if (fail_read_after != (size_t)-1 && wanted > fail_read_after - bytes_read)
        wanted = fail_read_after - bytes_read;
    memcpy(dst, jpeg[index] + position, wanted);
    *actual = wanted;
    bytes_read += wanted;
    return KATMED_IMAGE_OK;
}

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *config)
{
    assert(!config->format_if_mount_failed);
    return ESP_OK;
}
esp_err_t esp_vfs_littlefs_unregister(const char *label) { (void)label; return ESP_OK; }
esp_err_t esp_littlefs_info(const char *label, size_t *total, size_t *used)
{
    (void)label; *total = total_bytes; *used = used_bytes; return ESP_OK;
}
uint32_t esp_random(void) { return ++random_id; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test"; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t mutex, int ticks) { (void)mutex; (void)ticks; return fail_mutex ? 0 : pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t mutex) { (void)mutex; return pdTRUE; }

static void load_jpeg(const char *path, unsigned slot)
{
    FILE *stream = fopen(path, "rb"); assert(stream);
    assert(fseek(stream, 0, SEEK_END) == 0);
    long length = ftell(stream); assert(length > 0);
    assert(fseek(stream, 0, SEEK_SET) == 0);
    jpeg_size[slot] = (size_t)length;
    jpeg[slot] = malloc(jpeg_size[slot]); assert(jpeg[slot]);
    assert(fread(jpeg[slot], 1u, jpeg_size[slot], stream) == jpeg_size[slot]);
    assert(fclose(stream) == 0);
}

static bool exists(const char *path)
{
    struct stat info;
    return lstat(path, &info) == 0;
}

static void assert_same_file(const char *path, const unsigned char *expected, size_t size)
{
    unsigned char *actual = malloc(size); assert(actual);
    FILE *stream = fopen(path, "rb"); assert(stream);
    assert(fread(actual, 1u, size, stream) == size);
    assert(fgetc(stream) == EOF);
    assert(fclose(stream) == 0);
    assert(memcmp(actual, expected, size) == 0);
    free(actual);
}

static unsigned count_managed(void)
{
    DIR *dir = opendir("/tmp/ke/user-images"); assert(dir);
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
        if (strncmp(entry->d_name, "cat-", 4u) == 0 ||
            strncmp(entry->d_name, "img-", 4u) == 0) ++count;
    closedir(dir);
    return count;
}

bool katmed_source_writer_enter(void) { return true; }
void katmed_source_writer_leave(void) {}

static void seed_file(const char *path, unsigned slot)
{
    FILE *f=fopen(path,"wb"); assert(f);
    assert(fwrite(jpeg[slot],1,jpeg_size[slot],f)==jpeg_size[slot]);
    assert(fclose(f)==0);
}

int main(int argc, char **argv)
{
    assert(argc==3); load_jpeg(argv[1],0); load_jpeg(argv[2],1);
    assert(mkdir("/tmp/ke",0700)==0);
    assert(mkdir("/tmp/ke/factory-images",0700)==0);
    seed_file("/tmp/ke/factory-images/cat-00-0.jpg",0);
    seed_file("/tmp/ke/factory-images/cat-01-0.jpg",1);
    assert(katmed_image_store_mount()==KATMED_IMAGE_OK);
    assert(katmed_images_mount_partition()==KATMED_IMAGE_OK);
    unsigned char crossing[16];size_t actual=0;
    const uint32_t padded=((uint32_t)jpeg_size[0]+3u)&~3u;
    const uint32_t start=(uint32_t)jpeg_size[0]-4u;
    assert(captured_storage.read(NULL,start,crossing,sizeof(crossing),&actual));
    assert(actual==sizeof(crossing));
    for (size_t n=0;n<sizeof(crossing);++n) {
        uint32_t position=start+(uint32_t)n;
        unsigned char expected=position<jpeg_size[0]?jpeg[0][position]:position<padded?0xff:jpeg[1][position-padded];
        assert(crossing[n]==expected);
    }
    assert(captured_storage.read(NULL,17u,crossing,sizeof(crossing),&actual));
    assert(!memcmp(crossing,jpeg[0]+17,sizeof(crossing)));
    assert(captured_storage.read_detailed!=NULL);
    KatmedImageIoFailure detail={0};fail_mutex=true;
    assert(!captured_storage.read_detailed(NULL,0,crossing,sizeof(crossing),&actual,&detail));
    assert(actual==0 && detail.stage==KATMED_IMAGE_IO_VALIDATION_LOCK && detail.status==KATMED_IMAGE_UNAVAILABLE);
    fail_mutex=false;
    /* Close the cached first file, then inject a missing standard file in this test-only mount. */
    assert(captured_storage.read(NULL,padded,crossing,sizeof(crossing),&actual));
    assert(rename("/tmp/ke/factory-images/cat-00-0.jpg","/tmp/ke/factory-images/held.jpg")==0);
    assert(!captured_storage.read_detailed(NULL,0,crossing,sizeof(crossing),&actual,&detail));
    assert(detail.stage==KATMED_IMAGE_IO_FILE_OPEN && detail.status==KATMED_IMAGE_NOT_FOUND);
    assert(rename("/tmp/ke/factory-images/held.jpg","/tmp/ke/factory-images/cat-00-0.jpg")==0);
    assert(captured_storage.read_detailed(NULL,0,crossing,sizeof(crossing),&actual,&detail));
    assert(actual==sizeof(crossing) && detail.stage==KATMED_IMAGE_IO_NONE && detail.status==KATMED_IMAGE_OK);

    KatmedUserImageInfo standard={0}, again={0}, uploaded={0};
    assert(katmed_image_store_import_catalog(0,0,&standard)==KATMED_IMAGE_OK);
    assert(standard.id==UINT32_C(0x80000000));
    assert(katmed_image_store_import_catalog(0,0,&again)==KATMED_IMAGE_OK);
    assert(again.id==standard.id && again.bytes==jpeg_size[0] && count_managed()==0);
    assert(katmed_image_store_import_catalog(0,1,&again)==KATMED_IMAGE_BAD_ID);
    assert(katmed_image_store_remove(standard.id)==KATMED_IMAGE_BAD_ID);
    assert(katmed_image_store_remove(UINT32_C(0xffffffff))==KATMED_IMAGE_BAD_ID);
    assert(katmed_image_store_get(UINT32_C(0x800000ff),&again)==KATMED_IMAGE_BAD_ID);
    KatmedImageReader reader={0};
    assert(katmed_image_store_open_read(standard.id,&reader,&again)==KATMED_IMAGE_OK);
    unsigned char bytes[32]; size_t got=0;
    assert(katmed_image_store_read(reader,bytes,sizeof(bytes),&got)==KATMED_IMAGE_OK);
    assert(got==sizeof(bytes) && !memcmp(bytes,jpeg[0],sizeof(bytes)));
    assert(katmed_image_store_close_read(reader)==KATMED_IMAGE_OK);
    test_set_space(total_bytes,total_bytes);
    /* Referring to a standard file consumes no extra filesystem space. */
    assert(katmed_image_store_import_catalog(1,0,&again)==KATMED_IMAGE_OK);
    KatmedImageUpload upload={0};
    assert(katmed_image_store_upload_begin(jpeg_size[1],&upload)==KATMED_IMAGE_NO_SPACE);
    test_set_space(2u*1024u*1024u,0);
    assert(katmed_image_store_upload_begin(jpeg_size[1],&upload)==KATMED_IMAGE_OK);
    assert(katmed_image_store_upload_write(upload,jpeg[1],jpeg_size[1])==KATMED_IMAGE_OK);
    assert(katmed_image_store_upload_commit(upload,&uploaded)==KATMED_IMAGE_OK);
    assert(!(uploaded.id&UINT32_C(0x80000000)) && count_managed()==1);
    assert(katmed_image_store_list(0,&again)==KATMED_IMAGE_OK && again.id==uploaded.id);
    assert(katmed_image_store_list(1,&again)==KATMED_IMAGE_NOT_FOUND);
    assert(katmed_image_store_open_read(uploaded.id,&reader,&again)==KATMED_IMAGE_OK);
    assert(katmed_image_store_remove(uploaded.id)==KATMED_IMAGE_BUSY);
    assert(katmed_image_store_close_read(reader)==KATMED_IMAGE_OK);
    fail_validation=1;
    assert(katmed_image_store_import_catalog(1,0,&again)==KATMED_IMAGE_BAD_CATALOG);
    fail_validation=0;
    FILE *f=fopen("/tmp/ke/user-images/keep.txt","wb");assert(f);fclose(f);
    assert(katmed_image_store_clear_user_data()==KATMED_IMAGE_IO_ERROR);
    assert(exists("/tmp/ke/user-images/keep.txt"));
    assert(unlink("/tmp/ke/user-images/keep.txt")==0);
    f=fopen("/tmp/ke/user-images/.upload-87654321.tmp","wb");assert(f);fclose(f);
    assert(katmed_image_store_clear_user_data()==KATMED_IMAGE_OK);
    assert(count_managed()==0 && !exists("/tmp/ke/user-images/.upload-87654321.tmp"));
    assert_same_file("/tmp/ke/factory-images/cat-00-0.jpg",jpeg[0],jpeg_size[0]);
    assert_same_file("/tmp/ke/factory-images/cat-01-0.jpg",jpeg[1],jpeg_size[1]);
    assert(katmed_image_store_clear_user_data()==KATMED_IMAGE_OK);
    assert(katmed_image_store_get(standard.id,&again)==KATMED_IMAGE_OK);
    katmed_image_store_unmount();
    assert(unlink("/tmp/ke/factory-images/cat-00-0.jpg")==0);
    assert(unlink("/tmp/ke/factory-images/cat-01-0.jpg")==0);
    assert(rmdir("/tmp/ke/factory-images")==0);
    assert(rmdir("/tmp/ke/user-images")==0);assert(rmdir("/tmp/ke")==0);
    free(jpeg[0]);free(jpeg[1]);
    puts("shared standard/upload reads, protected IDs, no-copy references and reset retention passed");
    return 0;
}
