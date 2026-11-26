#include "katmed_web_server_journal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { unsigned char data[sizeof(KatmedServerRecord)]; size_t len; } Blob;
static Blob saved[3], pending;
static int pending_index = -1, fail_set, fail_commit, fail_get, fail_target_read = -1;
static int index_for(const char *key)
{
    if (!strcmp(key, "server0")) return 0;
    if (!strcmp(key, "server1")) return 1;
    if (!strcmp(key, "server_active")) return 2;
    assert(0);
    return -1;
}
bool katmed_source_writer_enter(void) { return true; }
void katmed_source_writer_leave(void) { }
esp_err_t nvs_get_blob(nvs_handle_t nvs, const char *key, void *out, size_t *len)
{
    (void)nvs;
    int index = index_for(key);
    if (fail_get || index == fail_target_read) return -1;
    Blob *b = &saved[index];
    if (!b->len) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < b->len) return -1;
    memcpy(out, b->data, b->len); *len = b->len; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t nvs, const char *key, const void *data, size_t len)
{
    (void)nvs;
    if (fail_set) return -1;
    assert(len <= sizeof(pending.data));
    pending_index = index_for(key); pending.len = len; memcpy(pending.data, data, len);
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t nvs)
{
    (void)nvs;
    if (fail_commit) { pending_index = -1; return -1; }
    assert(pending_index >= 0); saved[pending_index] = pending; pending_index = -1;
    return ESP_OK;
}

int main(void)
{
    KatmedServerRecord a = {.magic = KATMED_SERVER_RECORD_MAGIC, .chain_len = 2,
                            .key = {1}, .dns = "a.example", .chain = "a"};
    KatmedServerRecord b = {.magic = KATMED_SERVER_RECORD_MAGIC, .chain_len = 2,
                            .key = {2}, .dns = "b.example", .chain = "b"};
    KatmedServerRecord out = {0}; unsigned slot = 3, loaded = 3;
    assert(katmed_server_journal_load(1, &out, &loaded) == KATMED_SERVER_LEGACY);
    assert(katmed_server_journal_stage(1, &a, &slot) && slot == 0);
    assert(katmed_server_journal_load(1, &out, &loaded) == KATMED_SERVER_LEGACY);
    assert(katmed_server_journal_commit(1, slot));
    assert(katmed_server_journal_load(1, &out, &loaded) == KATMED_SERVER_ACTIVE &&
           loaded == 0 && !memcmp(&out, &a, sizeof(a)));
    fail_set = 1; assert(!katmed_server_journal_stage(1, &b, &slot)); fail_set = 0;
    fail_commit = 1; assert(!katmed_server_journal_stage(1, &b, &slot)); fail_commit = 0;
    fail_target_read = 1; assert(!katmed_server_journal_stage(1, &b, &slot)); fail_target_read = -1;
    assert(katmed_server_journal_load(1, &out, &loaded) == KATMED_SERVER_ACTIVE &&
           !memcmp(&out, &a, sizeof(a)));
    assert(katmed_server_journal_stage(1, &b, &slot) && slot == 1);
    /* A reboot before commitment must still select a. */
    assert(katmed_server_journal_load(1, &out, &loaded) == KATMED_SERVER_ACTIVE &&
           !memcmp(&out, &a, sizeof(a)));
    fail_commit = 1; assert(!katmed_server_journal_commit(1, slot)); fail_commit = 0;
    assert(katmed_server_journal_load(1, &out, &loaded) == KATMED_SERVER_ACTIVE &&
           !memcmp(&out, &a, sizeof(a)));
    assert(katmed_server_journal_commit(1, slot));
    assert(katmed_server_journal_load(1, &out, &loaded) == KATMED_SERVER_ACTIVE &&
           !memcmp(&out, &b, sizeof(b)));
    fail_get = 1; assert(katmed_server_journal_load(1, &out, &loaded) == KATMED_SERVER_STORAGE_ERROR);
    puts("NVS journal stage, commit and interrupted replay: PASS");
    return 0;
}
