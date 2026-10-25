#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "appmaint_target_match.h"

int main(void)
{
    const uint8_t expected[6] = {0x02,0xaa,0xbb,0xcc,0xdd,0xee};
    const uint8_t wrong[6] = {0x02,0xaa,0xbb,0xcc,0xdd,0xef};
    const uint8_t zero[6] = {0};
    assert(katmed_appmaint_target_matches(expected));
    assert(!katmed_appmaint_target_matches(wrong));
    assert(!katmed_appmaint_target_matches(zero));
    assert(!katmed_appmaint_target_matches(NULL));
    return 0;
}
