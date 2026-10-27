#ifndef KATMED_HOST_BOARD_ESP_HEAP_CAPS_H
#define KATMED_HOST_BOARD_ESP_HEAP_CAPS_H

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT UINT32_C(0x0004)
#define MALLOC_CAP_DMA UINT32_C(8)
#define MALLOC_CAP_INTERNAL UINT32_C(0x0800)
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);

#endif
