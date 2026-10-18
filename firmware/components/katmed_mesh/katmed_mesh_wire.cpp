#include "katmed_mesh_wire.h"

#include <cstring>
#ifdef KATMED_MESH_HOST
/* The host has libcrypto.so.3 but no OpenSSL development headers. Bind only
 * these two stable C ABI entry points; no cryptographic primitive is local. */
extern "C" {
struct evp_md_st;
const evp_md_st *EVP_sha256(void);
unsigned char *HMAC(const evp_md_st *, const void *, int,
                    const unsigned char *, size_t, unsigned char *, unsigned int *);
}
#else
#include "mbedtls/md.h"
#endif

namespace {
void put16(uint8_t *p, uint16_t v) { p[0]=uint8_t(v>>8); p[1]=uint8_t(v); }
void put64(uint8_t *p, uint64_t v) {
    for (unsigned i=0;i<8;++i) p[i]=uint8_t(v>>(56-8*i));
}
uint16_t get16(const uint8_t *p) { return uint16_t((uint16_t(p[0])<<8)|p[1]); }
uint64_t get64(const uint8_t *p) {
    uint64_t v=0;
    for (unsigned i=0;i<8;++i) v=(v<<8)|p[i];
    return v;
}
void put_id(uint8_t *p, EventId id) {
    put64(p,id.device); put64(p+8,id.epoch); put64(p+16,id.sequence);
}
EventId get_id(const uint8_t *p) {
    return EventId{get64(p),get64(p+8),get64(p+16)};
}
bool nonzero_id(EventId id) {
    return id.device!=0 && id.epoch!=0 && id.sequence!=0;
}
bool valid_kind(uint8_t kind) {
    return kind>=MESH_HELLO && kind<=MESH_INTERACTION;
}
bool nonzero_bytes(const uint8_t *p,size_t n){unsigned v=0;for(size_t i=0;i<n;++i)v|=p[i];return v!=0;}
bool tag(const uint8_t key[32], const uint8_t *message, size_t n,
         uint8_t out[32]) {
#ifdef KATMED_MESH_HOST
    unsigned length=0;
    return HMAC(EVP_sha256(),key,32,message,n,out,&length)!=nullptr && length==32;
#else
    const mbedtls_md_info_t *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return md!=nullptr && mbedtls_md_hmac(md,key,32,message,n,out)==0;
#endif
}
bool equal_const(const uint8_t *a, const uint8_t *b, size_t n) {
    unsigned difference=0;
    for (size_t i=0;i<n;++i) difference |= unsigned(a[i]^b[i]);
    return difference==0;
}
}

extern "C" bool katmed_mesh_wire_encode(const KatmedMeshFrame *frame,
                                          const uint8_t secret[32], uint8_t *bytes,
                                          size_t capacity, size_t *length) {
    if (!frame || !secret || !bytes || !length || !valid_kind(frame->kind) ||
        !nonzero_id(frame->id) || frame->sender!=frame->id.device ||
        !nonzero_bytes(frame->session_nonce,16) || frame->session_counter==0 ||
        (frame->kind==MESH_HELLO ? nonzero_bytes(frame->receiver_nonce,16)
                                 : !nonzero_bytes(frame->receiver_nonce,16)) ||
        frame->payload_length>KATMED_MESH_PAYLOAD_MAX) return false;
    const size_t n=KATMED_MESH_WIRE_HEADER+frame->payload_length+KATMED_MESH_WIRE_TAG;
    if (capacity<n) return false;
    std::memset(bytes,0,KATMED_MESH_WIRE_HEADER);
    std::memcpy(bytes,"KMS1",4);
    bytes[4]=KATMED_MESH_WIRE_VERSION;
    bytes[5]=uint8_t(frame->kind);
    put16(bytes+8,frame->payload_length);
    std::memcpy(bytes+10,frame->group,16);
    put_id(bytes+26,frame->id);
    put64(bytes+50,frame->sender);
    put64(bytes+58,frame->parent_revision.number);
    put64(bytes+66,frame->parent_revision.coordinator);
    put64(bytes+74,frame->recipient);
    std::memcpy(bytes+82,frame->session_nonce,16);
    std::memcpy(bytes+98,frame->receiver_nonce,16);
    put64(bytes+114,frame->session_counter);
    std::memcpy(bytes+KATMED_MESH_WIRE_HEADER,frame->payload,frame->payload_length);
    if (!tag(secret,bytes,n-KATMED_MESH_WIRE_TAG,bytes+n-KATMED_MESH_WIRE_TAG)) return false;
    *length=n;
    return true;
}

extern "C" bool katmed_mesh_wire_decode(const uint8_t *bytes, size_t length,
                                          const uint8_t expected_group[16],
                                          const uint8_t secret[32],
                                          KatmedMeshFrame *frame) {
    if (!bytes || !expected_group || !secret || !frame ||
        length<KATMED_MESH_WIRE_HEADER+KATMED_MESH_WIRE_TAG ||
        length>KATMED_MESH_WIRE_MAX || std::memcmp(bytes,"KMS1",4)!=0 ||
        bytes[4]!=KATMED_MESH_WIRE_VERSION || !valid_kind(bytes[5]) ||
        bytes[6]!=0 || bytes[7]!=0 || bytes[122]!=0 || bytes[123]!=0 ||
        !nonzero_bytes(bytes+82,16) || get64(bytes+114)==0 ||
        (bytes[5]==MESH_HELLO ? nonzero_bytes(bytes+98,16)
                              : !nonzero_bytes(bytes+98,16)) ||
        !equal_const(bytes+10,expected_group,16)) return false;
    const uint16_t payload_length=get16(bytes+8);
    if (payload_length>KATMED_MESH_PAYLOAD_MAX ||
        length!=KATMED_MESH_WIRE_HEADER+payload_length+KATMED_MESH_WIRE_TAG) return false;
    const EventId id=get_id(bytes+26);
    if (!nonzero_id(id) || id.device!=get64(bytes+50)) return false;
    uint8_t expected_tag[32];
    if (!tag(secret,bytes,length-KATMED_MESH_WIRE_TAG,expected_tag) ||
        !equal_const(expected_tag,bytes+length-KATMED_MESH_WIRE_TAG,32)) return false;
    KatmedMeshFrame candidate{};
    candidate.kind=KatmedMeshKind(bytes[5]);
    std::memcpy(candidate.group,bytes+10,16);
    candidate.id=id;
    candidate.sender=id.device;
    candidate.parent_revision={get64(bytes+58),get64(bytes+66)};
    candidate.recipient=get64(bytes+74);
    std::memcpy(candidate.session_nonce,bytes+82,16);
    std::memcpy(candidate.receiver_nonce,bytes+98,16);
    candidate.session_counter=get64(bytes+114);
    candidate.payload_length=payload_length;
    std::memcpy(candidate.payload,bytes+KATMED_MESH_WIRE_HEADER,payload_length);
    *frame=candidate;
    return true;
}
