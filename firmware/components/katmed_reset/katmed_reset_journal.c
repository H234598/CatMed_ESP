#include "katmed_reset_journal.h"
#include <string.h>

static bool valid_backend(KatmedResetJournalBackend b)
{
    return b.load && b.save && b.run_stage;
}

bool katmed_reset_begin(KatmedResetJournalBackend backend)
{
    if (!valid_backend(backend)) return false;
    KatmedResetJournal current={0};
    if (!backend.load(backend.context,&current)) return false;
    if (current.pending) return true;
    const KatmedResetJournal intent={true,KATMED_RESET_FILES};
    return backend.save(backend.context,&intent);
}

bool katmed_reset_resume(KatmedResetJournalBackend backend, bool *pending)
{
    if (!valid_backend(backend) || !pending) return false;
    KatmedResetJournal journal={0};
    if (!backend.load(backend.context,&journal)) return false;
    *pending=journal.pending;
    if (!journal.pending) return true;
    if (journal.stage<KATMED_RESET_FILES || journal.stage>KATMED_RESET_DONE)
        return false;
    while (journal.stage<KATMED_RESET_DONE) {
        if (!backend.run_stage(backend.context,(KatmedResetStage)journal.stage))
            return false;
        ++journal.stage;
        if (!backend.save(backend.context,&journal)) return false;
    }
    const KatmedResetJournal cleared={false,0};
    if (!backend.save(backend.context,&cleared)) return false;
    *pending=false;
    return true;
}

static void secure_zero(void *memory,size_t length)
{
    volatile uint8_t *p=(volatile uint8_t *)memory;
    while (length--) *p++=0;
}

bool katmed_reset_confirmation_prepare(KatmedResetConfirmation *state,
                                       KatmedResetRandomBytes random_bytes,
                                       void *context, uint64_t now_ms,
                                       uint64_t lifetime_ms, char *out,
                                       size_t capacity, size_t *length)
{
    static const char hex[]="0123456789abcdef";
    uint8_t random[16];
    if (!state || !random_bytes || !out || !length || capacity<33 ||
        lifetime_ms==0 || UINT64_MAX-now_ms<lifetime_ms ||
        !random_bytes(context,random,sizeof(random))) {
        if (out && capacity) out[0]=0;
        if (length) *length=0;
        return false;
    }
    for (size_t i=0;i<sizeof(random);++i) {
        state->token[2*i]=hex[random[i]>>4];
        state->token[2*i+1]=hex[random[i]&15];
    }
    state->token[32]=0; state->expires_ms=now_ms+lifetime_ms; state->valid=true;
    memcpy(out,state->token,sizeof(state->token)); *length=32;
    secure_zero(random,sizeof(random));
    return true;
}

bool katmed_reset_confirmation_consume(KatmedResetConfirmation *state,
                                       const char *token, size_t length,
                                       uint64_t now_ms)
{
    if (!state || !state->valid || !token || length!=32 || now_ms>state->expires_ms)
        return false;
    unsigned difference=0;
    for (size_t i=0;i<32;++i) difference|=(unsigned char)state->token[i]^
                                                   (unsigned char)token[i];
    if (difference) return false;
    state->valid=false;
    secure_zero(state->token,sizeof(state->token));
    state->expires_ms=0;
    return true;
}
