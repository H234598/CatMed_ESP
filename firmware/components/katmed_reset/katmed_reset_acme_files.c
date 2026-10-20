#ifndef ESP_PLATFORM
#define _POSIX_C_SOURCE 200809L
#endif
#include "katmed_reset_acme_files.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
/* LittleFS has no symlinks; ESP-IDF VFS provides stat(). */
#define acme_lstat stat
#else
#define acme_lstat lstat
#endif

#define ACME_DIRECTORY "/katmed/acme"
#define ACME_PATH_CAPACITY 512u

static const char *const acme_names[] = {
    "account.json", "directory.json", "account.pem", "order.json",
    "certkey.pem", "certificate.pem", "renewal.json",
};

static bool owned_name(const char *name)
{
    for (size_t i = 0; i < sizeof(acme_names) / sizeof(acme_names[0]); ++i) {
        const size_t length = strlen(acme_names[i]);
        if (strcmp(name, acme_names[i]) == 0) return true;
        /* katmed_acme_atomic_open() writes this decimal-suffixed temporary
         * file beside each target before renaming it. */
        if (strlen(name) <= length ||
            strncmp(name, acme_names[i], length) != 0 ||
            strncmp(name + length, ".katmed-", 8) != 0) continue;
        const char *digit = name + length + 8;
        if (!*digit) continue;
        for (; *digit >= '0' && *digit <= '9'; ++digit) { }
        if (!*digit) return true;
    }
    return false;
}

static bool entry_path(const char *directory, const char *name,
                       char path[ACME_PATH_CAPACITY])
{
    const int length = snprintf(path, ACME_PATH_CAPACITY, "%s/%s", directory, name);
    return length > 0 && (size_t)length < ACME_PATH_CAPACITY;
}

static bool scan(DIR *directory, const char *path, bool remove_files)
{
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) return errno == 0;
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        /* An unknown entry makes the whole file stage fail. Preflight before
         * deletion avoids partially clearing a directory with foreign data. */
        if (!owned_name(name)) return false;
        char filename[ACME_PATH_CAPACITY];
        struct stat info;
        if (!entry_path(path, name, filename) || acme_lstat(filename, &info) != 0 ||
            !S_ISREG(info.st_mode)) return false;
        if (remove_files && unlink(filename) != 0) return false;
    }
}

bool katmed_reset_acme_files_at(const char *directory_path)
{
    if (!directory_path || !directory_path[0]) return false;
    struct stat info;
    if (acme_lstat(directory_path, &info) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(info.st_mode)) return false;
    DIR *directory = opendir(directory_path);
    if (!directory) return false;
    bool ok = scan(directory, directory_path, false);
    if (ok) {
        rewinddir(directory);
        ok = scan(directory, directory_path, true);
    }
    if (closedir(directory) != 0) ok = false;
    return ok;
}

bool katmed_reset_acme_files(void)
{
    return katmed_reset_acme_files_at(ACME_DIRECTORY);
}
