#ifndef KATMED_RUNTIME_RESET_H
#define KATMED_RUNTIME_RESET_H

#include <stdbool.h>
#include <stddef.h>

/* Display-only warning callbacks after a prepared hardware reset reboot.
 * Result values are ACCEPTED, STALE, FAILED in that order. No HTTP route
 * receives or consumes this confirmation token. */
bool katmed_runtime_reset_prepare(void *context,char *out,size_t capacity,
                                 size_t *length);
int katmed_runtime_reset_confirm(void *context,const char *token,size_t length);

#endif
