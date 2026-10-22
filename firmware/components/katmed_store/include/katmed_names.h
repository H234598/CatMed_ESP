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
    bool opened, poisoned, fresh, opening;
    uint8_t open_stage;
    NamesResult error;
    /* Only one file scan runs in the serial adapter at a time. Share its
     * workspace while retaining the per-poll read allowance. */
    uint16_t scan_budget;
    bool scan_limited;
    uint8_t scan_kind; /* 0 idle, 1 uniqueness, 2 lookup, 3 cached lookup, 4 digest */
    uint32_t preserve_offset;
    bool planning, target_validated;
    union {
        struct {
            uint8_t target[80];
            uint64_t heap[16],after;
            uint32_t offset,slots;
            uint16_t count,equal,assigned;
            uint8_t bank;
            bool done;
        } unique;
        struct {
            uint8_t p[80],t[80];
            uint64_t device,reservation;
            uint32_t next;
            uint16_t found,held;
            uint8_t flags,phase,stage;
        } lookup;
        struct {
            uint8_t descriptor[80];
            void *hash;
            uint64_t appended;
            uint32_t slots,offset;
            uint8_t bank,mode;
        } digest;
    } scan;
} NamesStore;
/* One poll grants at most this many file reads to resumable scans. */
void names_poll_budget(NamesStore *,uint16_t reads);
void names_cancel_scan(NamesStore *);
/* Synchronous compatibility entry for isolated host fixture construction. */
NamesResult names_open(NamesStore *,NamesBackend,uint64_t,const uint8_t group[16]);
/* Firmware entry: NAMES_BUSY continues through names_reopen on later polls. */
NamesResult names_open_bounded(NamesStore *,NamesBackend,uint64_t,const uint8_t group[16],uint16_t reads);
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
    uint8_t registry[16],anchor_p[80],anchor_t[80];
    uint64_t barrier[8],controller,epoch,reserve_device,anchor_controller,anchor_epoch;
    uint64_t hash_state[32];
    uint32_t progress,assigned;
    uint16_t reserve_index,anchor_view,anchor_reserve_index;
    uint64_t anchor_reserve_device;
    uint32_t anchor_progress;
    uint8_t anchor_progress_hash[32];
    uint8_t barrier_count,anchor_flags,anchor_phase;
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
    bool active,writing,hashes_active;
    uint8_t stage; /* 0 pages, 1 suffix, 2 hashes, 3 final checkpoint, 4 checks, 5 page checkpoint */
} NamesMerge;
NamesResult names_merge_begin(NamesStore *,NamesMerge *,uint64_t,const uint8_t source[80]);
NamesResult names_merge_resume(NamesStore *,NamesMerge *);
NamesResult names_merge_page(NamesStore *,NamesMerge *,uint32_t,const uint64_t *,size_t,
                            const uint64_t *barrier,size_t barrier_count);
NamesResult names_merge_advance(NamesStore *,NamesMerge *,const uint64_t *barrier,size_t barrier_count);
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
