#ifndef KATMED_MESH_WIRE_H
#define KATMED_MESH_WIRE_H

#include "katmed_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KATMED_MESH_WIRE_VERSION 1u
#define KATMED_MESH_WIRE_HEADER 124u
#define KATMED_MESH_WIRE_TAG 32u
#define KATMED_MESH_WIRE_MAX 1200u
#define KATMED_MESH_PAYLOAD_MAX (KATMED_MESH_WIRE_MAX - KATMED_MESH_WIRE_HEADER - KATMED_MESH_WIRE_TAG)

typedef enum {
    MESH_HELLO = 1,
    MESH_STATE = 2,
    MESH_CONFIRM = 3,
    MESH_SET_SHARED_SETTINGS = 4,
    MESH_TIME_SYNC = 5,
    MESH_ACK = 6,
    MESH_STATUS = 7,
    MESH_INTERACTION = 8,
    MESH_NAME_MAP = 9,
    MESH_NAME_SYNC = 10,
    MESH_NIGHT = 11
} KatmedMeshKind;

typedef struct {
    KatmedMeshKind kind;
    uint8_t group[16];
    EventId id;
    uint64_t sender;
    StateRevision parent_revision;
    uint64_t recipient; /* Zero means every group member. */
    uint8_t session_nonce[16]; /* Sender's fresh boot challenge; HMAC bound. */
    uint8_t receiver_nonce[16]; /* Echoes this receiver's fresh challenge. */
    uint64_t session_counter; /* Strictly increasing per live boot session. */
    uint16_t payload_length;
    uint8_t payload[KATMED_MESH_PAYLOAD_MAX];
} KatmedMeshFrame;

/* Secret is 32 bytes from durable USB provisioning. Uses the SDK's mbedTLS
 * HMAC-SHA256 on target and OpenSSL HMAC on the host. */
bool katmed_mesh_wire_encode(const KatmedMeshFrame *frame,
                             const uint8_t secret[32], uint8_t *bytes,
                             size_t capacity, size_t *length);
bool katmed_mesh_wire_decode(const uint8_t *bytes, size_t length,
                             const uint8_t expected_group[16],
                             const uint8_t secret[32],
                             KatmedMeshFrame *frame);

#ifdef __cplusplus
}
#endif
#endif
