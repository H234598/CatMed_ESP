#include "sdk_stubs.h"
int esp_efuse_mac_get_default(uint8_t *mac);
enum { ESP_MAC_EFUSE_FACTORY = 6 };
int esp_read_mac(uint8_t *mac, int type);
