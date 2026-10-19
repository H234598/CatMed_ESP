#define _POSIX_C_SOURCE 200809L
#include "katmed_image_store.h"
#include "katmed_images_internal.h"
#include "esp_err.h"
#include "esp_littlefs.h"
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

static unsigned char *jpeg[2];
static size_t jpeg_size[2];
static size_t total_bytes = 1024u * 1024u;
static size_t used_bytes;
static int fail_validation;
static size_t fail_read_after = (size_t)-1;
static size_t bytes_read;
static unsigned random_id = 0x12345678u;

void test_set_space(size_t total, size_t used) { total_bytes = total; used_bytes = used; }

const KatmedImageVariant *katmed_images_variant(uint32_t index, uint32_t profile)
{
    static KatmedImageVariant variants[2];
    if (index >= 2u || profile != 0u) return NULL;
    variants[index] = (KatmedImageVariant){.length=(uint32_t)jpeg_size[index], .width=240u, .height=240u};
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
    (void)config;
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
int xSemaphoreTake(SemaphoreHandle_t mutex, int ticks) { (void)mutex; (void)ticks; return pdTRUE; }
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

int main(int argc, char **argv)
{
    assert(argc == 3);
    load_jpeg(argv[1], 0u); load_jpeg(argv[2], 1u);
    assert(mkdir("/tmp/ke", 0700) == 0);
    assert(katmed_image_store_mount() == KATMED_IMAGE_OK);

    KatmedUserImageInfo imported = {0}, repeated = {0};
    assert(katmed_image_store_import_catalog(0u, 0u, &imported) == KATMED_IMAGE_OK);
    assert(imported.id == UINT32_C(0x80000000));
    char cache_path[64];
    assert(snprintf(cache_path, sizeof(cache_path), "/tmp/ke/user-images/cat-00-0.jpg") > 0);
    assert_same_file(cache_path, jpeg[0], jpeg_size[0]);
    assert(katmed_image_store_import_catalog(0u, 0u, &repeated) == KATMED_IMAGE_OK);
    assert(repeated.id == imported.id && repeated.bytes == imported.bytes);
    assert(count_managed() == 1u);

    KatmedImageUpload upload = {0};
    KatmedUserImageInfo uploaded = {0};
    assert(katmed_image_store_upload_begin((uint32_t)jpeg_size[1], &upload) == KATMED_IMAGE_OK);
    assert(katmed_image_store_upload_write(upload, jpeg[1], jpeg_size[1]) == KATMED_IMAGE_OK);
    assert(katmed_image_store_upload_commit(upload, &uploaded) == KATMED_IMAGE_OK);
    char upload_path[64];
    assert(snprintf(upload_path, sizeof(upload_path), "/tmp/ke/user-images/img-%08x.jpg", uploaded.id) > 0);
    assert_same_file(upload_path, jpeg[1], jpeg_size[1]);
    assert(count_managed() == 2u);

    test_set_space(total_bytes, total_bytes);
    assert(katmed_image_store_import_catalog(1u, 0u, &repeated) == KATMED_IMAGE_NO_SPACE);
    assert(count_managed() == 2u && !exists("/tmp/ke/user-images/cat-01-0.jpg"));
    test_set_space(1024u * 1024u, 0u);

    fail_validation = 1;
    assert(katmed_image_store_import_catalog(1u, 0u, &repeated) == KATMED_IMAGE_BAD_CATALOG);
    fail_validation = 0;
    assert(count_managed() == 2u);

    bytes_read = 0u; fail_read_after = 700u;
    assert(katmed_image_store_import_catalog(1u, 0u, &repeated) == KATMED_IMAGE_IO_ERROR);
    fail_read_after = (size_t)-1;
    assert(count_managed() == 2u && !exists("/tmp/ke/user-images/cat-01-0.jpg"));

    FILE *orphan = fopen("/tmp/ke/user-images/.upload-87654321.tmp", "wb"); assert(orphan);
    assert(fwrite("temp", 1u, 4u, orphan) == 4u); assert(fclose(orphan) == 0);
    FILE *unrelated = fopen("/tmp/ke/user-images/keep.txt", "wb"); assert(unrelated);
    assert(fwrite("keep", 1u, 4u, unrelated) == 4u); assert(fclose(unrelated) == 0);
    FILE *immutable = fopen("/tmp/ke/immutable-pack.jpg", "wb"); assert(immutable);
    assert(fwrite(jpeg[0], 1u, jpeg_size[0], immutable) == jpeg_size[0]);
    assert(fclose(immutable) == 0);
    assert(katmed_image_store_clear_user_data() == KATMED_IMAGE_IO_ERROR);
    assert(exists("/tmp/ke/user-images/keep.txt"));
    assert_same_file("/tmp/ke/immutable-pack.jpg", jpeg[0], jpeg_size[0]);
    assert(unlink("/tmp/ke/user-images/keep.txt") == 0);
    assert(katmed_image_store_clear_user_data() == KATMED_IMAGE_OK);
    assert(!exists(cache_path) && !exists(upload_path));
    assert(!exists("/tmp/ke/user-images/.upload-87654321.tmp"));
    assert_same_file("/tmp/ke/immutable-pack.jpg", jpeg[0], jpeg_size[0]);

    katmed_image_store_unmount();
    free(jpeg[0]); free(jpeg[1]);
    assert(unlink("/tmp/ke/immutable-pack.jpg") == 0);
    assert(rmdir("/tmp/ke/user-images") == 0);
    assert(rmdir("/tmp/ke") == 0);
    puts("real catalog import, retention, failure cleanup and reset passed");
    return 0;
}
