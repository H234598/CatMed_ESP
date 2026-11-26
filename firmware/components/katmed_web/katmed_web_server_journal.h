#ifndef KATMED_WEB_SERVER_JOURNAL_H
#define KATMED_WEB_SERVER_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nvs.h"

#define KATMED_SERVER_CHAIN_MAX 6144u
#define KATMED_SERVER_DNS_MAX 254u
#define KATMED_SERVER_RECORD_MAGIC UINT32_C(0x4b575332)

typedef struct {
    uint32_t magic;
    uint16_t chain_len;
    uint8_t key[32];
    char dns[KATMED_SERVER_DNS_MAX];
    char chain[KATMED_SERVER_CHAIN_MAX];
} KatmedServerRecord;

/* Missing active marker means legacy identity. An unreadable marker or record
 * is an error and must never silently select a different identity. */
typedef enum { KATMED_SERVER_LEGACY, KATMED_SERVER_ACTIVE,
               KATMED_SERVER_STORAGE_ERROR } KatmedServerLoad;
KatmedServerLoad katmed_server_journal_load(nvs_handle_t nvs, KatmedServerRecord *out,
                                            unsigned *slot);
/* Candidate must already have been verified against a separately held anchor.
 * The returned slot is not active until commit. */
bool katmed_server_journal_stage(nvs_handle_t nvs, const KatmedServerRecord *record,
                                 unsigned *slot);
/* Call only after a successful TLS session has been created by the listener
 * configured from this candidate. A failed return means the caller must
 * quarantine the uncertain marker instead of reporting success or rollback. */
bool katmed_server_journal_commit(nvs_handle_t nvs, unsigned slot);

#endif
