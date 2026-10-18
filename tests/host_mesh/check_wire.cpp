#include "katmed_mesh_wire.h"
#include <cassert>
#include <cstring>
#include <cstdio>

int main() {
    uint8_t secret[32], group[16], wire[KATMED_MESH_WIRE_MAX];
    for (unsigned i=0;i<32;++i) secret[i]=uint8_t(1+i);
    for (unsigned i=0;i<16;++i) group[i]=uint8_t(41+i);
    KatmedMeshFrame input{};
    input.kind=MESH_CONFIRM;
    std::memcpy(input.group,group,16);
    input.id={7,9,11}; input.sender=7;
    input.parent_revision={13,17}; input.recipient=19;
    for(unsigned i=0;i<16;++i){
        input.session_nonce[i]=uint8_t(80+i);
        input.receiver_nonce[i]=uint8_t(120+i);
    }
    input.session_counter=23;
    input.payload_length=5;
    for (unsigned i=0;i<5;++i) input.payload[i]=uint8_t(i+1);
    size_t n=0;
    assert(katmed_mesh_wire_encode(&input,secret,wire,sizeof(wire),&n));
    assert(n==KATMED_MESH_WIRE_HEADER+5+KATMED_MESH_WIRE_TAG);
    KatmedMeshFrame output{};
    assert(katmed_mesh_wire_decode(wire,n,group,secret,&output));
    assert(output.kind==input.kind && output.id.sequence==11 &&
           output.parent_revision.coordinator==17 && output.recipient==19 &&
           std::memcmp(output.receiver_nonce,input.receiver_nonce,16)==0 &&
           std::memcmp(output.payload,input.payload,5)==0);
    uint8_t alien[16]; std::memset(alien,0x55,sizeof(alien));
    assert(!katmed_mesh_wire_decode(wire,n,alien,secret,&output));
    uint8_t wrong[32]; std::memset(wrong,0x77,sizeof(wrong));
    assert(!katmed_mesh_wire_decode(wire,n,group,wrong,&output));
    for (size_t i=0;i<n;++i) {
        uint8_t saved=wire[i]; wire[i]^=0x80;
        assert(!katmed_mesh_wire_decode(wire,n,group,secret,&output));
        wire[i]=saved;
    }
    for (size_t i=0;i<n;++i)
        assert(!katmed_mesh_wire_decode(wire,i,group,secret,&output));
    assert(!katmed_mesh_wire_encode(&input,secret,wire,n-1,&n));
    input.payload_length=KATMED_MESH_PAYLOAD_MAX+1;
    assert(!katmed_mesh_wire_encode(&input,secret,wire,sizeof(wire),&n));
    input.kind=MESH_STATUS;input.payload_length=904;
    std::memset(input.payload,0x42,input.payload_length);
    assert(katmed_mesh_wire_encode(&input,secret,wire,sizeof(wire),&n)&&n==1060);
    assert(katmed_mesh_wire_decode(wire,n,group,secret,&output)&&output.kind==MESH_STATUS);
    assert(!katmed_mesh_wire_decode(wire,n,group,wrong,&output));
    for(size_t i=0;i<n;++i){wire[i]^=0x80;
        assert(!katmed_mesh_wire_decode(wire,n,group,secret,&output));wire[i]^=0x80;}
    input.kind=MESH_INTERACTION;input.payload_length=228;
    assert(katmed_mesh_wire_encode(&input,secret,wire,sizeof(wire),&n)&&n==384);
    assert(katmed_mesh_wire_decode(wire,n,group,secret,&output)&&output.kind==MESH_INTERACTION);
    for(size_t i=0;i<n;++i){wire[i]^=0x80;
        assert(!katmed_mesh_wire_decode(wire,n,group,secret,&output));wire[i]^=0x80;}
    input.kind=static_cast<KatmedMeshKind>(9);
    assert(!katmed_mesh_wire_encode(&input,secret,wire,sizeof(wire),&n));
    std::puts("mesh wire auth/bounds plus STATUS7 1060-byte HMAC tamper rejection OK");
}
