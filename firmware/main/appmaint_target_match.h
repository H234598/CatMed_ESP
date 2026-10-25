#ifndef KATMED_APPMAINT_TARGET_MATCH_H
#define KATMED_APPMAINT_TARGET_MATCH_H

#include <stdbool.h>
#include <stdint.h>
#include "katmed_appmaint_target.h"

static inline bool katmed_appmaint_target_matches(const uint8_t mac48[6])
{
    if (!mac48) return false;
    uint64_t actual = 0;
    for (unsigned i = 0; i < 6; ++i) actual = (actual << 8) | mac48[i];
    return actual != 0 && actual == KATMED_APPMAINT_TARGET_MAC48;
}

#endif
