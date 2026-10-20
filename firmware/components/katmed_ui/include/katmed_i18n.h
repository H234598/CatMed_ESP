#ifndef KATMED_I18N_H
#define KATMED_I18N_H
#include <stdbool.h>
#include <stddef.h>
#include "katmed_language.h"

#ifdef __cplusplus
extern "C" {
#endif
bool katmed_i18n_available(uint8_t language);
void katmed_i18n_select(uint8_t language);
uint8_t katmed_i18n_selected(void);
/* Source keys are product-authored German copy; only literal UI copy is passed.
 * User alarm text, device names, addresses and SSIDs bypass this function. */
const char *katmed_i18n_text(const char *source);
void katmed_i18n_format_date(char *out, size_t capacity,
                             unsigned year, unsigned month, unsigned day);
#ifdef __cplusplus
}
#endif
#endif
