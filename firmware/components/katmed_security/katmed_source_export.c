#include "katmed_source_export.h"

#include "nvs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

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

/* 0: preboot, 1: boot window, 2: exporting, 3: sealed. The two-phase writer check
 * closes the race between an entrant and the exporter's active count. */
static atomic_uint snapshot_phase;
static atomic_uint active_writers;
static atomic_ullong snapshot_deadline_us;
static atomic_flag source_reader = ATOMIC_FLAG_INIT;
static char sorted_names[MAX_FILES][17];

static uint64_t monotonic_us(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return UINT64_MAX;
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
#endif
}

static bool checkpoint(const KatmedSourceExportSink *sink)
{
    if (sink->keep_going && !sink->keep_going(sink->context)) return false;
    if (atomic_load_explicit(&snapshot_phase, memory_order_acquire) == 2u &&
        monotonic_us() >= atomic_load_explicit(&snapshot_deadline_us, memory_order_acquire))
        return false;
    return true;
}

bool katmed_source_writer_enter(void)
{
    if (atomic_load_explicit(&snapshot_phase, memory_order_acquire) == 2u)
        return false;
    atomic_fetch_add_explicit(&active_writers, 1u, memory_order_acq_rel);
    if (atomic_load_explicit(&snapshot_phase, memory_order_acquire) == 2u) {
        atomic_fetch_sub_explicit(&active_writers, 1u, memory_order_acq_rel);
        return false;
    }
    return true;
}

void katmed_source_writer_leave(void)
{
    unsigned count = atomic_load_explicit(&active_writers, memory_order_acquire);
    while (count != 0u && !atomic_compare_exchange_weak_explicit(
        &active_writers, &count, count - 1u,
        memory_order_acq_rel, memory_order_acquire)) { }
}

void katmed_source_snapshot_boot_window_open(void)
{
    unsigned closed = 0u;
    (void)atomic_compare_exchange_strong_explicit(&snapshot_phase, &closed, 1u,
        memory_order_acq_rel, memory_order_acquire);
}

void katmed_source_snapshot_boot_window_close(void)
{
    unsigned armed = 1u;
    (void)atomic_compare_exchange_strong_explicit(&snapshot_phase, &armed, 3u,
        memory_order_acq_rel, memory_order_acquire);
}

bool katmed_source_snapshot_boot(const KatmedSourceExportSink *sink,
                                 bool (*uncontrolled_writers_idle)(void))
{
    if (!sink || !sink->finish) return false;
    unsigned armed = 1u;
    bool acquired = sink->keep_going && uncontrolled_writers_idle &&
        uncontrolled_writers_idle() &&
        atomic_compare_exchange_strong_explicit(&snapshot_phase, &armed, 2u,
            memory_order_acq_rel, memory_order_acquire);
    bool ok = false;
    if (acquired) atomic_store_explicit(&snapshot_deadline_us,
        monotonic_us() + 30000000u, memory_order_release);
    if (acquired && atomic_load_explicit(&active_writers, memory_order_acquire) == 0u &&
        uncontrolled_writers_idle()) {
        ok = katmed_source_export(sink);
    } else {
        sink->finish(sink->context, false);
        ok = false;
    }
    if (acquired) atomic_store_explicit(&snapshot_phase, 1u, memory_order_release);
    return ok;
}

typedef struct { const char *name; const char *key; } Key;
static const Key keys[] = {
    {"katmed", "state"}, {"katmed", "mesh_history"},
    {"katmed_web", "auth"}, {"katmed_web", "identity"},
    {"katmed_reset", "journal"}
};
static const char *const acme_names[] = {
    "account.json", "directory.json", "account.pem", "order.json",
    "certkey.pem", "certificate.pem", "renewal.json"
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
static bool scan_nvs(const KatmedSourceExportSink *sink, uint8_t *mask)
{
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", NULL, NVS_TYPE_ANY, &it);
    *mask = 0;
    while (err == ESP_OK) {
        if (!checkpoint(sink)) { nvs_release_iterator(it); return false; }
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
    return err == ESP_ERR_NVS_NOT_FOUND && (*mask & 1u) &&
        (((*mask >> 2u) & 1u) == ((*mask >> 3u) & 1u));
}

static bool regular_file(const char *path, size_t *size)
{
    struct stat st;
    if (source_stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || st.st_size > MAX_FILE) return false;
    *size = (size_t)st.st_size;
    return true;
}

static bool scan_directory(const KatmedSourceExportSink *sink, const char *name,
                           size_t *count, size_t *content,
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
        if (!checkpoint(sink)) { ok = false; break; }
        const char *file = entry->d_name;
        if (strcmp(file, ".") == 0 || strcmp(file, "..") == 0) { errno = 0; continue; }
        if (!(strcmp(name, "acme") == 0 ? acme_name(file) :
              (image_name(file) || catalog_name(file)))) { ok = false; break; }
        char path[160];
        size_t size;
        n = snprintf(path, sizeof(path), "%s/%s", directory, file);
        if (n <= 0 || (size_t)n >= sizeof(path) || !regular_file(path, &size) ||
            (strcmp(name, "acme") == 0 && size == 0) ||
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

static bool scan_files(const KatmedSourceExportSink *sink, size_t *content,
                       bool *acme_present, size_t *count)
{
    DIR *root = opendir(FS_ROOT);
    if (!root) return false;
    bool ok = true;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(root)) != NULL) {
        if (!checkpoint(sink)) { ok = false; break; }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) { errno = 0; continue; }
        if (strcmp(entry->d_name, "user-images") != 0 &&
            strcmp(entry->d_name, "acme") != 0) { ok = false; break; }
        errno = 0;
    }
    if (errno != 0) ok = false;
    if (closedir(root) != 0) ok = false;
    bool images_present = false;
    return ok && scan_directory(sink, "user-images", count, content, &images_present) &&
        images_present && scan_directory(sink, "acme", count, content, acme_present);
}

static bool send_nvs(const KatmedSourceExportSink *sink, const Key *key,
                     uint8_t buffer[MAX_NVS_BLOB])
{
    if (!checkpoint(sink)) return false;
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
    return n > 0 && (size_t)n < sizeof(logical) && checkpoint(sink) &&
        sink->begin(sink->context, logical, length) &&
        sink->bytes(sink->context, buffer, length) && sink->end(sink->context);
}

static bool send_directory(const KatmedSourceExportSink *sink, const char *name,
                           bool expected, size_t *count, size_t *content,
                           uint8_t buffer[512])
{
    char directory[128];
    int n = snprintf(directory, sizeof(directory), FS_ROOT "/%s", name);
    if (n <= 0 || (size_t)n >= sizeof(directory)) return false;
    DIR *dir = opendir(directory);
    if (!dir) return !expected && errno == ENOENT;
    if (!expected) { closedir(dir); return false; }
    memset(sorted_names, 0, sizeof(sorted_names));
    size_t name_count = 0;
    bool ok = true;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (!checkpoint(sink)) { ok = false; break; }
        const char *file = entry->d_name;
        if (strcmp(file, ".") == 0 || strcmp(file, "..") == 0) { errno = 0; continue; }
        if (!(strcmp(name, "acme") == 0 ? acme_name(file) :
              (image_name(file) || catalog_name(file))) ||
            name_count == MAX_FILES) { ok = false; break; }
        memcpy(sorted_names[name_count++], file, strlen(file) + 1u);
        errno = 0;
    }
    if (errno != 0) ok = false;
    if (closedir(dir) != 0) ok = false;
    if (!ok) return false;
    for (size_t i = 1; i < name_count; ++i) {
        char current[17];
        memcpy(current, sorted_names[i], sizeof(current));
        size_t j = i;
        while (j > 0 && strcmp(sorted_names[j - 1], current) > 0) {
            memcpy(sorted_names[j], sorted_names[j - 1], sizeof(sorted_names[j]));
            --j;
        }
        memcpy(sorted_names[j], current, sizeof(current));
    }
    for (size_t i = 0; i < name_count; ++i) {
        if (!checkpoint(sink)) return false;
        const char *file = sorted_names[i];
        char path[160], logical[80];
        size_t size;
        n = snprintf(path, sizeof(path), "%s/%s", directory, file);
        int m = snprintf(logical, sizeof(logical), "littlefs/%s/%s",
                         strcmp(name, "acme") == 0 ? "acme" : "user-images", file);
        if (n <= 0 || (size_t)n >= sizeof(path) || m <= 0 ||
            (size_t)m >= sizeof(logical) || !regular_file(path, &size) ||
            (strcmp(name, "acme") == 0 && size == 0) ||
            *count >= MAX_FILES || size > MAX_CONTENT - *content) {
            ok = false; break;
        }
        ++*count;
        *content += size;
        int fd = open(path, O_RDONLY | O_NOFOLLOW);
        if (fd < 0) { ok = false; break; }
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size != (off_t)size || !sink->begin(sink->context, logical, size)) ok = false;
        size_t remaining = size;
        while (ok && remaining) {
            if (!checkpoint(sink)) { ok = false; break; }
            size_t wanted = remaining < 512 ? remaining : 512;
            ssize_t got = read(fd, buffer, wanted);
            if (got != (ssize_t)wanted || !sink->bytes(sink->context, buffer, wanted)) ok = false;
            else remaining -= wanted;
        }
        if (close(fd) != 0) ok = false;
        if (ok) ok = checkpoint(sink) && sink->end(sink->context);
        if (!ok) return false;
    }
    return true;
}

bool katmed_source_export(const KatmedSourceExportSink *sink)
{
    if (!sink || !sink->finish) return false;
#ifdef ESP_PLATFORM
    if (atomic_load_explicit(&snapshot_phase, memory_order_acquire) != 2u) {
        sink->finish(sink->context, false);
        return false;
    }
#endif
    if (atomic_flag_test_and_set_explicit(&source_reader, memory_order_acquire)) {
        sink->finish(sink->context, false);
        return false;
    }
    if (!sink->begin || !sink->bytes || !sink->end) {
        sink->finish(sink->context, false);
        atomic_flag_clear_explicit(&source_reader, memory_order_release);
        return false;
    }
    size_t content = 0;
    uint8_t mask = 0;
    bool acme_present = false;
    bool ok = checkpoint(sink) && scan_nvs(sink, &mask);
    size_t nvs_count = 0;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (mask & (1u << i)) ++nvs_count;
    size_t scanned_count = nvs_count;
    if (ok) ok = scan_files(sink, &content, &acme_present, &scanned_count);
    if (ok) {
        uint8_t buffer[MAX_NVS_BLOB];
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]) && ok; ++i) {
            if (mask & (1u << i)) ok = send_nvs(sink, &keys[i], buffer);
            wipe(buffer, sizeof(buffer));
        }
        size_t sent_count = nvs_count;
        size_t sent_content = 0;
        if (ok) ok = send_directory(sink, "user-images", true,
                                    &sent_count, &sent_content, buffer);
        if (ok) ok = send_directory(sink, "acme", acme_present,
                                    &sent_count, &sent_content, buffer);
        if (ok) ok = sent_count == scanned_count && sent_content == content &&
                      checkpoint(sink);
        wipe(buffer, sizeof(buffer));
    }
    sink->finish(sink->context, ok);
    wipe((uint8_t *)sorted_names, sizeof(sorted_names));
    atomic_flag_clear_explicit(&source_reader, memory_order_release);
    return ok;
}
