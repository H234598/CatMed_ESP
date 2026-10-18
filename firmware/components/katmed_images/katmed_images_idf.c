#include "katmed_images_internal.h"
#include "esp_partition.h"

static bool partition_read(void *context, uint32_t offset, uint8_t *dst,
                           size_t wanted, size_t *actual)
{
    const esp_partition_t *partition = (const esp_partition_t *)context;
    if(actual) *actual = 0;
    if(partition == NULL || offset > partition->size || wanted > partition->size - offset)
        return false;
    if(esp_partition_read(partition, offset, dst, wanted) != ESP_OK) return false;
    if(actual) *actual = wanted;
    return true;
}

KatmedImageStatus katmed_images_mount_partition(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "images");
    if(partition == NULL) {
        katmed_images_unmount();
        return KATMED_IMAGE_UNAVAILABLE;
    }
    KatmedStorage storage = {
        .context = (void *)partition,
        .partition_size = partition->size,
        .read = partition_read,
    };
    return katmed_images_mount_storage(storage);
}
