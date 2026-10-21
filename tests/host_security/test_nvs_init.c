#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "katmed_nvs_init.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "esp_rom_crc.h"

uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *data, uint32_t length)
{
    crc = ~crc;
    for (uint32_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* After the bootloader encrypts an empty key sector, raw flash is ciphertext
 * while esp_partition_read decrypts it back to erased (0xff) bytes. */
static uint8_t key_bytes[0x1000], key_raw_bytes[0x1000];
static uint8_t nvs_bytes[0x7000], image_bytes[0x90000], guard_bytes[0x1000];
static uint8_t phy_bytes[0x1000];
static esp_partition_t keys = {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS,
    0x18000, sizeof(key_bytes), "nvs_keys", true, false, key_bytes};
static esp_partition_t data = {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
    0x11000, sizeof(nvs_bytes), "nvs", false, false, nvs_bytes};
static esp_partition_t images = {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
    0x230000, sizeof(image_bytes), "user_images", true, false, image_bytes};
static esp_partition_t guard = {ESP_PARTITION_TYPE_DATA, 0x40,
    0x19000, sizeof(guard_bytes), "nvs_guard", false, false, guard_bytes};
static esp_partition_t phy = {ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY,
    0x1a000, sizeof(phy_bytes), "phy_init", false, false, phy_bytes};
static bool enabled = true;
static bool missing_keys;
static esp_err_t read_result = ESP_ERR_NVS_KEYS_NOT_INITIALIZED;
static esp_err_t init_result = ESP_OK;
static int generated, initialized;
static int marker_writes;

bool esp_flash_encryption_enabled(void) { return enabled; }
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    assert(type == ESP_PARTITION_TYPE_DATA);
    if (strcmp(label, "nvs_keys") == 0) {
        assert(subtype == ESP_PARTITION_SUBTYPE_ANY);
        return missing_keys ? NULL : &keys;
    }
    if (strcmp(label, "nvs") == 0) return &data;
    if (strcmp(label, "user_images") == 0) return &images;
    if (strcmp(label, "nvs_guard") == 0) return &guard;
    if (strcmp(label, "phy_init") == 0) return &phy;
    assert(0);
    return NULL;
}
esp_err_t esp_partition_read_raw(const esp_partition_t *p, size_t off, void *dst, size_t len)
{
    assert(off + len <= p->size);
    memcpy(dst, (p == &keys ? key_raw_bytes : p->storage) + off, len);
    return ESP_OK;
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t len)
{
    assert(p == &keys);
    memcpy(dst, p->storage + off, len);
    return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t len)
{
    assert(p == &guard && off == 0 && len == 16);
    memcpy(p->storage + off, src, len);
    marker_writes++;
    return ESP_OK;
}
esp_err_t nvs_flash_read_security_cfg(const esp_partition_t *p, nvs_sec_cfg_t *cfg)
{
    assert(p == &keys);
    memset(cfg, 0x42, sizeof(*cfg));
    return read_result;
}
esp_err_t nvs_flash_generate_keys(const esp_partition_t *p, nvs_sec_cfg_t *cfg)
{
    assert(p == &keys && cfg != NULL);
    generated++;
    return ESP_OK;
}
esp_err_t nvs_flash_secure_init(nvs_sec_cfg_t *cfg)
{
    assert(cfg != NULL);
    initialized++;
    return init_result;
}
esp_err_t nvs_flash_init(void) { assert(0); return ESP_FAIL; }

int main(int argc, char **argv)
{
    memset(key_bytes, 0xff, sizeof(key_bytes));
    memset(key_raw_bytes, 0x42, sizeof(key_raw_bytes));
    memset(nvs_bytes, 0xff, sizeof(nvs_bytes));
    memset(guard_bytes, 0xff, sizeof(guard_bytes));
    if (argc == 3 && (strcmp(argv[1], "plaintext") == 0 ||
                      strcmp(argv[1], "plaintext-marked") == 0)) {
        FILE *input = fopen(argv[2], "rb");
        assert(input != NULL);
        assert(fread(nvs_bytes, 1, sizeof(nvs_bytes), input) == sizeof(nvs_bytes));
        assert(fclose(input) == 0);
        assert(nvs_bytes[0] != 0xff); /* Real IDF-generated NVS page. */
        if (strcmp(argv[1], "plaintext-marked") == 0) {
            static const uint8_t marker[16] = {
                'K', 'A', 'T', 'M', 'E', 'D', '-', 'N', 'V', 'S', '-', 'S', '1', 0, 0, 1
            };
            memcpy(guard_bytes, marker, sizeof(marker));
        }
        read_result = ESP_OK;
        assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE &&
               generated == 0 && initialized == 0 && marker_writes == 0);
        puts("IDF plaintext NVS with valid keys blocked before mount");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "marked-reboot") == 0) {
        /* Simulated next process: an earlier successful secure mount left its
         * marker and nonblank NVS. No key generation or marker rewrite. */
        static const uint8_t marker[16] = {
            'K', 'A', 'T', 'M', 'E', 'D', '-', 'N', 'V', 'S', '-', 'S', '1', 0, 0, 1
        };
        memcpy(guard_bytes, marker, sizeof(marker));
        nvs_bytes[0] = 0x42;
        read_result = ESP_OK;
        assert(katmed_nvs_init() == ESP_OK && generated == 0 && initialized == 1 && marker_writes == 0);
        puts("NVS marked reboot case passed");
        return 0;
    }
    enabled = false;
    assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE);
    enabled = true;
    missing_keys = true;
    assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE);
    missing_keys = false;
    images.encrypted = false;
    assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE);
    images.encrypted = true;
    /* Every critical field is checked before key generation or NVS mount. */
    esp_partition_t *parts[] = {&data, &keys, &images, &guard, &phy};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        esp_partition_t *p = parts[i];
        uint32_t address = p->address;
        size_t size = p->size;
        int subtype = p->subtype;
        p->address += 0x1000;
        assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE && generated == 0 && initialized == 0);
        p->address = address;
        p->size -= 0x1000;
        assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE && generated == 0 && initialized == 0);
        p->size = size;
        p->subtype = 0x7f;
        assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE && generated == 0 && initialized == 0);
        p->subtype = subtype;
        p->type = 0;
        assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE && generated == 0 && initialized == 0);
        p->type = ESP_PARTITION_TYPE_DATA;
        p->encrypted = !p->encrypted;
        assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE && generated == 0 && initialized == 0);
        p->encrypted = !p->encrypted;
        p->readonly = true;
        assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE && generated == 0 && initialized == 0);
        p->readonly = false;
    }
    /* Valid keys plus old plaintext pages are stopped before secure init. */
    read_result = ESP_OK;
    nvs_bytes[0] = 0x42;
    assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE && initialized == 0);
    nvs_bytes[0] = 0xff;
    read_result = ESP_ERR_NVS_KEYS_NOT_INITIALIZED;
    guard_bytes[0] = 0;
    assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE && initialized == 0);
    guard_bytes[0] = 0xff;
    nvs_bytes[sizeof(nvs_bytes) - 1] = 0;
    assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE && generated == 0);
    nvs_bytes[sizeof(nvs_bytes) - 1] = 0xff;
    key_bytes[0xfff] = 0;
    assert(katmed_nvs_init() == ESP_ERR_INVALID_STATE && generated == 0);
    key_bytes[0xfff] = 0xff;
    read_result = ESP_ERR_NVS_CORRUPT_KEY_PART;
    assert(katmed_nvs_init() == ESP_ERR_NVS_CORRUPT_KEY_PART && generated == 0);
    read_result = ESP_ERR_NVS_KEYS_NOT_INITIALIZED;
    init_result = ESP_FAIL;
    assert(katmed_nvs_init() == ESP_FAIL && generated == 1 && initialized == 1);
    init_result = ESP_OK;
    read_result = ESP_OK; /* Keys survive a failed NVS mount; retry reads them. */
    assert(katmed_nvs_init() == ESP_OK && generated == 1 && initialized == 2 && marker_writes == 1);
    assert(katmed_nvs_init() == ESP_OK && generated == 1 && initialized == 2);
    puts("NVS safety cases passed");
}
