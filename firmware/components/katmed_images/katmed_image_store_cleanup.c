#ifndef ESP_PLATFORM
#define _POSIX_C_SOURCE 200809L
#endif
#include "katmed_image_store_cleanup.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#define cleanup_lstat stat
#else
#define cleanup_lstat lstat
#endif

bool katmed_image_store_temporary_name(const char *name)
{
    if (!name || strlen(name) != 20u || memcmp(name, ".upload-", 8u) != 0 ||
        memcmp(name + 16u, ".tmp", 4u) != 0) return false;
    for (size_t i = 8u; i < 16u; ++i) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

bool katmed_image_store_cleanup_entries(void *context,
                                        KatmedImageCleanupNext next,
                                        KatmedImageCleanupRemove remove)
{
    if (!next || !remove) return false;
    for (;;) {
        char name[256] = {0};
        bool regular = false, at_end = false;
        if (!next(context, name, sizeof(name), &regular, &at_end)) return false;
        if (at_end) return true;
        if (!katmed_image_store_temporary_name(name)) continue;
        if (!regular || !remove(context, name)) return false;
    }
}

typedef struct {
    DIR *directory;
    const char *path;
} PosixContext;

static bool posix_next(void *opaque, char *name, size_t capacity,
                       bool *is_regular, bool *at_end)
{
    PosixContext *context = opaque;
    errno = 0;
    struct dirent *entry = readdir(context->directory);
    if (!entry) {
        *at_end = true;
        return errno == 0;
    }
    size_t length = strlen(entry->d_name);
    if (length + 1u > capacity) return false;
    memcpy(name, entry->d_name, length + 1u);
    *at_end = false;
    size_t path_length = strlen(context->path) + 1u + length + 1u;
    char path[path_length];
    int written = snprintf(path, path_length, "%s/%s", context->path,
                           entry->d_name);
    struct stat info;
    if (written <= 0 || (size_t)written >= path_length ||
        cleanup_lstat(path, &info) != 0) return false;
    *is_regular = S_ISREG(info.st_mode);
    return true;
}

static bool posix_remove(void *opaque, const char *name)
{
    PosixContext *context = opaque;
    size_t path_length = strlen(context->path) + 1u + strlen(name) + 1u;
    char path[path_length];
    int written = snprintf(path, path_length, "%s/%s", context->path, name);
    struct stat info;
    return written > 0 && (size_t)written < path_length &&
           cleanup_lstat(path, &info) == 0 && S_ISREG(info.st_mode) && unlink(path) == 0;
}

bool katmed_image_store_cleanup_orphan_uploads(const char *directory_path)
{
    if (!directory_path || !directory_path[0]) return false;
    DIR *directory = opendir(directory_path);
    if (!directory) return false;
    PosixContext context = {.directory = directory, .path = directory_path};
    bool ok = katmed_image_store_cleanup_entries(&context, posix_next,
                                                  posix_remove);
    closedir(directory);
    return ok;
}
