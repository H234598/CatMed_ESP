#include "katmed_image_store_cleanup.h"
#include "lfs.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE 4096u
#define BLOCK_COUNT 64u
#define CACHE_SIZE 256u

static uint8_t flash[BLOCK_SIZE * BLOCK_COUNT];

static int bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                   void *buffer, lfs_size_t size)
{
    (void)c;
    if (block >= BLOCK_COUNT || off + size > BLOCK_SIZE) return LFS_ERR_IO;
    memcpy(buffer, &flash[block * BLOCK_SIZE + off], size);
    return 0;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                   const void *buffer, lfs_size_t size)
{
    (void)c;
    if (block >= BLOCK_COUNT || off + size > BLOCK_SIZE) return LFS_ERR_IO;
    const uint8_t *input = buffer;
    uint8_t *output = &flash[block * BLOCK_SIZE + off];
    for (lfs_size_t i = 0; i < size; ++i) {
        if ((output[i] & input[i]) != input[i]) return LFS_ERR_IO;
        output[i] &= input[i];
    }
    return 0;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    (void)c;
    if (block >= BLOCK_COUNT) return LFS_ERR_IO;
    memset(&flash[block * BLOCK_SIZE], 0xff, BLOCK_SIZE);
    return 0;
}

static int bd_sync(const struct lfs_config *c)
{
    (void)c;
    return 0;
}

typedef struct {
    lfs_t *fs;
    lfs_dir_t dir;
    bool open;
} LittlefsContext;

static bool littlefs_next(void *opaque, char *name, size_t capacity,
                          bool *is_regular, bool *at_end)
{
    LittlefsContext *context = opaque;
    struct lfs_info info;
    int result = lfs_dir_read(context->fs, &context->dir, &info);
    if (result < 0) return false;
    if (result == 0) {
        *at_end = true;
        return true;
    }
    size_t length = strlen(info.name);
    if (length + 1u > capacity) return false;
    memcpy(name, info.name, length + 1u);
    *is_regular = info.type == LFS_TYPE_REG;
    *at_end = false;
    return true;
}

static bool littlefs_remove(void *opaque, const char *name)
{
    LittlefsContext *context = opaque;
    char path[64];
    int length = snprintf(path, sizeof(path), "/user-images/%s", name);
    return length > 0 && (size_t)length < sizeof(path) &&
           lfs_remove(context->fs, path) == 0;
}

static void create_file(lfs_t *fs, const char *path)
{
    lfs_file_t file;
    assert(lfs_file_open(fs, &file, path, LFS_O_WRONLY | LFS_O_CREAT) == 0);
    assert(lfs_file_write(fs, &file, "fixture", 7) == 7);
    assert(lfs_file_close(fs, &file) == 0);
}

static void create_bytes(lfs_t *fs, const char *path, const char *bytes)
{
    lfs_file_t file;
    size_t length = strlen(bytes);
    assert(lfs_file_open(fs, &file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) == 0);
    assert(lfs_file_write(fs, &file, bytes, length) == (lfs_ssize_t)length);
    assert(lfs_file_close(fs, &file) == 0);
}

static void assert_bytes(lfs_t *fs, const char *path, const char *expected)
{
    char actual[32] = {0};
    lfs_file_t file;
    size_t length = strlen(expected);
    assert(length < sizeof(actual));
    assert(lfs_file_open(fs, &file, path, LFS_O_RDONLY) == 0);
    assert(lfs_file_read(fs, &file, actual, sizeof(actual)) == (lfs_ssize_t)length);
    assert(memcmp(actual, expected, length) == 0);
    assert(lfs_file_close(fs, &file) == 0);
}

static bool exists(lfs_t *fs, const char *path)
{
    struct lfs_info info;
    return lfs_stat(fs, path, &info) == 0;
}

static void recover(lfs_t *fs)
{
    LittlefsContext context = {.fs = fs};
    assert(lfs_dir_open(fs, &context.dir, "/user-images") == 0);
    context.open = true;
    assert(katmed_image_store_cleanup_entries(&context, littlefs_next,
                                               littlefs_remove));
    assert(lfs_dir_close(fs, &context.dir) == 0);
}

int main(void)
{
    memset(flash, 0xff, sizeof(flash));
    static uint8_t read_cache[CACHE_SIZE], prog_cache[CACHE_SIZE];
    static uint8_t lookahead[CACHE_SIZE];
    struct lfs_config config = {
        .read = bd_read, .prog = bd_prog, .erase = bd_erase, .sync = bd_sync,
        .read_size = 16, .prog_size = 16, .block_size = BLOCK_SIZE,
        .block_count = BLOCK_COUNT, .block_cycles = 500,
        .cache_size = CACHE_SIZE, .lookahead_size = CACHE_SIZE,
        .read_buffer = read_cache, .prog_buffer = prog_cache,
        .lookahead_buffer = lookahead,
    };
    lfs_t fs;
    assert(lfs_format(&fs, &config) == 0);
    assert(lfs_mount(&fs, &config) == 0);
    assert(lfs_mkdir(&fs, "/user-images") == 0);
    create_file(&fs, "/user-images/.upload-0123abcd.tmp");
    create_file(&fs, "/user-images/img-00000001.jpg");
    create_bytes(&fs, "/user-images/.upload-12345678.tmp", "replacement");
    assert(lfs_rename(&fs, "/user-images/.upload-12345678.tmp",
                      "/user-images/img-00000001.jpg") == 0);
    assert_bytes(&fs, "/user-images/img-00000001.jpg", "replacement");
    create_file(&fs, "/user-images/readme.txt");
    assert(lfs_unmount(&fs) == 0); /* Simulated abrupt power loss. */

    assert(lfs_mount(&fs, &config) == 0);
    recover(&fs);
    assert(!exists(&fs, "/user-images/.upload-0123abcd.tmp"));
    assert(exists(&fs, "/user-images/img-00000001.jpg"));
    assert(exists(&fs, "/user-images/readme.txt"));
    recover(&fs); /* Idempotent recovery after the remount. */
    assert(exists(&fs, "/user-images/img-00000001.jpg"));
    assert(lfs_unmount(&fs) == 0);

    assert(lfs_mount(&fs, &config) == 0);
    create_file(&fs, "/user-images/.upload-abcdef12.tmp");
    assert(lfs_mkdir(&fs, "/user-images/.upload-87654321.tmp") == 0);
    LittlefsContext context = {.fs = &fs};
    assert(lfs_dir_open(&fs, &context.dir, "/user-images") == 0);
    assert(!katmed_image_store_cleanup_entries(&context, littlefs_next,
                                                littlefs_remove));
    assert(lfs_dir_close(&fs, &context.dir) == 0);
    assert(exists(&fs, "/user-images/.upload-abcdef12.tmp"));
    assert(exists(&fs, "/user-images/.upload-87654321.tmp"));
    assert(lfs_unmount(&fs) == 0);
    puts("actual LittleFS remount orphan cleanup: PASS");
    return 0;
}
