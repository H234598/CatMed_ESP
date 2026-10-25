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
#if KATMED_APPSTORE_MAINTENANCE
typedef enum {
    KATMED_APPMAINT_HOLD,
    KATMED_APPMAINT_ABSENT,
    KATMED_APPMAINT_PENDING,
    KATMED_APPMAINT_COMPLETED
} KatmedAppmaintStatus;
KatmedAppmaintStatus katmed_reset_idf_maintenance_status(uint8_t *stage);
bool katmed_reset_idf_maintenance_preflight(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
