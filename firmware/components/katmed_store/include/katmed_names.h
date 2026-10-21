#ifndef KATMED_NAMES_H
#define KATMED_NAMES_H
#include "katmed_store.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Serial application-loop API. Backend errors poison writes until reopen.
 * Control/View are independent atomic keys; files are never authority. */
enum { NAMES_CONTROL_BYTES=480, NAMES_VIEW_BYTES=48, NAMES_HEADER_BYTES=64,
       NAMES_DESCRIPTOR_BYTES=80, NAMES_PAGE_SLOTS=89, NAMES_MAX_SLOTS=65535 };
typedef enum { NAMES_CTL, NAMES_VIEW } NamesKey;
typedef struct {
    void *context;
    StoreResult (*read)(void *,NamesKey,uint8_t *,size_t *);
    StoreResult (*commit)(void *,NamesKey,const uint8_t *,size_t);
    StoreResult (*size)(void *,unsigned,uint32_t *);
    StoreResult (*read_file)(void *,unsigned,uint32_t,uint8_t *,size_t);
    StoreResult (*write_file)(void *,unsigned,uint32_t,const uint8_t *,size_t);
    StoreResult (*truncate)(void *,unsigned,uint32_t);
    StoreResult (*sync)(void *,unsigned);
    StoreResult (*reopen)(void *);
} NamesBackend;
typedef enum { NAMES_OK, NAMES_BUSY, NAMES_UNSUPPORTED, NAMES_NEED_SYNC,
    NAMES_CONFLICT, NAMES_FULL, NAMES_IO, NAMES_COMMIT_UNKNOWN, NAMES_STALE,
    NAMES_CORRUPT, NAMES_INDEX_EXHAUSTED } NamesResult;
typedef enum { NAMES_IDLE, NAMES_LOCAL_PREPARE, NAMES_RX_APPEND,
    NAMES_RX_REPLACE, NAMES_PREPARED, NAMES_FINALIZING } NamesPhase;
typedef struct {
    /* Canonical little-endian durable control; avoids a second decoded copy. */
    uint8_t ctl[NAMES_CONTROL_BYTES];
    uint8_t expected_group[16]; /* Runtime binding survives a failed NVS read. */
    NamesBackend backend;
    uint64_t local_device;
    uint16_t visible_index;
    bool opened, poisoned, fresh;
    NamesResult error;
} NamesStore;
NamesResult names_open(NamesStore *,NamesBackend,uint64_t,const uint8_t group[16]);
NamesResult names_reopen(NamesStore *);
/* Called only for a root with no new-format object. registry must be random. */
NamesResult names_initialize(NamesStore *,const uint8_t registry[16]);
/* Exposes only durable P; pending reserve is separately returned. */
NamesResult names_lookup(NamesStore *,uint64_t,uint16_t *confirmed,uint16_t *reserved);
NamesResult names_reserve(NamesStore *,uint64_t,const uint64_t *peers,size_t);
/* Resume the durable local intent after a known reopen, never allocate anew. */
NamesResult names_resume(NamesStore *);
NamesResult names_set_barrier(NamesStore *,const uint64_t *,size_t,uint64_t controller);
/* Caller must prove current live-session PREPARED_ACK barrier, never PAGE_ACK. */
NamesResult names_publish(NamesStore *);
/* Idempotent for exact P after IDLE/restart. Persist view before ACK/visibility. */
NamesResult names_confirm(NamesStore *,const uint8_t descriptor[80]);
NamesResult names_finish(NamesStore *);
NamesResult names_read_page(NamesStore *,bool target,uint32_t start,uint64_t *,size_t *);
NamesResult names_receive_begin(NamesStore *,const uint8_t registry[16],
    const uint8_t target[80],bool append,uint64_t controller,uint64_t epoch,
    const uint64_t *,size_t,uint64_t reserve_device,uint16_t reserve_index);
NamesResult names_receive_page(NamesStore *,uint32_t start,const uint64_t *,size_t);
typedef struct {
    uint8_t target[80];
    uint64_t hash_state[32];
    uint32_t progress,assigned;
    bool active,complete;
} NamesRetarget;
NamesResult names_retarget_begin(NamesStore *,NamesRetarget *,const uint8_t target[80]);
NamesResult names_retarget_page(NamesStore *,NamesRetarget *,uint32_t,const uint64_t *,size_t);
NamesResult names_retarget_accept(NamesStore *,NamesRetarget *,uint64_t controller,
    uint64_t epoch,const uint64_t *,size_t);
void names_retarget_cancel(NamesRetarget *);
typedef struct {
    uint8_t source[80],target[80];
    uint64_t hashes[64],source_device;
    uint32_t progress,assigned,source_assigned;
    bool active,writing;
} NamesMerge;
NamesResult names_merge_begin(NamesStore *,NamesMerge *,uint64_t,const uint8_t source[80]);
NamesResult names_merge_resume(NamesStore *,NamesMerge *);
NamesResult names_merge_page(NamesStore *,NamesMerge *,uint32_t,const uint64_t *,size_t,
                            const uint64_t *barrier,size_t barrier_count);
void names_merge_cancel(NamesMerge *);
const uint8_t *names_published(const NamesStore *);
const uint8_t *names_target(const NamesStore *);
uint32_t names_cursor(const NamesStore *);
NamesPhase names_phase(const NamesStore *);
void names_visible(const NamesStore *,char out[32],bool *assigned,bool *uncertain);
/* Explicit codecs shared by persistence and wire; no C-struct serialization. */
bool names_descriptor_valid(const uint8_t descriptor[80]);
void names_descriptor_wire(uint8_t out[80],const uint8_t in[80],bool encode);
#ifdef __cplusplus
}
#endif
#endif
