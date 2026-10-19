#ifndef TEST_KATMED_IMAGES_H
#define TEST_KATMED_IMAGES_H
#include <stdint.h>
#include "katmed_image_store.h"
typedef struct { uint32_t bytes; uint16_t width, height; } KatmedImageVariantInfo;
uint32_t katmed_images_builtin_count(void);
uint32_t katmed_images_builtin_profile_count(void);
KatmedImageStatus katmed_images_builtin_variant_info(uint32_t index,
                                                      uint32_t profile,
                                                      KatmedImageVariantInfo *out);
#endif
