#include "katmed_nvs_init.h"
#include "katmed_security_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_partition.h"
#include "nvs_flash.h"

#if CONFIG_NVS_ENCRYPTION
#include "esp_flash_encrypt.h"
#include "esp_rom_crc.h"

/* Public state only. This records that this firmware first mounted an empty
 * NVS partition with secure init; it is not an authentication tag. */
static const uint8_t nvs_guard_marker[16] = {
    'K', 'A', 'T', 'M', 'E', 'D', '-', 'N', 'V', 'S', '-', 'S', '1', 0, 0, 1
};

typedef enum { GUARD_BLANK, GUARD_PRESENT, GUARD_INVALID } guard_state_t;

static guard_state_t guard_state(const esp_partition_t *partition)
{
    uint8_t bytes[256];
    bool blank = true;
    bool present = true;
    for (size_t offset = 0; offset < partition->size; offset += sizeof(bytes)) {
        size_t length = partition->size - offset < sizeof(bytes) ?
            partition->size - offset : sizeof(bytes);
        if (esp_partition_read_raw(partition, offset, bytes, length) != ESP_OK)
            return GUARD_INVALID;
        for (size_t i = 0; i < length; ++i) {
            uint8_t expected = offset + i < sizeof(nvs_guard_marker) ?
                nvs_guard_marker[offset + i] : 0xff;
            if (bytes[i] != 0xff) blank = false;
            if (bytes[i] != expected) present = false;
        }
    }
    return blank ? GUARD_BLANK : present ? GUARD_PRESENT : GUARD_INVALID;
}

static bool layout_matches(const esp_partition_t *partition, const char *label,
                           esp_partition_subtype_t subtype, uint32_t address,
                           uint32_t size, bool encrypted)
{
    return partition && partition->type == ESP_PARTITION_TYPE_DATA &&
        partition->subtype == subtype && strcmp(partition->label, label) == 0 &&
        partition->address == address && partition->size == size &&
        partition->encrypted == encrypted && !partition->readonly;
}

/* IDF 6.1 nvs_constants.h/nvs_types.cpp: 4-KiB pages, entry state table
 * at +32, 126 32-byte entries at +64, and Item CRC over [0:4],[8:32].
 * A matching raw CRC is evidence of a plaintext entry, so reject it. A
 * mismatch does NOT prove the page is encrypted or safe to mount. */
static bool raw_plaintext_entry_or_read_error(const esp_partition_t *partition)
{
    uint8_t states[32], item[32];
    for (size_t page = 0; page < partition->size; page += 4096) {
        if (esp_partition_read_raw(partition, page + 32, states,
                                   sizeof(states)) != ESP_OK) return true;
        for (size_t i = 0; i < 126; ++i) {
            if (((states[i / 4] >> ((i % 4) * 2)) & 3u) != 2u) continue;
            if (esp_partition_read_raw(partition, page + 64 + i * 32,
                                       item, sizeof(item)) != ESP_OK) return true;
            uint32_t crc = 0xffffffff;
            crc = esp_rom_crc32_le(crc, item, 4);
            crc = esp_rom_crc32_le(crc, item + 8, 24);
            uint32_t stored = (uint32_t)item[4] | ((uint32_t)item[5] << 8) |
                ((uint32_t)item[6] << 16) | ((uint32_t)item[7] << 24);
            if (crc == stored) return true;
        }
    }
    return false;
}

static bool partition_blank(const esp_partition_t *partition, bool encrypted)
{
    uint8_t bytes[256];
    for (size_t offset = 0; offset < partition->size; offset += sizeof(bytes)) {
        const size_t length = partition->size - offset < sizeof(bytes) ?
            partition->size - offset : sizeof(bytes);
        esp_err_t err = encrypted ? esp_partition_read(partition, offset, bytes, length) :
            esp_partition_read_raw(partition, offset, bytes, length);
        if (err != ESP_OK)
            return false;
        for (size_t i = 0; i < length; ++i)
            if (bytes[i] != 0xff) return false;
    }
    return true;
}

static void clear_security_cfg(nvs_sec_cfg_t *cfg)
{
    volatile uint8_t *bytes = (volatile uint8_t *)cfg;
    for (size_t i = 0; i < sizeof(*cfg); ++i) bytes[i] = 0;
}
#endif

esp_err_t katmed_nvs_init(void)
{
    static bool initialized;
    if (initialized) return ESP_OK;
#if CONFIG_NVS_ENCRYPTION
    /* This checks the running device, but cannot stop the bootloader's earlier
     * first-boot eFuse action. The build profile must never be flashed casually. */
    if (!esp_flash_encryption_enabled()) return ESP_ERR_INVALID_STATE;
    const esp_partition_t *keys = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "nvs_keys");
    const esp_partition_t *data = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "nvs");
    const esp_partition_t *user_images = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        KATMED_USER_IMAGE_PARTITION_LABEL);
    const esp_partition_t *phy = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "phy_init");
    const esp_partition_t *guard = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "nvs_guard");
    if (!layout_matches(data, "nvs", ESP_PARTITION_SUBTYPE_DATA_NVS,
                        0x11000, 0x7000, false) ||
        !layout_matches(keys, "nvs_keys", ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS,
                        0x18000, 0x1000, true) ||
        !layout_matches(guard, "nvs_guard", 0x40,
                        0x19000, 0x1000, false) ||
        !layout_matches(phy, "phy_init", ESP_PARTITION_SUBTYPE_DATA_PHY,
                        0x1a000, 0x1000, false) ||
        !layout_matches(user_images, KATMED_USER_IMAGE_PARTITION_LABEL,
                        ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
                        0x236000, 0x90000, true))
        return ESP_ERR_INVALID_STATE;

    guard_state_t state = guard_state(guard);
    if (state == GUARD_INVALID) return ESP_ERR_INVALID_STATE;
    /* An unmarked populated NVS is an explicit migration boundary even when
     * valid keys happen to be present. The first secure mount is blank only. */
    if (state == GUARD_BLANK && !partition_blank(data, false))
        return ESP_ERR_INVALID_STATE;
    if (state == GUARD_PRESENT && raw_plaintext_entry_or_read_error(data))
        return ESP_ERR_INVALID_STATE;

    nvs_sec_cfg_t cfg = {0};
    esp_err_t err = nvs_flash_read_security_cfg(keys, &cfg);
    if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        /* A partly written key sector or old plaintext NVS needs an explicit,
         * device-specific recovery/migration decision. Never erase either. */
        if (state != GUARD_BLANK || !partition_blank(keys, true)) {
            clear_security_cfg(&cfg);
            return ESP_ERR_INVALID_STATE;
        }
        err = nvs_flash_generate_keys(keys, &cfg);
    }
    if (err == ESP_OK) err = nvs_flash_secure_init(&cfg);
    if (err == ESP_OK && state == GUARD_BLANK)
        err = esp_partition_write(guard, 0, nvs_guard_marker,
                                  sizeof(nvs_guard_marker));
    clear_security_cfg(&cfg);
    if (err == ESP_OK) initialized = true;
    return err;
#else
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) initialized = true;
    return err;
#endif
}
