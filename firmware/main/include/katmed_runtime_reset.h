#ifndef KATMED_RUNTIME_RESET_H
#define KATMED_RUNTIME_RESET_H

#include <stdbool.h>
#include <stddef.h>

/* Adapter targets for KatmedWebResetServices. Result values match ACCEPTED,
 * STALE, FAILED in that order. The web layer must authorize before invoking. */
bool katmed_runtime_reset_prepare(void *context,char *out,size_t capacity,
                                 size_t *length);
int katmed_runtime_reset_confirm(void *context,const char *token,size_t length);

#endif
