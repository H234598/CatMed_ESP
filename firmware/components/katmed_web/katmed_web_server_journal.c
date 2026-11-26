#include "katmed_web_server_journal.h"
#include "katmed_source_export.h"
#include <stdlib.h>
#include <string.h>

#define MARKER_MAGIC UINT32_C(0x4b574d32)

typedef struct { uint32_t magic; uint32_t slot; } ServerMarker;

static const char *slot_key(unsigned slot) { return slot ? "server1" : "server0"; }

static bool record_valid(const KatmedServerRecord *record)
{
    return record && record->magic == KATMED_SERVER_RECORD_MAGIC && record->chain_len > 1 &&
        record->chain_len <= sizeof(record->chain) &&
        record->chain[record->chain_len - 1] == 0 &&
        strnlen(record->chain, record->chain_len) == (size_t)record->chain_len - 1u &&
        record->dns[0] && strnlen(record->dns, sizeof(record->dns)) < sizeof(record->dns);
}

static void wipe(void *p, size_t length)
{
    volatile unsigned char *bytes = p;
    while (length--) *bytes++ = 0;
}

static KatmedServerLoad marker_read(nvs_handle_t nvs, unsigned *slot)
{
    ServerMarker marker = {0};
    size_t size = sizeof(marker);
    esp_err_t result = nvs_get_blob(nvs, "server_active", &marker, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) return KATMED_SERVER_LEGACY;
    if (result != ESP_OK || size != sizeof(marker) || marker.magic != MARKER_MAGIC ||
        marker.slot > 1) return KATMED_SERVER_STORAGE_ERROR;
    *slot = marker.slot;
    return KATMED_SERVER_ACTIVE;
}

KatmedServerLoad katmed_server_journal_load(nvs_handle_t nvs, KatmedServerRecord *out,
                                            unsigned *slot)
{
    if (!out || !slot) return KATMED_SERVER_STORAGE_ERROR;
    unsigned selected = 0;
    KatmedServerLoad result = marker_read(nvs, &selected);
    if (result != KATMED_SERVER_ACTIVE) return result;
    size_t size = sizeof(*out);
    if (nvs_get_blob(nvs, slot_key(selected), out, &size) != ESP_OK ||
        size != sizeof(*out) || !record_valid(out)) {
        wipe(out, sizeof(*out));
        return KATMED_SERVER_STORAGE_ERROR;
    }
    *slot = selected;
    return KATMED_SERVER_ACTIVE;
}

bool katmed_server_journal_stage(nvs_handle_t nvs, const KatmedServerRecord *record,
                                 unsigned *slot)
{
    if (!record_valid(record) || !slot) return false;
    unsigned active = 0;
    KatmedServerLoad state = marker_read(nvs, &active);
    if (state == KATMED_SERVER_STORAGE_ERROR) return false;
    unsigned target = state == KATMED_SERVER_ACTIVE ? active ^ 1u : 0u;
    KatmedServerRecord *check = malloc(sizeof(*check));
    if (!check || !katmed_source_writer_enter()) { free(check); return false; }
    bool ok = nvs_set_blob(nvs, slot_key(target), record, sizeof(*record)) == ESP_OK &&
              nvs_commit(nvs) == ESP_OK;
    size_t size = sizeof(*check);
    if (ok) ok = nvs_get_blob(nvs, slot_key(target), check, &size) == ESP_OK &&
                 size == sizeof(*check) && memcmp(record, check, size) == 0;
    katmed_source_writer_leave();
    wipe(check, sizeof(*check));
    free(check);
    if (ok) *slot = target;
    return ok;
}

bool katmed_server_journal_commit(nvs_handle_t nvs, unsigned slot)
{
    if (slot > 1 || !katmed_source_writer_enter()) return false;
    KatmedServerRecord *record = malloc(sizeof(*record));
    size_t length = sizeof(*record);
    if (!record || nvs_get_blob(nvs, slot_key(slot), record, &length) != ESP_OK ||
        length != sizeof(*record) || !record_valid(record)) {
        if (record) { wipe(record, sizeof(*record)); free(record); }
        katmed_source_writer_leave();
        return false;
    }
    wipe(record, sizeof(*record));
    free(record);
    ServerMarker marker = {.magic = MARKER_MAGIC, .slot = slot};
    bool ok = nvs_set_blob(nvs, "server_active", &marker, sizeof(marker)) == ESP_OK &&
              nvs_commit(nvs) == ESP_OK;
    unsigned actual = 2;
    if (ok) ok = marker_read(nvs, &actual) == KATMED_SERVER_ACTIVE && actual == slot;
    katmed_source_writer_leave();
    return ok;
}
