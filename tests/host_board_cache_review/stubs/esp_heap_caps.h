#ifndef KATMED_HOST_BOARD_ESP_HEAP_CAPS_H
#define KATMED_HOST_BOARD_ESP_HEAP_CAPS_H

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_DMA UINT32_C(8)
size_t heap_caps_get_largest_free_block(uint32_t caps);

#endif
