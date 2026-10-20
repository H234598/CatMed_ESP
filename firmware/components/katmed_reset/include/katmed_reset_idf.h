#ifndef KATMED_RESET_IDF_H
#define KATMED_RESET_IDF_H

#include "katmed_reset_journal.h"

#ifdef __cplusplus
extern "C" {
#endif

KatmedResetJournalBackend katmed_reset_idf_backend(void);
/* A preparation is a non-destructive, one-boot marker. It is consumed before
 * normal UI startup; it never starts the reset journal. */
bool katmed_reset_idf_prepare(void);
bool katmed_reset_idf_cancel_preparation(void);
bool katmed_reset_idf_take_preparation(bool *prepared);

#ifdef __cplusplus
}
#endif
#endif
