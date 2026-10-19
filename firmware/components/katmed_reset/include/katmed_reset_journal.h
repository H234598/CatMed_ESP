#ifndef KATMED_RESET_JOURNAL_H
#define KATMED_RESET_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KATMED_RESET_FILES = 1,
    KATMED_RESET_KATMED_NVS,
    KATMED_RESET_WEB_NVS,
    KATMED_RESET_ACME_NVS,
    KATMED_RESET_DONE
} KatmedResetStage;

typedef struct {
    bool pending;
    uint8_t stage;
} KatmedResetJournal;

typedef struct {
    void *context;
    bool (*load)(void *context, KatmedResetJournal *out);
    bool (*save)(void *context, const KatmedResetJournal *journal);
    bool (*run_stage)(void *context, KatmedResetStage stage);
} KatmedResetJournalBackend;

/* Writes durable intent before any stage mutates data. A failed stage or
 * stage-progress commit leaves a resumable journal and never means success. */
bool katmed_reset_begin(KatmedResetJournalBackend backend);
bool katmed_reset_resume(KatmedResetJournalBackend backend, bool *pending);

typedef struct {
    char token[33];
    uint64_t expires_ms;
    bool valid;
} KatmedResetConfirmation;
typedef bool (*KatmedResetRandomBytes)(void *context, uint8_t *out, size_t length);
bool katmed_reset_confirmation_prepare(KatmedResetConfirmation *state,
                                       KatmedResetRandomBytes random_bytes,
                                       void *context, uint64_t now_ms,
                                       uint64_t lifetime_ms, char *out,
                                       size_t capacity, size_t *length);
bool katmed_reset_confirmation_consume(KatmedResetConfirmation *state,
                                       const char *token, size_t length,
                                       uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
