#include "katmed_reset_idf.h"
#include "katmed_reset_acme_files.h"
#include "katmed_names_cleanup.h"
#include "katmed_nvs_init.h"
#include "katmed_source_export.h"

#include "esp_err.h"
#include "katmed_image_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#if KATMED_APPSTORE_MAINTENANCE
#include "katmed_security_layout.h"
#include "esp_littlefs.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static bool nvs_ready(void)
{
    return katmed_nvs_init()==ESP_OK;
}

bool katmed_reset_idf_prepare(void)
{
    if (!nvs_ready()) return false;
    if (!katmed_source_writer_enter()) return false;
    nvs_handle_t handle;
    if (nvs_open("katmed_reset", NVS_READWRITE, &handle) != ESP_OK) {
        katmed_source_writer_leave(); return false;
    }
    esp_err_t result = nvs_set_u8(handle, "prepared", 1);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    katmed_source_writer_leave();
    return result == ESP_OK;
}

bool katmed_reset_idf_cancel_preparation(void)
{
    if (!nvs_ready()) return false;
    if (!katmed_source_writer_enter()) return false;
    nvs_handle_t handle;
    if (nvs_open("katmed_reset", NVS_READWRITE, &handle) != ESP_OK) {
        katmed_source_writer_leave(); return false;
    }
    esp_err_t result = nvs_erase_key(handle, "prepared");
    if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    katmed_source_writer_leave();
    return result == ESP_OK;
}

bool katmed_reset_idf_take_preparation(bool *prepared)
{
    if (!prepared || !nvs_ready()) return false;
    *prepared = false;
    if (!katmed_source_writer_enter()) return false;
    nvs_handle_t handle;
    if (nvs_open("katmed_reset", NVS_READWRITE, &handle) != ESP_OK) {
        katmed_source_writer_leave(); return false;
    }
    uint8_t value = 0;
    esp_err_t result = nvs_get_u8(handle, "prepared", &value);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        katmed_source_writer_leave();
        return true;
    }
    if (result != ESP_OK) { nvs_close(handle); katmed_source_writer_leave(); return false; }
    result = nvs_erase_key(handle, "prepared");
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    katmed_source_writer_leave();
    if (result != ESP_OK) return false;
    *prepared = value == 1;
    return true;
}

#if KATMED_APPSTORE_MAINTENANCE
static const uint8_t appmaint_tag[4] = {'K','A','M','1'};
static bool appmaint_fs_mounted;

/* The full-reset journal is a separate read-only veto, never our authority. */
static bool legacy_safe(nvs_handle_t handle)
{
    uint8_t bytes[2] = {0};
    size_t length = sizeof(bytes);
    esp_err_t result = nvs_get_blob(handle, "journal", bytes, &length);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    return result == ESP_OK && length == sizeof(bytes) &&
           bytes[0] == 0 && bytes[1] == 0;
}

static KatmedAppmaintStatus read_control(uint8_t *stage)
{
    if (stage) *stage = 0;
    if (!nvs_ready()) return KATMED_APPMAINT_HOLD;
    nvs_handle_t handle;
    esp_err_t result = nvs_open("katmed_reset", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return KATMED_APPMAINT_ABSENT;
    if (result != ESP_OK) return KATMED_APPMAINT_HOLD;
    if (!legacy_safe(handle)) { nvs_close(handle); return KATMED_APPMAINT_HOLD; }
    uint8_t bytes[8] = {0};
    size_t length = sizeof(bytes);
    result = nvs_get_blob(handle, "appmaint_v1", bytes, &length);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return KATMED_APPMAINT_ABSENT;
    if (result != ESP_OK || length != sizeof(bytes) ||
        memcmp(bytes, appmaint_tag, sizeof(appmaint_tag)) != 0 ||
        bytes[4] != 1 || bytes[7] != 0 ||
        bytes[5] < 1 || bytes[5] > 2 ||
        bytes[6] < KATMED_RESET_FILES || bytes[6] > KATMED_RESET_DONE ||
        (bytes[5] == 2 && bytes[6] != KATMED_RESET_DONE))
        return KATMED_APPMAINT_HOLD;
    if (stage) *stage = bytes[6];
    return bytes[5] == 1 ? KATMED_APPMAINT_PENDING : KATMED_APPMAINT_COMPLETED;
}

KatmedAppmaintStatus katmed_reset_idf_maintenance_status(uint8_t *stage)
{
    return read_control(stage);
}

bool katmed_reset_idf_maintenance_preflight(void)
{
    if (!appmaint_fs_mounted) {
        const esp_vfs_littlefs_conf_t config = {
            .base_path = "/katmed",
            .partition_label = KATMED_USER_IMAGE_PARTITION_LABEL,
            .format_if_mount_failed = false,
            .read_only = false,
            .dont_mount = false,
            .grow_on_mount = false,
        };
        if (esp_vfs_littlefs_register(&config) != ESP_OK) return false;
        appmaint_fs_mounted = true;
    }
    DIR *directory = opendir("/katmed/mesh-names");
    if (!directory) return errno == ENOENT;
    bool valid = true;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (strcmp(entry->d_name, "ledger-a.bin") &&
            strcmp(entry->d_name, "ledger-b.bin")) { valid = false; break; }
        char path[64];
        int length = snprintf(path, sizeof(path), "/katmed/mesh-names/%s", entry->d_name);
        struct stat info;
        if (length < 0 || (size_t)length >= sizeof(path) ||
            stat(path, &info) != 0 || !S_ISREG(info.st_mode)) { valid = false; break; }
        errno = 0;
    }
    if (errno != 0) valid = false;
    if (closedir(directory) != 0) valid = false;
    return valid;
}

static bool journal_load(void *context,KatmedResetJournal *out)
{
    (void)context;
    if (!out) return false;
    uint8_t stage = 0;
    KatmedAppmaintStatus status = read_control(&stage);
    if (status == KATMED_APPMAINT_HOLD) return false;
    *out = (KatmedResetJournal){status == KATMED_APPMAINT_PENDING, stage};
    return true;
}

static bool journal_save(void *context,const KatmedResetJournal *journal)
{
    (void)context;
    if (!journal || !nvs_ready()) return false;
    uint8_t old_stage = 0;
    KatmedAppmaintStatus old = read_control(&old_stage);
    if (old == KATMED_APPMAINT_HOLD || old == KATMED_APPMAINT_COMPLETED) return false;
    const bool completing = !journal->pending && journal->stage == 0;
    uint8_t next_stage = completing ? KATMED_RESET_DONE : journal->stage;
    if (completing) {
        if (old != KATMED_APPMAINT_PENDING || old_stage != KATMED_RESET_DONE) return false;
    } else if (!journal->pending || next_stage < KATMED_RESET_FILES ||
               next_stage > KATMED_RESET_DONE ||
               (old == KATMED_APPMAINT_ABSENT && next_stage != KATMED_RESET_FILES) ||
               (old == KATMED_APPMAINT_PENDING &&
                next_stage != old_stage && next_stage != old_stage + 1)) return false;
    if (!katmed_source_writer_enter()) return false;
    nvs_handle_t handle;
    if (nvs_open("katmed_reset", NVS_READWRITE, &handle) != ESP_OK) {
        katmed_source_writer_leave(); return false;
    }
    const uint8_t bytes[8] = {'K','A','M','1',1,completing ? 2 : 1,next_stage,0};
    esp_err_t result = nvs_set_blob(handle, "appmaint_v1", bytes, sizeof(bytes));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    katmed_source_writer_leave();
    if (result != ESP_OK) return false;
    uint8_t observed_stage = 0;
    KatmedAppmaintStatus observed = read_control(&observed_stage);
    return observed_stage == next_stage && observed ==
        (completing ? KATMED_APPMAINT_COMPLETED : KATMED_APPMAINT_PENDING);
}

static bool clear_key(const char *name)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open("katmed", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    nvs_close(handle);
    if (nvs_open("katmed", NVS_READWRITE, &handle) != ESP_OK) return false;
    result = nvs_erase_key(handle, name);
    if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

static bool journal_run_stage(void *context,KatmedResetStage stage)
{
    (void)context;
    uint8_t recorded_stage = 0;
    if (read_control(&recorded_stage) != KATMED_APPMAINT_PENDING ||
        recorded_stage != stage || !katmed_source_writer_enter()) return false;
    bool ok = false;
    switch (stage) {
    case KATMED_RESET_FILES:
        ok = katmed_reset_idf_maintenance_preflight();
        if (ok && unlink("/katmed/mesh-names/ledger-a.bin") != 0 && errno != ENOENT) ok = false;
        if (ok && unlink("/katmed/mesh-names/ledger-b.bin") != 0 && errno != ENOENT) ok = false;
        if (ok && rmdir("/katmed/mesh-names") != 0 && errno != ENOENT) ok = false;
        break;
    case KATMED_RESET_KATMED_NVS:
        ok = clear_key("state") && clear_key("mesh_history") &&
             clear_key("names_ctl") && clear_key("names_view");
        break;
    case KATMED_RESET_WEB_NVS:
    case KATMED_RESET_ACME_NVS:
        ok = true;
        break;
    default:
        break;
    }
    katmed_source_writer_leave();
    return ok;
}
#else
static bool journal_load(void *context,KatmedResetJournal *out)
{
    (void)context;
    if (!out || !nvs_ready()) return false;
    nvs_handle_t handle;
    if (nvs_open("katmed_reset",NVS_READWRITE,&handle)!=ESP_OK) return false;
    uint8_t bytes[2]={0}; size_t length=sizeof(bytes);
    esp_err_t result=nvs_get_blob(handle,"journal",bytes,&length);
    nvs_close(handle);
    if (result==ESP_ERR_NVS_NOT_FOUND) { *out=(KatmedResetJournal){0}; return true; }
    if (result!=ESP_OK || length!=sizeof(bytes) || bytes[0]>1) return false;
    *out=(KatmedResetJournal){.pending=bytes[0]!=0,.stage=bytes[1]};
    return true;
}

static bool journal_save(void *context,const KatmedResetJournal *journal)
{
    (void)context;
    if (!journal || !nvs_ready()) return false;
    if (!katmed_source_writer_enter()) return false;
    nvs_handle_t handle;
    if (nvs_open("katmed_reset",NVS_READWRITE,&handle)!=ESP_OK) {
        katmed_source_writer_leave(); return false;
    }
    const uint8_t bytes[2]={(uint8_t)journal->pending,journal->stage};
    esp_err_t result=nvs_set_blob(handle,"journal",bytes,sizeof(bytes));
    if (result==ESP_OK) result=nvs_commit(handle);
    nvs_close(handle);
    katmed_source_writer_leave();
    return result==ESP_OK;
}

static bool clear_namespace(const char *name)
{
    if (!nvs_ready()) return false;
    if (!katmed_source_writer_enter()) return false;
    nvs_handle_t handle;
    if (nvs_open(name,NVS_READWRITE,&handle)!=ESP_OK) {
        katmed_source_writer_leave(); return false;
    }
    esp_err_t result=nvs_erase_all(handle);
    if (result==ESP_OK) result=nvs_commit(handle);
    nvs_close(handle);
    katmed_source_writer_leave();
    return result==ESP_OK;
}

static bool journal_run_stage(void *context,KatmedResetStage stage)
{
    (void)context;
    if (!katmed_source_writer_enter()) return false;
    bool ok = false;
    switch (stage) {
    case KATMED_RESET_FILES:
        ok = katmed_image_store_clear_user_data()==KATMED_IMAGE_OK &&
             katmed_reset_acme_files() && names_idf_clear_files();
        break;
    case KATMED_RESET_KATMED_NVS:
        /* Contains app state, credentials, and the separate mesh_history key. */
        ok = clear_namespace("katmed");
        break;
    case KATMED_RESET_WEB_NVS:
        /* Clears web auth, server identity, uploaded CA and client records. */
        ok = clear_namespace("katmed_web");
        break;
    case KATMED_RESET_ACME_NVS:
        ok = clear_namespace("katmed_acme");
        break;
    case KATMED_RESET_DONE:
        ok = true;
        break;
    }
    katmed_source_writer_leave();
    return ok;
}
#endif

KatmedResetJournalBackend katmed_reset_idf_backend(void)
{
    return (KatmedResetJournalBackend){.context=NULL,.load=journal_load,
        .save=journal_save,.run_stage=journal_run_stage};
}
