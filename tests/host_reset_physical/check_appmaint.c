#define _DEFAULT_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_littlefs.h"
#include "katmed_reset_idf.h"
#include "nvs.h"

static uint8_t legacy[2], own[8], staged[8];
static size_t legacy_len, own_len;
static bool legacy_exists, own_exists, staged_exists, prepared;
static bool files[2], directory_exists, foreign_entry, mount_fails;
static bool keys[4];
static unsigned deletions, namespace_clears, mount_calls;
static int fail_delete_at, fail_key_at;
static bool fail_after_delete, fail_own_write, fail_commit, fail_own_read;
static bool fail_after_commit_read;
static unsigned commit_count, fail_readback_commit_at;
static int read_error_kind;

static void fixture(void)
{
    memset(legacy, 0, sizeof(legacy));
    memset(own, 0, sizeof(own));
    legacy_len = 2; own_len = 8;
    legacy_exists = own_exists = staged_exists = prepared = false;
    files[0] = files[1] = directory_exists = true;
    foreign_entry = mount_fails = false;
    for (unsigned i = 0; i < 4; ++i) keys[i] = true;
    deletions = namespace_clears = mount_calls = 0;
    fail_delete_at = fail_key_at = 0;
    fail_after_delete = fail_own_write = fail_commit = fail_own_read = false;
    fail_after_commit_read = false;
    commit_count = fail_readback_commit_at = 0;
    read_error_kind = 0;
}

static void record(uint8_t state, uint8_t stage)
{
    const uint8_t value[8] = {'K','A','M','1',1,state,stage,0};
    memcpy(own, value, sizeof(value));
    own_len = sizeof(value); own_exists = true;
}

esp_err_t katmed_nvs_init(void) { return ESP_OK; }
bool katmed_source_writer_enter(void) { return true; }
void katmed_source_writer_leave(void) {}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out)
{
    (void)mode;
    *out = strcmp(name, "katmed_reset") == 0 ? 1 : 2;
    staged_exists = own_exists;
    memcpy(staged, own, sizeof(staged));
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{
    assert(h == 1 && strcmp(key, "prepared") == 0 && value == 1);
    prepared = true; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *value)
{
    assert(h == 1 && strcmp(key, "prepared") == 0);
    if (!prepared) return ESP_ERR_NVS_NOT_FOUND;
    *value = 1; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size)
{
    assert(h == 1 && strcmp(key, "appmaint_v1") == 0 && size == 8);
    if (fail_own_write) return ESP_FAIL;
    memcpy(staged, data, size); staged_exists = true; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *size)
{
    assert(h == 1);
    if (strcmp(key, "journal") == 0) {
        if (read_error_kind == 1) return ESP_FAIL;
        if (!legacy_exists) return ESP_ERR_NVS_NOT_FOUND;
        if (*size < legacy_len) return ESP_FAIL;
        memcpy(data, legacy, legacy_len); *size = legacy_len; return ESP_OK;
    }
    assert(strcmp(key, "appmaint_v1") == 0);
    if (read_error_kind == 2 || fail_own_read) { fail_own_read = false; return ESP_FAIL; }
    if (!own_exists) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < own_len) return ESP_FAIL;
    memcpy(data, own, own_len); *size = own_len; return ESP_OK;
}
static int key_index(const char *key)
{
    static const char *const names[] = {"state","mesh_history","names_ctl","names_view"};
    for (int i = 0; i < 4; ++i) if (strcmp(key, names[i]) == 0) return i;
    return -1;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    if (h == 1) {
        assert(strcmp(key, "prepared") == 0);
        if (!prepared) return ESP_ERR_NVS_NOT_FOUND;
        prepared = false; return ESP_OK;
    }
    int index = key_index(key);
    assert(h == 2 && index >= 0);
    ++deletions;
    if (fail_key_at == (int)deletions && !fail_after_delete) return ESP_FAIL;
    if (!keys[index]) return ESP_ERR_NVS_NOT_FOUND;
    keys[index] = false;
    return fail_key_at == (int)deletions ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t h)
{
    (void)h; ++namespace_clears; return ESP_FAIL;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    if (fail_commit) return ESP_FAIL;
    if (h == 1 && staged_exists) {
        ++commit_count;
        memcpy(own, staged, sizeof(own)); own_len = 8; own_exists = true;
        if (fail_after_commit_read || commit_count == fail_readback_commit_at) {
            fail_own_read = true; fail_after_commit_read = false;
        }
    }
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *config)
{
    ++mount_calls;
    assert(strcmp(config->base_path, "/katmed") == 0);
    assert(!config->format_if_mount_failed && !config->grow_on_mount &&
           !config->read_only && !config->dont_mount);
    return mount_fails ? ESP_FAIL : ESP_OK;
}
/* Normal-only dependencies are deliberately unreachable in this variant. */
int katmed_image_store_clear_user_data(void) { assert(false); return 0; }
bool katmed_reset_acme_files(void) { assert(false); return false; }
bool names_idf_clear_files(void) { assert(false); return false; }

DIR *__wrap_opendir(const char *path)
{
    assert(strcmp(path, "/katmed/mesh-names") == 0);
    if (!directory_exists) { errno = ENOENT; return NULL; }
    return (DIR *)1;
}
struct dirent *__wrap_readdir(DIR *directory)
{
    static struct dirent entry;
    static unsigned cursor;
    assert(directory == (DIR *)1);
    while (cursor < 3) {
        unsigned next = cursor++;
        const char *name = next == 0 && files[0] ? "ledger-a.bin" :
            next == 1 && files[1] ? "ledger-b.bin" :
            next == 2 && foreign_entry ? "foreign.bin" : NULL;
        if (!name) continue;
        memset(&entry, 0, sizeof(entry));
        strcpy(entry.d_name, name);
        return &entry;
    }
    cursor = 0; errno = 0; return NULL;
}
int __wrap_closedir(DIR *directory) { assert(directory == (DIR *)1); return 0; }
int __wrap_stat(const char *path, struct stat *out)
{
    assert(strstr(path, "/katmed/mesh-names/") == path);
    memset(out, 0, sizeof(*out)); out->st_mode = S_IFREG;
    return 0;
}
int __wrap_unlink(const char *path)
{
    int index = strstr(path, "ledger-a.bin") ? 0 : strstr(path, "ledger-b.bin") ? 1 : -1;
    assert(index >= 0 && strstr(path, "/katmed/mesh-names/") == path);
    ++deletions;
    if (fail_delete_at == (int)deletions && !fail_after_delete) { errno = EIO; return -1; }
    if (!files[index]) { errno = ENOENT; return -1; }
    files[index] = false;
    if (fail_delete_at == (int)deletions) { errno = EIO; return -1; }
    return 0;
}
int __wrap_rmdir(const char *path)
{
    assert(strcmp(path, "/katmed/mesh-names") == 0);
    if (!directory_exists) { errno = ENOENT; return -1; }
    assert(!files[0] && !files[1] && !foreign_entry);
    directory_exists = false; return 0;
}

static void no_delete(void)
{
    assert(deletions == 0 && namespace_clears == 0);
    assert(files[0] && files[1] && keys[0] && keys[1] && keys[2] && keys[3]);
}
static void completed(void)
{
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_COMPLETED);
    assert(!files[0] && !files[1] && !directory_exists);
    for (unsigned i = 0; i < 4; ++i) assert(!keys[i]);
    assert(namespace_clears == 0);
}
int main(void)
{
    KatmedResetJournalBackend backend = katmed_reset_idf_backend();
    bool pending = true;
    fixture();
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_ABSENT);
    no_delete();
    mount_fails = true;
    assert(!katmed_reset_idf_maintenance_preflight()); no_delete();
    mount_fails = false;
    legacy_exists = true; legacy[0] = 1; legacy[1] = KATMED_RESET_FILES;
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    assert(!katmed_reset_begin(backend)); no_delete();
    legacy[0] = 0; legacy[1] = 4;
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    legacy[1] = 0; legacy_len = 1;
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    legacy_len = 2; read_error_kind = 1;
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    read_error_kind = 0; legacy_exists = false;
    record(1, 1); own[0] = 'X';
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    own_len = 7;
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    record(1, 1); own[4] = 2;
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    record(1, 1); own[5] = 3;
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    record(1, 1); own[6] = 0;
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    record(1, 1); own[6] = 6;
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    record(1, 1); own[7] = 1;
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    record(2, 4);
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    own_len = 8; record(1, 1); read_error_kind = 2;
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    read_error_kind = 0; own_exists = false; no_delete();

    assert(katmed_reset_idf_prepare());
    assert(katmed_reset_idf_take_preparation(&pending) && pending);
    no_delete();
    fail_own_write = true;
    assert(!katmed_reset_begin(backend)); no_delete();
    fail_own_write = false; fail_commit = true;
    assert(!katmed_reset_begin(backend)); no_delete();
    fail_commit = false; fail_after_commit_read = true;
    assert(!katmed_reset_begin(backend)); no_delete();
    /* A committed but unreadable intent is held until an exact read succeeds. */
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_PENDING);

    for (int fault = 1; fault <= 6; ++fault) {
        for (int after = 0; after <= 1; ++after) {
            fixture();
            assert(katmed_reset_idf_maintenance_preflight());
            assert(katmed_reset_begin(backend));
            if (fault <= 2) {
                fail_delete_at = fault; fail_after_delete = after != 0;
            } else { fail_key_at = fault; fail_after_delete = after != 0; }
            assert(!katmed_reset_resume(backend, &pending));
            assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_PENDING);
            fail_delete_at = fail_key_at = 0;
            assert(katmed_reset_resume(backend, &pending) && !pending);
            completed();
        }
    }
    for (unsigned boundary = 2; boundary <= 6; ++boundary) {
        fixture();
        assert(katmed_reset_idf_maintenance_preflight());
        assert(katmed_reset_begin(backend));
        fail_readback_commit_at = boundary;
        assert(!katmed_reset_resume(backend, &pending));
        unsigned before = deletions;
        assert(katmed_reset_resume(backend, &pending) && !pending);
        if (boundary == 6) assert(deletions == before);
        completed();
    }
    fixture(); foreign_entry = true;
    assert(!katmed_reset_idf_maintenance_preflight()); no_delete();
    fixture(); record(1, KATMED_RESET_DONE);
    fail_own_write = true;
    assert(!katmed_reset_resume(backend, &pending));
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_PENDING);
    no_delete();
    fail_own_write = false;
    assert(katmed_reset_resume(backend, &pending) && !pending);
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_COMPLETED);
    no_delete();
    assert(!katmed_reset_begin(backend));
    fixture(); record(2, KATMED_RESET_DONE);
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_COMPLETED);
    assert(!katmed_reset_begin(backend)); no_delete();
    legacy_exists = true; legacy[0] = 1; legacy[1] = KATMED_RESET_FILES;
    assert(katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_HOLD);
    no_delete();
    return 0;
}
