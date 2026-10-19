#ifndef KATMED_WIFI_PROFILE_POLICY_H
#define KATMED_WIFI_PROFILE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* Zero means keep the current station profile. Priority values are 1..count. */
static inline uint8_t katmed_wifi_profile_reselect(bool disconnected,
                                                    bool explicit_reconnect,
                                                    uint8_t current_priority,
                                                    uint8_t count) {
    if (!count) return 0;
    if (explicit_reconnect) return 1;
    if (!disconnected) return 0;
    return (uint8_t)(current_priority % count + 1u);
}

#endif
