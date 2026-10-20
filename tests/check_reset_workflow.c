#include "c3_pins.h"
#include "katmed_reset_journal.h"

#include <assert.h>
#include <string.h>

_Static_assert(KATMED_C3_LCD_WIDTH == 240 && KATMED_C3_LCD_HEIGHT == 240,
               "C3 display profile must match GUITION evidence");
_Static_assert(KATMED_C3_LCD_SCK == 6 && KATMED_C3_LCD_MOSI == 7 &&
               KATMED_C3_LCD_DC == 2 && KATMED_C3_LCD_CS == 10 &&
               KATMED_C3_LCD_BACKLIGHT == 3,
               "C3 LCD pins must match GUITION evidence");
_Static_assert(KATMED_C3_TOUCH_SDA == 4 && KATMED_C3_TOUCH_SCL == 5 &&
               KATMED_C3_TOUCH_RESET == 1 && KATMED_C3_TOUCH_INTERRUPT == 0 &&
               KATMED_C3_TOUCH_I2C_ADDRESS_7BIT == 0x15,
               "C3 touch pins must match GUITION evidence");

typedef struct {
    bool marker;
    bool warning;
    KatmedResetConfirmation confirmation;
    uint8_t random_sequence;
    KatmedResetJournal journal;
    unsigned journal_saves;
    unsigned destructive_stages;
} ResetModel;

static bool random_bytes(void *context, uint8_t *out, size_t length)
{
    ResetModel *model = context;
    for (size_t i = 0; i < length; ++i) out[i] = ++model->random_sequence;
    return true;
}

static bool journal_load(void *context, KatmedResetJournal *out)
{
    *out = ((ResetModel *)context)->journal;
    return true;
}

static bool journal_save(void *context, const KatmedResetJournal *journal)
{
    ResetModel *model = context;
    model->journal = *journal;
    ++model->journal_saves;
    return true;
}

static bool journal_run_stage(void *context, KatmedResetStage stage)
{
    (void)stage;
    ++((ResetModel *)context)->destructive_stages;
    return true;
}

static void prepare_tile(ResetModel *model)
{
    assert(!model->warning && !model->marker);
    model->marker = true; /* Same durable one-boot marker used by the UI arm path. */
}

static void simulate_hardware_boot(ResetModel *model, bool reason_may_be_physical)
{
    const bool consumed = model->marker;
    model->marker = false;
    model->warning = katmed_reset_warning_after_boot(consumed, reason_may_be_physical);
    memset(&model->confirmation, 0, sizeof(model->confirmation));
}

static void abort_warning(ResetModel *model)
{
    model->marker = false;
    model->warning = false;
    memset(&model->confirmation, 0, sizeof(model->confirmation));
}

int main(void)
{
    ResetModel model = {0};
    KatmedResetJournalBackend backend = {
        .context = &model,
        .load = journal_load,
        .save = journal_save,
        .run_stage = journal_run_stage,
    };
    char token[33];
    size_t token_length = 0;

    /* Opening the tile only persists a marker. It never starts the journal. */
    prepare_tile(&model);
    assert(model.marker && !model.warning && !model.journal.pending);
    assert(model.journal_saves == 0 && model.destructive_stages == 0);

    /* Unknown reset cause consumes the marker without reopening the prompt. */
    simulate_hardware_boot(&model, false);
    assert(!model.marker && !model.warning);
    simulate_hardware_boot(&model, true);
    assert(!model.warning); /* A discarded preparation cannot return. */

    /* RESET wired to EN can report external or power-on. Either only warns. */
    prepare_tile(&model);
    simulate_hardware_boot(&model, true);
    assert(model.warning && !model.marker && !model.journal.pending);
    assert(katmed_reset_warning_after_boot(false, true) == false);
    assert(katmed_reset_warning_after_boot(true, false) == false);
    assert(katmed_reset_confirmation_prepare(&model.confirmation, random_bytes,
        &model, 1000, 90000, token, sizeof(token), &token_length));
    assert(token_length == 32 && !model.journal.pending &&
           model.destructive_stages == 0);

    /* Nein/Abbruch invalidates the one-time token and leaves data untouched. */
    char cancelled_token[33];
    memcpy(cancelled_token, token, sizeof(cancelled_token));
    abort_warning(&model);
    assert(!katmed_reset_confirmation_consume(&model.confirmation,
        cancelled_token, 32, 1001));
    assert(!model.journal.pending && model.journal_saves == 0 &&
           model.destructive_stages == 0);

    /* A fresh EN/REST restart opens one warning; only its bound Ja token works. */
    prepare_tile(&model);
    simulate_hardware_boot(&model, true);
    assert(model.warning && !model.marker && !model.journal.pending);
    assert(katmed_reset_confirmation_prepare(&model.confirmation, random_bytes,
        &model, 2000, 90000, token, sizeof(token), &token_length));
    assert(katmed_reset_confirmation_consume(&model.confirmation,
        token, token_length, 2001));
    assert(!katmed_reset_confirmation_consume(&model.confirmation,
        token, token_length, 2002));
    assert(model.warning && !model.journal.pending && model.destructive_stages == 0);

    /* The accepted confirmation may write reset intent; no stage runs here. */
    assert(katmed_reset_begin(backend));
    assert(model.journal.pending && model.journal_saves == 1 &&
           model.destructive_stages == 0);

    return 0;
}
