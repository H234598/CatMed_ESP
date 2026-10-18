#ifndef KATMED_MESH_STATUS_H
#define KATMED_MESH_STATUS_H
#include "katmed_mesh_wire.h"
#include "katmed_mesh_session.h"
#include "katmed_store.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MESH_HISTORY_MAX 256u
#define MESH_HISTORY_LEGACY_BLOB 2080u
#define MESH_HISTORY_BLOB 2152u
#define MESH_STATUS_PAGE_IDS 100u
#define MESH_STATUS_FRESH_MS 15000u
enum { MS_SATURATED=1, MS_STORE_ERROR=2, MS_REBUILD=4,
       MS_CAPACITY=8, MS_PREHISTORY=16, MS_CURRENT=32 };
typedef struct {
    uint64_t ids[MESH_HISTORY_MAX];
    uint8_t group[16];
    uint64_t generation;
    StoreBackend backend;
    uint8_t admitted[32], processed[32], baseline[32];
    uint64_t self_initial;
    uint16_t count;
    bool saturated, dirty, error, read_failed;
} MeshHistory;
typedef struct {
    uint64_t hello_ms, status_ms, generation;
    uint16_t count;
    uint8_t mask, flags, cursor, pages;
    bool hello_valid, status_valid, baseline_complete;
} MeshStatusPeer;
typedef struct {
    MeshStatusPeer peers[KATMED_MESH_PEERS];
    uint64_t current[9];
    uint64_t now_ms, started_ms, last_tx_ms, warning_until_ms, advertised_generation;
    uint32_t observed_drops, observed_sends;
    uint8_t current_count, root_flags, recipient_cursor;
    bool current_valid, clock_valid;
} MeshStatusState;
typedef struct {
    uint64_t generation, root;
    uint16_t count, page_ids;
    uint8_t flags, page, pages, current_count;
    const uint8_t *current, *ids;
    uint8_t stride;
} MeshStatusPage;
void mesh_history_open(MeshHistory *, const uint8_t group[16], uint8_t *scratch);
bool mesh_history_add(MeshHistory *, uint64_t id);
bool mesh_history_admit(MeshHistory *, uint64_t id);
bool mesh_history_processed(MeshHistory *, uint64_t id);
bool mesh_history_unprocessed(const MeshHistory *, uint64_t *id);
void mesh_history_commit(MeshHistory *, uint8_t *scratch);
bool mesh_history_encode(const MeshHistory *, uint8_t *bytes);
StoreResult mesh_history_decode(MeshHistory *, const uint8_t *bytes, size_t length,
                                const uint8_t group[16]);
bool mesh_status_decode(const uint8_t *, size_t, bool from_root,
                        uint64_t sender, MeshStatusPage *);
uint16_t mesh_status_encode(const MeshHistory *, uint8_t flags, uint8_t page,
                            uint64_t root, const uint64_t *current,
                            uint8_t count, uint8_t *out);
bool mesh_status_fresh(uint64_t now, uint64_t then, bool valid);
bool mesh_status_merge(MeshHistory *, MeshStatusPeer *, const MeshStatusPage *, uint64_t now);
bool mesh_status_complete(const MeshStatusPeer *, uint64_t now);
#ifdef __cplusplus
}
static_assert(32+8*9+9*MESH_STATUS_PAGE_IDS<=KATMED_MESH_PAYLOAD_MAX,"history page remains within original frame budget");
#endif
#endif
