#include "katmed_source_export.h"

#include "nvs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mbedtls/md.h"
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

/* Low three bits are phase: 0 preboot, 1 open, 2 exporting, 3 sealed,
 * 4 exporting with a pending seal. Upper bits are active leases. One CAS
 * orders admission, export entry and sealing, including close during export. */
#define PHASE_MASK 7u
#define LEASE_UNIT 8u
#define MAX_LEASES (UINT_MAX >> 3u)
static atomic_uint snapshot_state;
static _Thread_local unsigned owned_leases;
static atomic_ullong snapshot_deadline_us;
static atomic_flag source_reader = ATOMIC_FLAG_INIT;
static char sorted_names[MAX_FILES][17];
typedef struct {
    char name[17];
    uint8_t digest[32];
    size_t size;
    bool acme;
    bool seen;
} FileIdentity;
static FileIdentity file_ids[MAX_FILES];
static size_t file_id_count;
static uint8_t nvs_digests[5][32];
static size_t nvs_lengths[5];
#ifdef KATMED_SOURCE_EXPORT_HOST_TEST
void (*katmed_source_test_before_admission)(void);
#endif

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
    /* NULL is the callback-free final verification phase. The internal
     * deadline still applies, but no sink can mutate the source here. */
    if (sink && sink->keep_going && !sink->keep_going(sink->context)) return false;
    unsigned phase = atomic_load_explicit(&snapshot_state, memory_order_acquire) & PHASE_MASK;
    if ((phase == 2u || phase == 4u) &&
        monotonic_us() >= atomic_load_explicit(&snapshot_deadline_us, memory_order_acquire))
        return false;
    return true;
}

bool katmed_source_writer_enter(void)
{
    unsigned state = atomic_load_explicit(&snapshot_state, memory_order_acquire);
#ifdef KATMED_SOURCE_EXPORT_HOST_TEST
    if (katmed_source_test_before_admission) katmed_source_test_before_admission();
#endif
    do {
        if ((state & PHASE_MASK) == 2u || (state & PHASE_MASK) == 4u ||
            state / LEASE_UNIT == MAX_LEASES ||
            owned_leases == UINT_MAX) return false;
    } while (!atomic_compare_exchange_weak_explicit(&snapshot_state, &state,
                 state + LEASE_UNIT, memory_order_acq_rel, memory_order_acquire));
    ++owned_leases;
    return true;
}

void katmed_source_writer_leave(void)
{
    if (owned_leases == 0u) return;
    --owned_leases;
    unsigned previous = atomic_fetch_sub_explicit(&snapshot_state, LEASE_UNIT,
                                                    memory_order_acq_rel);
    /* Only this task's successful enters can reach here. Never hide a corrupt
     * zero count by wrapping it into a seemingly busy state. */
    if (previous < LEASE_UNIT) abort();
}

void katmed_source_snapshot_boot_window_open(void)
{
    unsigned closed = 0u;
    (void)atomic_compare_exchange_strong_explicit(&snapshot_state, &closed, 1u,
        memory_order_acq_rel, memory_order_acquire);
}

void katmed_source_snapshot_boot_window_close(void)
{
    unsigned state = atomic_load_explicit(&snapshot_state, memory_order_acquire);
    while ((state & PHASE_MASK) == 0u || (state & PHASE_MASK) == 1u ||
           (state & PHASE_MASK) == 2u) {
        unsigned sealed = (state & ~PHASE_MASK) |
                          ((state & PHASE_MASK) == 2u ? 4u : 3u);
        if (atomic_compare_exchange_weak_explicit(&snapshot_state, &state, sealed,
                memory_order_acq_rel, memory_order_acquire)) break;
    }
}

bool katmed_source_snapshot_boot(const KatmedSourceExportSink *sink,
                                 bool (*uncontrolled_writers_idle)(void))
{
    if (!sink || !sink->finish) return false;
    unsigned armed = 1u;
#ifdef KATMED_SOURCE_EXPORT_HOST_TEST
    if (katmed_source_test_before_admission) katmed_source_test_before_admission();
#endif
    bool acquired = sink->keep_going && uncontrolled_writers_idle &&
        uncontrolled_writers_idle() &&
        atomic_compare_exchange_strong_explicit(&snapshot_state, &armed, 2u,
            memory_order_acq_rel, memory_order_acquire);
    bool ok = false;
    if (acquired) atomic_store_explicit(&snapshot_deadline_us,
        monotonic_us() + 30000000u, memory_order_release);
    if (acquired && uncontrolled_writers_idle()) {
        ok = katmed_source_export(sink);
    } else {
        sink->finish(sink->context, false);
        ok = false;
    }
    if (acquired) {
        unsigned exporting = atomic_load_explicit(&snapshot_state, memory_order_acquire);
        unsigned destination = exporting == 4u ? 3u : 1u;
        if ((exporting != 2u && exporting != 4u) ||
            !atomic_compare_exchange_strong_explicit(&snapshot_state, &exporting,
                destination, memory_order_acq_rel, memory_order_acquire)) abort();
    }
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

static bool hash_start(mbedtls_md_context_t *hash)
{
    mbedtls_md_init(hash);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || mbedtls_md_setup(hash, info, 0) != 0 ||
        mbedtls_md_starts(hash) != 0) {
        mbedtls_md_free(hash);
        return false;
    }
    return true;
}

static bool hash_bytes(const uint8_t *data, size_t length, uint8_t digest[32])
{
    mbedtls_md_context_t hash;
    if (!hash_start(&hash)) return false;
    bool ok = mbedtls_md_update(&hash, data, length) == 0 &&
              mbedtls_md_finish(&hash, digest) == 0;
    mbedtls_md_free(&hash);
    return ok;
}

static bool hash_file(const KatmedSourceExportSink *sink, const char *path,
                      size_t expected, uint8_t digest[32])
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    mbedtls_md_context_t hash;
    bool started = hash_start(&hash);
    bool ok = started && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
              st.st_size == (off_t)expected;
    uint8_t buffer[512];
    size_t remaining = expected;
    while (ok && remaining) {
        if (!checkpoint(sink)) { ok = false; break; }
        size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        ssize_t got = read(fd, buffer, wanted);
        if (got != (ssize_t)wanted ||
            mbedtls_md_update(&hash, buffer, wanted) != 0) ok = false;
        else remaining -= wanted;
    }
    if (ok) ok = mbedtls_md_finish(&hash, digest) == 0;
    if (started) mbedtls_md_free(&hash);
    wipe(buffer, sizeof(buffer));
    if (close(fd) != 0) ok = false;
    return ok;
}

static FileIdentity *find_file(const char *name, bool acme)
{
    for (size_t i = 0; i < file_id_count; ++i)
        if (file_ids[i].acme == acme && strcmp(file_ids[i].name, name) == 0)
            return &file_ids[i];
    return NULL;
}

static bool image_name(const char *s)
{
    if (strlen(s) != 16 || strncmp(s, "img-", 4) != 0 ||
        strcmp(s + 12, ".jpg") != 0) return false;
    for (unsigned i = 4; i < 12; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'a' && s[i] <= 'f'))) return false;
    /* The import parser accepts only IDs 1..0x7fffffff. */
    if (s[4] > '7') return false;
    for (unsigned i = 4; i < 12; ++i)
        if (s[i] != '0') return true;
    return false;
}

static bool catalog_name(const char *s)
{
    if (strlen(s) != 12 || strncmp(s, "cat-", 4) != 0 ||
        s[6] != '-' || (s[7] != '0' && s[7] != '1') ||
        strcmp(s + 8, ".jpg") != 0) return false;
    for (unsigned i = 4; i < 6; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'a' && s[i] <= 'f'))) return false;
    /* There are 72 catalog IDs, 0x00..0x47. */
    return s[4] < '4' || (s[4] == '4' && s[5] <= '7');
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
        (!(*mask & (1u << 2u)) || (*mask & (1u << 3u)));
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
                           bool *present, bool verify)
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
        uint8_t digest[32];
        if (!hash_file(sink, path, size, digest)) { ok = false; break; }
        bool is_acme = strcmp(name, "acme") == 0;
        FileIdentity *id = verify ? find_file(file, is_acme) :
                           (file_id_count < MAX_FILES ? &file_ids[file_id_count] : NULL);
        if (!id || (verify && (id->seen || id->size != size ||
                              memcmp(id->digest, digest, sizeof(digest)) != 0))) {
            ok = false; break;
        }
        if (verify) id->seen = true;
        else {
            memset(id, 0, sizeof(*id));
            memcpy(id->name, file, strlen(file) + 1u);
            memcpy(id->digest, digest, sizeof(digest));
            id->size = size;
            id->acme = is_acme;
            ++file_id_count;
        }
        errno = 0;
    }
    if (errno != 0) ok = false;
    if (closedir(dir) != 0) ok = false;
    return ok;
}

static bool scan_files(const KatmedSourceExportSink *sink, size_t *content,
                       bool *acme_present, size_t *count, bool verify)
{
    if (verify)
        for (size_t i = 0; i < file_id_count; ++i) file_ids[i].seen = false;
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
    ok = ok && scan_directory(sink, "user-images", count, content,
                              &images_present, verify) && images_present &&
         scan_directory(sink, "acme", count, content, acme_present, verify);
    if (ok && verify)
        for (size_t i = 0; i < file_id_count; ++i)
            if (!file_ids[i].seen) return false;
    return ok;
}

static bool read_nvs(const KatmedSourceExportSink *sink, const Key *key,
                     uint8_t buffer[MAX_NVS_BLOB], size_t *length)
{
    if (!checkpoint(sink)) return false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(key->name, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;
    *length = 0;
    err = nvs_get_blob(handle, key->key, NULL, length);
    if (err != ESP_OK || *length == 0 || *length > MAX_NVS_BLOB) {
        nvs_close(handle);
        return false;
    }
    size_t actual = *length;
    err = nvs_get_blob(handle, key->key, buffer, &actual);
    nvs_close(handle);
    if (err != ESP_OK || actual != *length) return false;
    if (strcmp(key->name, "katmed_reset") == 0 &&
        (*length != 2 || buffer[0] != 0 || buffer[1] > 5)) return false;
    return true;
}

static bool bind_nvs(const KatmedSourceExportSink *sink, uint8_t mask,
                     uint8_t buffer[MAX_NVS_BLOB], bool verify)
{
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (!(mask & (1u << i))) continue;
        size_t length;
        uint8_t digest[32];
        bool ok = read_nvs(sink, &keys[i], buffer, &length) &&
                  hash_bytes(buffer, length, digest);
        wipe(buffer, sizeof(uint8_t) * MAX_NVS_BLOB);
        if (!ok || (verify && (nvs_lengths[i] != length ||
                              memcmp(nvs_digests[i], digest, sizeof(digest)) != 0)))
            return false;
        if (!verify) {
            nvs_lengths[i] = length;
            memcpy(nvs_digests[i], digest, sizeof(digest));
        }
    }
    return true;
}

static bool send_nvs(const KatmedSourceExportSink *sink, const Key *key,
                     unsigned index, uint8_t buffer[MAX_NVS_BLOB])
{
    size_t length;
    uint8_t digest[32];
    if (!read_nvs(sink, key, buffer, &length) ||
        !hash_bytes(buffer, length, digest) || length != nvs_lengths[index] ||
        memcmp(digest, nvs_digests[index], sizeof(digest)) != 0) return false;
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
        FileIdentity *id = find_file(file, strcmp(name, "acme") == 0);
        if (!id || id->size != size) { ok = false; break; }
        ++*count;
        *content += size;
        int fd = open(path, O_RDONLY | O_NOFOLLOW);
        if (fd < 0) { ok = false; break; }
        struct stat st;
        mbedtls_md_context_t hash;
        bool started = hash_start(&hash);
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size != (off_t)size || !started ||
            !sink->begin(sink->context, logical, size)) ok = false;
        size_t remaining = size;
        while (ok && remaining) {
            if (!checkpoint(sink)) { ok = false; break; }
            size_t wanted = remaining < 512 ? remaining : 512;
            ssize_t got = read(fd, buffer, wanted);
            if (got != (ssize_t)wanted ||
                mbedtls_md_update(&hash, buffer, wanted) != 0 ||
                !sink->bytes(sink->context, buffer, wanted)) ok = false;
            else remaining -= wanted;
        }
        uint8_t digest[32];
        if (ok && (mbedtls_md_finish(&hash, digest) != 0 ||
                   memcmp(digest, id->digest, sizeof(digest)) != 0)) ok = false;
        if (started) mbedtls_md_free(&hash);
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
    unsigned phase = atomic_load_explicit(&snapshot_state, memory_order_acquire) & PHASE_MASK;
    if (phase != 2u && phase != 4u) {
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
    file_id_count = 0;
    bool ok = checkpoint(sink) && scan_nvs(sink, &mask);
    size_t nvs_count = 0;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (mask & (1u << i)) ++nvs_count;
    size_t scanned_count = nvs_count;
    if (ok) ok = scan_files(sink, &content, &acme_present, &scanned_count, false);
    if (ok) {
        uint8_t buffer[MAX_NVS_BLOB];
        ok = bind_nvs(sink, mask, buffer, false);
        uint8_t checked_mask = 0;
        size_t checked_count = nvs_count, checked_content = 0;
        bool checked_acme = false;
        if (ok) ok = scan_nvs(sink, &checked_mask) && checked_mask == mask &&
                     scan_files(sink, &checked_content, &checked_acme,
                                &checked_count, true) &&
                     checked_acme == acme_present && checked_count == scanned_count &&
                     checked_content == content && bind_nvs(sink, mask, buffer, true);
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]) && ok; ++i) {
            if (mask & (1u << i)) ok = send_nvs(sink, &keys[i], i, buffer);
            wipe(buffer, sizeof(buffer));
        }
        size_t sent_count = nvs_count;
        size_t sent_content = 0;
        if (ok) ok = send_directory(sink, "user-images", true,
                                    &sent_count, &sent_content, buffer);
        if (ok) ok = send_directory(sink, "acme", acme_present,
                                    &sent_count, &sent_content, buffer);
        checked_mask = 0;
        checked_count = nvs_count;
        checked_content = 0;
        checked_acme = false;
        /* This is the last externally supplied callback before finish().
         * All subsequent source checks pass NULL, including nested reads. */
        if (ok) ok = sent_count == scanned_count && sent_content == content &&
                     checkpoint(sink) &&
                     scan_nvs(NULL, &checked_mask) && checked_mask == mask &&
                     scan_files(NULL, &checked_content, &checked_acme,
                                &checked_count, true) && checked_acme == acme_present &&
                     checked_count == scanned_count && checked_content == content &&
                     bind_nvs(NULL, mask, buffer, true) && checkpoint(NULL);
        wipe(buffer, sizeof(buffer));
    }
    sink->finish(sink->context, ok);
    wipe((uint8_t *)sorted_names, sizeof(sorted_names));
    wipe((uint8_t *)file_ids, sizeof(file_ids));
    wipe((uint8_t *)nvs_digests, sizeof(nvs_digests));
    wipe((uint8_t *)nvs_lengths, sizeof(nvs_lengths));
    file_id_count = 0;
    atomic_flag_clear_explicit(&source_reader, memory_order_release);
    return ok;
}

#ifdef KATMED_SOURCE_EXPORT_HOST_TEST
/* Inject the packed counter boundary only in the synthetic host fixture. */
void katmed_source_test_set_state(unsigned state)
{
    atomic_store_explicit(&snapshot_state, state, memory_order_release);
}
#endif
