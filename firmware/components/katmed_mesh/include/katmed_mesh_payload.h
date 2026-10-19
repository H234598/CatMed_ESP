#ifndef KATMED_MESH_PAYLOAD_H
#define KATMED_MESH_PAYLOAD_H

#include "katmed_mesh_wire.h"
#include "katmed_store.h"

#ifdef __cplusplus
extern "C" {
#endif

bool katmed_mesh_encode_confirm(const StorePendingConfirm *pending,
                                uint64_t age_ms_at_send,
                                uint8_t *out, size_t capacity, uint16_t *length);
bool katmed_mesh_decode_confirm(const uint8_t *bytes, size_t length,
                                StorePendingConfirm *pending,
                                uint64_t *age_ms_at_send);
bool katmed_mesh_encode_snapshot(const StateSnapshot *snapshot,
                                 uint8_t *out, size_t capacity, uint16_t *length);
bool katmed_mesh_decode_snapshot(const uint8_t *bytes, size_t length,
                                 StateSnapshot *snapshot);
bool katmed_mesh_encode_hello(const uint8_t echo[16], const uint8_t catalog[32],
                              const char *version, uint8_t pending_count,
                              uint8_t *out,
                              size_t capacity, uint16_t *length);
/* A catalog mismatch is reported separately, not a medication-auth failure. */
bool katmed_mesh_decode_hello(const uint8_t *bytes, size_t length,
                              const uint8_t expected_catalog[32],
                              const char *expected_version,
                              uint8_t echo[16], bool *catalog_mismatch,
                              uint8_t *pending_count);
bool katmed_mesh_encode_name_map(const StoreMeshNameEntry *entries, uint8_t count,
                                 uint8_t *out, size_t capacity, uint16_t *length);
bool katmed_mesh_decode_name_map(const uint8_t *bytes, size_t length,
                                 StoreMeshNameEntry *entries, uint8_t *count);

#ifdef __cplusplus
}
#endif
#endif
