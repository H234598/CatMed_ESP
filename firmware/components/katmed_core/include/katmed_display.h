#ifndef KATMED_DISPLAY_H
#define KATMED_DISPLAY_H
#include "katmed_types.h"

typedef struct {
    uint8_t brightness; /* 1..100; zero is not a saved brightness. */
    bool night_enabled;
    uint16_t night_start, night_end; /* local minutes, start inclusive/end exclusive */
} KatmedDisplaySettings;
typedef struct {
    bool local_saved, active, finished, membership_known;
    uint8_t expected, acknowledged, rejected;
} KatmedNightCopyStatus;
typedef struct {
    uint64_t wake_until_ms;
    bool awake, consume_contact, contact_down, initialized;
} KatmedDisplayPolicy;
static inline KatmedDisplaySettings katmed_display_defaults(void)
{
    KatmedDisplaySettings s={100,false,1320,420}; return s;
}
static inline bool katmed_display_valid(KatmedDisplaySettings s)
{
    return s.brightness>=1 && s.brightness<=100 && s.night_start<1440 &&
           s.night_end<1440 && s.night_start!=s.night_end;
}
static inline bool katmed_display_night(KatmedDisplaySettings s,bool time_valid,uint16_t minute)
{
    if (!katmed_display_valid(s) || !s.night_enabled || !time_valid || minute>=1440) return false;
    return s.night_start<s.night_end ? minute>=s.night_start && minute<s.night_end :
                                    minute>=s.night_start || minute<s.night_end;
}
static inline bool katmed_display_evaluate(KatmedDisplayPolicy *p,KatmedDisplaySettings s,
    bool time_valid,uint16_t minute,bool alarm,uint64_t now)
{
    if (!p) return true;
    p->initialized=true;
    if (p->wake_until_ms && now>=p->wake_until_ms) p->wake_until_ms=0;
    p->awake=alarm || !katmed_display_night(s,time_valid,minute) || p->wake_until_ms!=0;
    return p->awake;
}
/* Return true only if this contact may reach the existing UI. An alarm never
 * creates/renews the independent touch-wake window, including its ACK contact. */
static inline bool katmed_display_touch(KatmedDisplayPolicy *p,bool valid,bool down,
    bool alarm,uint64_t now)
{
    if (!p || !valid) return false;
    if(!p->initialized){p->awake=true;return true;}
    if(alarm)p->awake=true;
    const bool fresh=down && !p->contact_down;
    p->contact_down=down;
    if (fresh && !p->awake) {
        p->consume_contact=true; p->awake=true;
        p->wake_until_ms=now>UINT64_MAX-900000u ? UINT64_MAX : now+900000u;
    } else if (fresh && p->wake_until_ms && !alarm) {
        p->wake_until_ms=now>UINT64_MAX-900000u ? UINT64_MAX : now+900000u;
    }
    if (p->consume_contact) {
        if (!down) p->consume_contact=false;
        return false;
    }
    return p->awake;
}
#endif
