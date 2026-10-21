/* Compile the actual IDF import callback with a synthetic nvs_get_stats.
 * The reserved GC page is part of free_entries but not available_entries. */
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "../../firmware/components/katmed_import/katmed_import_idf.c"

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
