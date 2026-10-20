#ifndef KATMED_WEB_IMPORT_H
#define KATMED_WEB_IMPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Validate the exact NVS blobs before an import writes any live key. The
 * server certificate must match its private key; the optional client
 * certificate must match its stored fingerprint. No value is logged. */
bool katmed_web_import_records_valid(const uint8_t *auth, size_t auth_len,
                                     const uint8_t *identity, size_t identity_len);

#ifdef __cplusplus
}
#endif
#endif
