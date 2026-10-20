#include "katmed_import.h"

#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "katmed_web_import.h"

#if CONFIG_NVS_ENCRYPTION
/* Factory MAC is the same read-only local identity used by boot_identity().
 * Never use the staged capsule or mutable NVS as the authority. */
static bool local_device_id(uint64_t *out)
{
    uint8_t mac[6];
    if (!out || esp_efuse_mac_get_default(mac) != ESP_OK) return false;
    uint64_t device = 0;
    for (unsigned i = 0; i < sizeof(mac); ++i)
        device = (device << 8) | mac[i];
    *out = device;
    return device != 0;
}

static KatmedImportRead read_blob(void *context, const char *ns, const char *key,
                                  uint8_t *out, size_t *length)
{
    (void)context;
    if (!ns || !key || !length) return KATMED_IMPORT_READ_ERROR;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return KATMED_IMPORT_READ_ABSENT;
    if (err != ESP_OK) return KATMED_IMPORT_READ_ERROR;
    err = nvs_get_blob(handle, key, out, length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return KATMED_IMPORT_READ_ABSENT;
    return err == ESP_OK ? KATMED_IMPORT_READ_OK : KATMED_IMPORT_READ_ERROR;
}

static bool write_blob(void *context, const char *ns, const char *key,
                       const uint8_t *data, size_t length)
{
    (void)context;
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, key, data, length);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

static bool erase_blob(void *context, const char *ns, const char *key)
{
    (void)context;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;
    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

static bool target_empty(void *context)
{
    (void)context;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", NULL, NVS_TYPE_ANY, &it);
    bool empty = err == ESP_ERR_NVS_NOT_FOUND;
    nvs_release_iterator(it);
    return empty;
}

static bool free_entries(void *context, size_t *out)
{
    (void)context;
    nvs_stats_t stats;
    if (!out || nvs_get_stats("nvs", &stats) != ESP_OK) return false;
    *out = stats.free_entries;
    return true;
}

static bool validate_web(void *context, const uint8_t *auth, size_t auth_len,
                         const uint8_t *identity, size_t identity_len)
{
    (void)context;
    return katmed_web_import_records_valid(auth, auth_len, identity, identity_len);
}

static KatmedImportBackend backend(void)
{
    KatmedImportBackend b = {
        .context = NULL, .read = read_blob, .write = write_blob,
        .erase = erase_blob, .target_empty = target_empty,
        .free_entries = free_entries, .validate_web = validate_web
    };
    return b;
}
#endif

bool katmed_import_apply_idf(const KatmedImportInput *input)
{
#if CONFIG_NVS_ENCRYPTION
    uint64_t device = 0;
    if (!input || !local_device_id(&device) ||
        input->expected_device != device) return false;
    KatmedImportBackend b = backend();
    return katmed_import_apply(input, &b);
#else
    (void)input;
    return false;
#endif
}

bool katmed_import_resume_idf(void)
{
#if CONFIG_NVS_ENCRYPTION
    uint64_t device = 0;
    if (!local_device_id(&device)) return false;
    KatmedImportBackend b = backend();
    return katmed_import_resume(&b, device);
#else
    return false;
#endif
}

bool katmed_import_abort_idf(void)
{
#if CONFIG_NVS_ENCRYPTION
    KatmedImportBackend b = backend();
    return katmed_import_abort(&b);
#else
    return false;
#endif
}
