#include "katmed_source_export.h"

#include "nvs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
/* ESP-IDF 6.1 VFS has stat/fstat but no lstat. LittleFS has no symlinks. */
#define source_stat stat
#else
/* The host fixture must reject symlinks before opening a source file. */
#define source_stat lstat
#endif

#ifdef KATMED_SOURCE_EXPORT_HOST_TEST
#define FS_ROOT KATMED_SOURCE_EXPORT_HOST_TEST
#else
#define FS_ROOT "/katmed"
#endif

#define MAX_FILES 128u
#define MAX_FILE (128u * 1024u)
#define MAX_CONTENT (0x40000u - 8192u)
#define MAX_NVS_BLOB 4096u

typedef struct { const char *name; const char *key; } Key;
static const Key keys[] = {
    {"katmed", "state"}, {"katmed", "mesh_history"},
    {"katmed_web", "auth"}, {"katmed_web", "identity"},
    {"katmed_reset", "journal"}
};
static const char *const acme_names[] = {
    "account.json", "directory.json", "account.pem", "order.json",
    "certkey.pem", "certificate.pem"
};

static void wipe(uint8_t *buffer, size_t length)
{
    volatile uint8_t *p = buffer;
    while (length--) *p++ = 0;
}

static bool image_name(const char *s)
{
    if (strlen(s) != 16 || strncmp(s, "img-", 4) != 0 ||
        strcmp(s + 12, ".jpg") != 0) return false;
    for (unsigned i = 4; i < 12; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

static bool catalog_name(const char *s)
{
    if (strlen(s) != 12 || strncmp(s, "cat-", 4) != 0 ||
        s[6] != '-' || (s[7] != '0' && s[7] != '1') ||
        strcmp(s + 8, ".jpg") != 0) return false;
    for (unsigned i = 4; i < 6; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

static bool acme_name(const char *s)
{
    for (unsigned i = 0; i < sizeof(acme_names) / sizeof(acme_names[0]); ++i)
        if (strcmp(s, acme_names[i]) == 0) return true;
    return false;
}

static int key_index(const nvs_entry_info_t *info)
{
    if (info->type != NVS_TYPE_BLOB) return -1;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (strcmp(info->namespace_name, keys[i].name) == 0 &&
            strcmp(info->key, keys[i].key) == 0) return (int)i;
    return -1;
}

/* Enumerate the whole default partition, including SDK and historical keys.
 * Unknown entries are a hard error: no implicit semantic omission. */
static bool scan_nvs(uint8_t *mask)
{
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", NULL, NVS_TYPE_ANY, &it);
    *mask = 0;
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        if (nvs_entry_info(it, &info) != ESP_OK) {
            nvs_release_iterator(it);
            return false;
        }
        int index = key_index(&info);
        if (index < 0 || (*mask & (1u << index))) {
            nvs_release_iterator(it);
            return false;
        }
        *mask |= (uint8_t)(1u << index);
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return err == ESP_ERR_NVS_NOT_FOUND && (*mask & 1u);
}

static bool regular_file(const char *path, size_t *size)
{
    struct stat st;
    if (source_stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || st.st_size > MAX_FILE) return false;
    *size = (size_t)st.st_size;
    return true;
}

static bool scan_directory(const char *name, size_t *count, size_t *content,
                           bool *present)
{
    char directory[128];
    int n = snprintf(directory, sizeof(directory), FS_ROOT "/%s", name);
    if (n <= 0 || (size_t)n >= sizeof(directory)) return false;
    struct stat dir_info;
    if (source_stat(directory, &dir_info) != 0) {
        *present = false;
        return errno == ENOENT && strcmp(name, "acme") == 0;
    }
    if (!S_ISDIR(dir_info.st_mode)) return false;
    DIR *dir = opendir(directory);
    *present = dir != NULL;
    if (!dir) return false;
    bool ok = true;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        const char *file = entry->d_name;
        if (strcmp(file, ".") == 0 || strcmp(file, "..") == 0) { errno = 0; continue; }
        if (!(strcmp(name, "acme") == 0 ? acme_name(file) :
              (image_name(file) || catalog_name(file)))) { ok = false; break; }
        char path[160];
        size_t size;
        n = snprintf(path, sizeof(path), "%s/%s", directory, file);
        if (n <= 0 || (size_t)n >= sizeof(path) || !regular_file(path, &size) ||
            ++*count > MAX_FILES || size > MAX_CONTENT - *content) {
            ok = false; break;
        }
        *content += size;
        errno = 0;
    }
    if (errno != 0) ok = false;
    if (closedir(dir) != 0) ok = false;
    return ok;
}

static bool scan_files(size_t *content, bool *acme_present, size_t nvs_count)
{
    DIR *root = opendir(FS_ROOT);
    if (!root) return false;
    bool ok = true;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(root)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) { errno = 0; continue; }
        if (strcmp(entry->d_name, "user-images") != 0 &&
            strcmp(entry->d_name, "acme") != 0) { ok = false; break; }
        errno = 0;
    }
    if (errno != 0) ok = false;
    if (closedir(root) != 0) ok = false;
    size_t count = nvs_count;
    bool images_present = false;
    return ok && scan_directory("user-images", &count, content, &images_present) &&
        images_present && scan_directory("acme", &count, content, acme_present);
}

static bool send_nvs(const KatmedSourceExportSink *sink, const Key *key,
                     uint8_t buffer[MAX_NVS_BLOB])
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(key->name, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;
    size_t length = 0;
    err = nvs_get_blob(handle, key->key, NULL, &length);
    if (err != ESP_OK || length == 0 || length > MAX_NVS_BLOB) {
        nvs_close(handle);
        return false;
    }
    size_t actual = length;
    err = nvs_get_blob(handle, key->key, buffer, &actual);
    nvs_close(handle);
    if (err != ESP_OK || actual != length) return false;
    if (strcmp(key->name, "katmed_reset") == 0 &&
        (length != 2 || buffer[0] != 0 || buffer[1] > 5)) return false;
    char logical[48];
    int n = snprintf(logical, sizeof(logical), "nvs/%s/%s", key->name, key->key);
    return n > 0 && (size_t)n < sizeof(logical) &&
        sink->begin(sink->context, logical, length) &&
        sink->bytes(sink->context, buffer, length) && sink->end(sink->context);
}

static bool send_directory(const KatmedSourceExportSink *sink, const char *name,
                           bool expected,
                           uint8_t buffer[512])
{
    char directory[128];
    int n = snprintf(directory, sizeof(directory), FS_ROOT "/%s", name);
    if (n <= 0 || (size_t)n >= sizeof(directory)) return false;
    DIR *dir = opendir(directory);
    if (!dir) return !expected && errno == ENOENT;
    if (!expected) { closedir(dir); return false; }
    bool ok = true;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        const char *file = entry->d_name;
        if (strcmp(file, ".") == 0 || strcmp(file, "..") == 0) { errno = 0; continue; }
        char path[160], logical[80];
        size_t size;
        n = snprintf(path, sizeof(path), "%s/%s", directory, file);
        int m = snprintf(logical, sizeof(logical), "littlefs/%s/%s",
                         strcmp(name, "acme") == 0 ? "acme" : "user-images", file);
        if (!(strcmp(name, "acme") == 0 ? acme_name(file) :
              (image_name(file) || catalog_name(file))) ||
            n <= 0 || (size_t)n >= sizeof(path) || m <= 0 ||
            (size_t)m >= sizeof(logical) || !regular_file(path, &size)) { ok = false; break; }
        int fd = open(path, O_RDONLY | O_NOFOLLOW);
        if (fd < 0) { ok = false; break; }
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size != (off_t)size || !sink->begin(sink->context, logical, size)) ok = false;
        size_t remaining = size;
        while (ok && remaining) {
            size_t wanted = remaining < 512 ? remaining : 512;
            ssize_t got = read(fd, buffer, wanted);
            if (got != (ssize_t)wanted || !sink->bytes(sink->context, buffer, wanted)) ok = false;
            else remaining -= wanted;
        }
        if (close(fd) != 0) ok = false;
        if (ok) ok = sink->end(sink->context);
        if (!ok) break;
        errno = 0;
    }
    if (errno != 0) ok = false;
    if (closedir(dir) != 0) ok = false;
    return ok;
}

bool katmed_source_export(const KatmedSourceExportSink *sink)
{
    if (!sink || !sink->finish) return false;
    if (!sink->begin || !sink->bytes || !sink->end) {
        sink->finish(sink->context, false);
        return false;
    }
    size_t content = 0;
    uint8_t mask = 0;
    bool acme_present = false;
    bool ok = scan_nvs(&mask);
    size_t nvs_count = 0;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (mask & (1u << i)) ++nvs_count;
    if (ok) ok = scan_files(&content, &acme_present, nvs_count);
    if (ok) {
        uint8_t buffer[MAX_NVS_BLOB];
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]) && ok; ++i) {
            if (mask & (1u << i)) ok = send_nvs(sink, &keys[i], buffer);
            wipe(buffer, sizeof(buffer));
        }
        if (ok) ok = send_directory(sink, "user-images", true, buffer);
        if (ok) ok = send_directory(sink, "acme", acme_present, buffer);
        wipe(buffer, sizeof(buffer));
    }
    sink->finish(sink->context, ok);
    return ok;
}
