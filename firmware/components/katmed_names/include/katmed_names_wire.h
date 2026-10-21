#ifndef KATMED_NAMES_WIRE_H
#define KATMED_NAMES_WIRE_H
#include "katmed_names.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { NAMES_WIRE_HEADER=328, NAMES_WIRE_PAYLOAD=1040 };
typedef enum { NAMES_CAPS=1,NAMES_MANIFEST,NAMES_REQUEST,NAMES_PAGE,
    NAMES_PAGE_ACK,NAMES_PREPARED_ACK,NAMES_COMMIT,NAMES_COMMIT_ACK,
    NAMES_ERROR,NAMES_HEARTBEAT,NAMES_VERIFY_PROGRESS } NamesOpcode;
/* Canonical LE metadata in memory; PAGE IDs encoded separately without a
 * second frame-sized object. The outer Mesh codec authenticates these bytes. */
typedef struct {
    uint8_t opcode;
    uint16_t flags,status;
    uint64_t epoch,controller;
    uint8_t registry[16],base[80],target[80],progress_hash[32];
    uint32_t progress,start;
    uint64_t reserved_device,barrier[8];
    uint16_t reserved_index;
    uint8_t barrier_count,barrier_flags;
} NamesMessage;
bool names_wire_encode(const NamesMessage *,const uint64_t *ids,size_t count,
                       uint8_t *out,size_t capacity,size_t *length);
bool names_wire_decode(const uint8_t *,size_t,NamesMessage *);
uint64_t names_wire_slot(const uint8_t *payload,size_t index);
#ifdef __cplusplus
}
#endif
#endif
