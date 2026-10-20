#include "katmed_i18n.h"
#include <stdio.h>

void katmed_i18n_format_date(char *out, size_t capacity,
                             unsigned year, unsigned month, unsigned day)
{
    if (!out || !capacity) return;
    switch (katmed_i18n_selected()) {
    case KATMED_LANG_EN:
    case KATMED_LANG_ZH_HANS:
    case KATMED_LANG_JA:
    case KATMED_LANG_TLH_LATN:
        (void)snprintf(out, capacity, "%04u-%02u-%02u", year, month, day);
        break;
    case KATMED_LANG_FR:
    case KATMED_LANG_ES:
    case KATMED_LANG_AR:
        (void)snprintf(out, capacity, "%02u/%02u/%04u", day, month, year);
        break;
    default:
        (void)snprintf(out, capacity, "%02u.%02u.%04u", day, month, year);
        break;
    }
}
