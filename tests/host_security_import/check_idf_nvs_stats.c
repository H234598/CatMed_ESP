/* Compile the actual IDF import callback with a synthetic nvs_get_stats.
 * The reserved GC page is part of free_entries but not available_entries. */
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "../../firmware/components/katmed_import/katmed_import_idf.c"

static bool mac_c6,mac_error,mac_zero;
static unsigned legacy_calls,factory_calls;
static const uint8_t factory[6]={0x02,0x31,0x42,0x53,0x64,0x75};
esp_err_t esp_efuse_mac_get_default(uint8_t mac[6])
{
    ++legacy_calls;if(mac_error)return ESP_FAIL;
    if(mac_c6){
        const uint8_t eui[8]={0x02,0x31,0x42,0xff,0xfe,0x53,0x64,0x75};
        struct {uint8_t six[6],sentinel[2];} capacity={{0},{0xa5,0x5a}};
        memcpy(&capacity,eui,sizeof(eui));
        assert(capacity.sentinel[0]!=0xa5 && capacity.sentinel[1]!=0x5a);
        memcpy(mac,capacity.six,6); /* bounded mock exposes old wrong fold, no host overflow */
    }else memcpy(mac,factory,6);
    return ESP_OK;
}
esp_err_t esp_read_mac(uint8_t *mac,esp_mac_type_t type)
{
    assert(type==ESP_MAC_EFUSE_FACTORY);++factory_calls;
    if(mac_error)return ESP_FAIL;
    if(mac_zero)memset(mac,0,6);else memcpy(mac,factory,6);
    return ESP_OK;
}
static void check_factory_identity(void)
{
    for(unsigned chip=0;chip<2;++chip){
        mac_c6=chip==0;mac_error=mac_zero=false;legacy_calls=factory_calls=0;
        uint64_t device=0;assert(local_device_id(&device));
        assert(device==UINT64_C(0x023142536475));
        assert(legacy_calls==0 && factory_calls==1);
        struct {uint8_t six[6],sentinel[2];} out={{0},{0xa5,0x5a}};
        assert(esp_read_mac(out.six,ESP_MAC_EFUSE_FACTORY)==ESP_OK);
        assert(out.sentinel[0]==0xa5 && out.sentinel[1]==0x5a);
        const unsigned calls=factory_calls;assert(!local_device_id(NULL) && factory_calls==calls);
        device=UINT64_C(0x1234);mac_error=true;assert(!local_device_id(&device) && device==UINT64_C(0x1234));
        mac_error=false;mac_zero=true;assert(!local_device_id(&device) && device==0);
        mac_zero=false;
    }
}

static nvs_stats_t reported;
static esp_err_t stats_result = ESP_OK;
static unsigned queries;
esp_err_t nvs_get_stats(const char *partition, nvs_stats_t *out)
{
    assert(strcmp(partition, "nvs") == 0 && out);
    ++queries;
    *out = reported;
    return stats_result;
}

int main(void)
{
    check_factory_identity();
    size_t usable = 0;
    reported.free_entries = 882;
    reported.available_entries = 756;
    reported.total_entries = 882;
    assert(available_entries(NULL, &usable) && usable == 756 && queries == 1);
    reported.free_entries = 756;
    reported.available_entries = 630;
    assert(available_entries(NULL, &usable) && usable == 630 && queries == 2);
    stats_result = ESP_FAIL;
    assert(!available_entries(NULL, &usable) && queries == 3);
    assert(!available_entries(NULL, NULL) && queries == 3);
    return 0;
}
