#include "katmed_store_idf.h"

#include <string.h>
#include "nvs_flash.h"

static StoreResult idf_read(void *context, uint8_t *buffer, size_t *length)
{
    StoreIdfNvs *nvs = (StoreIdfNvs *)context;
    if (!nvs || !nvs->opened || !buffer || !length) return STORE_INVALID;
    size_t available = 0;
    esp_err_t err = nvs_get_blob(nvs->handle, "state", NULL, &available);
    if (err == ESP_ERR_NVS_NOT_FOUND) return STORE_NOT_FOUND;
    if (err != ESP_OK) return STORE_IO;
    if (available == 0 || available > *length) return STORE_CORRUPT;
    err = nvs_get_blob(nvs->handle, "state", buffer, &available);
    if (err != ESP_OK) return STORE_IO;
    *length = available;
    return STORE_OK;
}

static StoreResult idf_write_commit(void *context, const uint8_t *buffer, size_t length)
{
    StoreIdfNvs *nvs = (StoreIdfNvs *)context;
    if (!nvs || !nvs->opened || !buffer || length == 0 ||
        length > KATMED_STORE_BLOB_MAX) return STORE_INVALID;
    esp_err_t err = nvs_set_blob(nvs->handle, "state", buffer, length);
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) return STORE_FULL;
    if (err != ESP_OK) return STORE_IO;
    err = nvs_commit(nvs->handle);
    if (err != ESP_OK) return STORE_COMMIT_UNKNOWN;
    return STORE_OK;
}

StoreResult store_idf_open_backend(StoreIdfNvs *nvs, StoreBackend *backend)
{
    if (!nvs || !backend) return STORE_INVALID;
    memset(nvs, 0, sizeof(*nvs));
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) return STORE_FULL;
    if (err == ESP_ERR_NVS_NEW_VERSION_FOUND) return STORE_UNSUPPORTED;
    if (err != ESP_OK) return STORE_IO;
    err = nvs_open("katmed", NVS_READWRITE, &nvs->handle);
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) return STORE_FULL;
    if (err != ESP_OK) return STORE_IO;
    nvs->opened = true;
    *backend = (StoreBackend){.context=nvs, .read=idf_read, .write_commit=idf_write_commit};
    return STORE_OK;
}

void store_idf_close_backend(StoreIdfNvs *nvs)
{
    if (!nvs || !nvs->opened) return;
    nvs_close(nvs->handle);
    nvs->opened = false;
}

/* Separate additive key. Called only by the serial application/mesh poll. */
static StoreResult history_read(void *context, uint8_t *buffer, size_t *length)
{
    StoreIdfNvs *nvs = context;
    if (!nvs || !nvs->opened || !buffer || !length) return STORE_UNAVAILABLE;
    size_t size = 0;
    esp_err_t err = nvs_get_blob(nvs->handle, "mesh_history", NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) return STORE_NOT_FOUND;
    if (err != ESP_OK) return STORE_IO;
    if ((size != 2080 && size != 2152) || size > *length) return STORE_CORRUPT;
    err = nvs_get_blob(nvs->handle, "mesh_history", buffer, &size);
    if (err != ESP_OK) return STORE_IO;
    *length = size;
    return STORE_OK;
}
static StoreResult history_write(void *context, const uint8_t *buffer, size_t length)
{
    StoreIdfNvs *nvs = context;
    if (!nvs || !nvs->opened || !buffer || length != 2152) return STORE_UNAVAILABLE;
    /* Stats describe the actual live partition, not a promise of contiguous
     * blob capacity. Only set/commit can decide whether replacement fits. */
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) != ESP_OK) return STORE_IO;
    esp_err_t err = nvs_set_blob(nvs->handle, "mesh_history", buffer, length);
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) return STORE_FULL;
    if (err != ESP_OK) return STORE_IO;
    if (nvs_commit(nvs->handle) != ESP_OK) return STORE_COMMIT_UNKNOWN;
    return STORE_OK;
}
StoreBackend store_idf_history_backend(StoreIdfNvs *nvs)
{
    return (StoreBackend){.context=nvs, .read=history_read, .write_commit=history_write};
}
