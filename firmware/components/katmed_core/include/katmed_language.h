#ifndef KATMED_LANGUAGE_H
#define KATMED_LANGUAGE_H
#include <stdint.h>

/* Durable v11 values. Append only; never reorder saved language numbers. */
typedef enum {
    KATMED_LANG_EN = 0, KATMED_LANG_DE, KATMED_LANG_FR, KATMED_LANG_NL,
    KATMED_LANG_UK, KATMED_LANG_AR, KATMED_LANG_ZH_HANS, KATMED_LANG_ES,
    KATMED_LANG_JA, KATMED_LANG_BAR, KATMED_LANG_EO, KATMED_LANG_TLH_LATN
} KatmedLanguage;
#define KATMED_LANGUAGE_COUNT 12u
#define KATMED_LANGUAGE_ENGLISH KATMED_LANG_EN

#endif
