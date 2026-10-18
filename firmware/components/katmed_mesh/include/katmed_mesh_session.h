#ifndef KATMED_MESH_SESSION_H
#define KATMED_MESH_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KATMED_MESH_PEERS 8u
typedef struct {
    uint64_t device;
    uint8_t nonce[16];
    uint64_t highest_counter;
    uint8_t advertised_pending;
    bool live;
} KatmedMeshPeer;
typedef struct {
    uint8_t local_nonce[16];
    KatmedMeshPeer peers[KATMED_MESH_PEERS];
} KatmedMeshSessions;

bool katmed_mesh_sessions_init(KatmedMeshSessions *sessions,
                               const uint8_t local_nonce[16]);
/* HELLO is already HMAC/group authenticated. The echo proves that the peer
 * received our fresh challenge. A mere old signed HELLO grants no session. */
bool katmed_mesh_sessions_accept_hello(KatmedMeshSessions *sessions,
                                        uint64_t device,
                                        const uint8_t peer_nonce[16],
                                        const uint8_t echo[16]);
bool katmed_mesh_sessions_nonce_changed(const KatmedMeshSessions *sessions,
                                         uint64_t device,
                                         const uint8_t peer_nonce[16]);
bool katmed_mesh_sessions_check(KatmedMeshSessions *sessions, uint64_t device,
                                 const uint8_t nonce[16], uint64_t counter,
                                 bool advance);
void katmed_mesh_sessions_forget(KatmedMeshSessions *sessions, uint64_t device);

#ifdef __cplusplus
}
#endif
#endif
