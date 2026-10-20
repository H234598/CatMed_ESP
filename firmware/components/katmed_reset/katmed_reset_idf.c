#include "katmed_reset_idf.h"
#include "katmed_reset_acme_files.h"
#include "katmed_nvs_init.h"

#include "esp_err.h"
#include "katmed_image_store.h"
#include "nvs.h"
#include "nvs_flash.h"

static bool nvs_ready(void)
{
    return katmed_nvs_init()==ESP_OK;
}

bool katmed_reset_idf_prepare(void)
{
    if (!nvs_ready()) return false;
    nvs_handle_t handle;
    if (nvs_open("katmed_reset", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_u8(handle, "prepared", 1);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

bool katmed_reset_idf_cancel_preparation(void)
{
    if (!nvs_ready()) return false;
    nvs_handle_t handle;
    if (nvs_open("katmed_reset", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_erase_key(handle, "prepared");
    if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}

bool katmed_reset_idf_take_preparation(bool *prepared)
{
    if (!prepared || !nvs_ready()) return false;
    *prepared = false;
    nvs_handle_t handle;
    if (nvs_open("katmed_reset", NVS_READWRITE, &handle) != ESP_OK) return false;
    uint8_t value = 0;
    esp_err_t result = nvs_get_u8(handle, "prepared", &value);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return true;
    }
    if (result != ESP_OK) { nvs_close(handle); return false; }
    result = nvs_erase_key(handle, "prepared");
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) return false;
    *prepared = value == 1;
    return true;
}

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
    nvs_handle_t handle;
    if (nvs_open("katmed_reset",NVS_READWRITE,&handle)!=ESP_OK) return false;
    const uint8_t bytes[2]={(uint8_t)journal->pending,journal->stage};
    esp_err_t result=nvs_set_blob(handle,"journal",bytes,sizeof(bytes));
    if (result==ESP_OK) result=nvs_commit(handle);
    nvs_close(handle);
    return result==ESP_OK;
}

static bool clear_namespace(const char *name)
{
    if (!nvs_ready()) return false;
    nvs_handle_t handle;
    if (nvs_open(name,NVS_READWRITE,&handle)!=ESP_OK) return false;
    esp_err_t result=nvs_erase_all(handle);
    if (result==ESP_OK) result=nvs_commit(handle);
    nvs_close(handle);
    return result==ESP_OK;
}

static bool journal_run_stage(void *context,KatmedResetStage stage)
{
    (void)context;
    switch (stage) {
    case KATMED_RESET_FILES:
        return katmed_image_store_clear_user_data()==KATMED_IMAGE_OK &&
               katmed_reset_acme_files();
    case KATMED_RESET_KATMED_NVS:
        /* Contains app state, credentials, and the separate mesh_history key. */
        return clear_namespace("katmed");
    case KATMED_RESET_WEB_NVS:
        /* Clears web auth, server identity, uploaded CA and client records. */
        return clear_namespace("katmed_web");
    case KATMED_RESET_ACME_NVS:
        return clear_namespace("katmed_acme");
    case KATMED_RESET_DONE:
        return true;
    }
    return false;
}

KatmedResetJournalBackend katmed_reset_idf_backend(void)
{
    return (KatmedResetJournalBackend){.context=NULL,.load=journal_load,
        .save=journal_save,.run_stage=journal_run_stage};
}
