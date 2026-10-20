#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "katmed_reset_idf.h"
#include "katmed_image_store.h"
#include "nvs.h"

static bool prepared, staged_prepared;
static bool journal_exists, staged_journal_exists;
static uint8_t journal[2], staged_journal[2];
static bool fail_commit;
static unsigned image_clears, acme_clears, namespace_clears;
static bool fail_acme;

esp_err_t katmed_nvs_init(void) { return ESP_OK; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out)
{
    (void)mode;
    *out = !strcmp(name, "katmed_reset") ? 1 : 2;
    staged_prepared = prepared;
    staged_journal_exists = journal_exists;
    memcpy(staged_journal, journal, 2);
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{
    assert(h == 1 && !strcmp(key, "prepared") && value == 1);
    staged_prepared = true;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *value)
{
    assert(h == 1 && !strcmp(key, "prepared"));
    if (!prepared) return ESP_ERR_NVS_NOT_FOUND;
    *value = 1;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    assert(h == 1 && !strcmp(key, "prepared"));
    if (!prepared && !staged_prepared) return ESP_ERR_NVS_NOT_FOUND;
    staged_prepared = false;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size)
{
    assert(h == 1 && !strcmp(key, "journal") && size == 2);
    memcpy(staged_journal, data, 2);
    staged_journal_exists = true;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *size)
{
    assert(h == 1 && !strcmp(key, "journal") && *size == 2);
    if (!journal_exists) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(data, journal, 2);
    *size = 2;
    return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t h)
{
    assert(h == 2);
    ++namespace_clears;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    if (fail_commit) return ESP_FAIL;
    prepared = staged_prepared;
    journal_exists = staged_journal_exists;
    memcpy(journal, staged_journal, 2);
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
KatmedImageStatus katmed_image_store_clear_user_data(void)
{
    ++image_clears;
    return KATMED_IMAGE_OK;
}
bool katmed_reset_acme_files(void)
{
    ++acme_clears;
    return !fail_acme;
}

int main(void)
{
    bool warning = true;
    assert(katmed_reset_idf_take_preparation(&warning) && !warning);
    assert(image_clears == 0 && namespace_clears == 0);

    assert(katmed_reset_idf_prepare());
    assert(prepared && image_clears == 0 && !journal_exists);
    assert(katmed_reset_idf_cancel_preparation());
    assert(katmed_reset_idf_take_preparation(&warning) && !warning);

    assert(katmed_reset_idf_prepare());
    assert(katmed_reset_idf_take_preparation(&warning) && warning);
    assert(katmed_reset_idf_take_preparation(&warning) && !warning);
    assert(image_clears == 0 && !journal_exists);

    fail_commit = true;
    assert(!katmed_reset_idf_prepare());
    assert(!prepared);
    fail_commit = false;
    assert(katmed_reset_idf_take_preparation(&warning) && !warning);

    /* Only a separate, confirmed journal begin may touch user data. */
    KatmedResetJournalBackend backend = katmed_reset_idf_backend();
    assert(katmed_reset_begin(backend));
    assert(journal_exists && journal[0] == 1 && image_clears == 0 && acme_clears == 0);
    bool pending = false;
    fail_acme = true;
    assert(!katmed_reset_resume(backend, &pending));
    assert(journal[0] == 1 && journal[1] == KATMED_RESET_FILES);
    assert(image_clears == 1 && acme_clears == 1 && namespace_clears == 0);
    fail_acme = false;
    assert(katmed_reset_resume(backend, &pending) && !pending);
    assert(image_clears == 2 && acme_clears == 2 && namespace_clears == 3);
    return 0;
}
