#ifndef KATMED_WEB_SERVER_VALIDATION_H
#define KATMED_WEB_SERVER_VALIDATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The caller must obtain utc_seconds from a trusted, fresh time source. PEM
 * inputs include their terminating NUL in the supplied lengths. The anchor is
 * provisioned separately, never taken from the submitted chain. */
bool katmed_web_server_identity_validate(const uint8_t *key, size_t key_len,
                                         const char *chain, size_t chain_len,
                                         const char *anchor, size_t anchor_len,
                                         const char *dns, int64_t utc_seconds);

#endif
