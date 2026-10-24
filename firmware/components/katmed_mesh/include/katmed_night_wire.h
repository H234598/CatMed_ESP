#ifndef KATMED_NIGHT_WIRE_H
#define KATMED_NIGHT_WIRE_H
#include "katmed_display.h"
#include <string.h>
/* Runs only inside the existing authenticated, nonce/counter checked frame.
 * No brightness, wake deadline, coordinate, group secret or medical payload. */
typedef struct {
    uint8_t op; /* 1 request,2 apply,3 ACK,4 progress/report */
    EventId id;
    KatmedDisplaySettings settings;
    uint8_t expected, acknowledged, rejected;
    bool finished;
} KatmedNightMessage;
#define KATMED_NIGHT_BYTES 36u
static inline bool katmed_night_message_valid(const KatmedNightMessage *m)
{
    if(!m || m->op<1 || m->op>4 || !m->id.device || !m->id.epoch || !m->id.sequence ||
       !katmed_display_valid(m->settings) || m->expected>8 ||
       m->acknowledged+m->rejected>m->expected) return false;
    if(m->op<=2 && (m->expected || m->acknowledged || m->rejected || m->finished)) return false;
    if(m->op==3 && (m->expected!=1 || !m->finished || m->acknowledged+m->rejected!=1)) return false;
    return true;
}
static inline bool katmed_night_encode(const KatmedNightMessage *m,uint8_t out[KATMED_NIGHT_BYTES])
{
    if(!out || !katmed_night_message_valid(m))return false;
    memset(out,0,KATMED_NIGHT_BYTES);out[0]=1;out[1]=m->op;
    const uint64_t ids[3]={m->id.device,m->id.epoch,m->id.sequence};
    for(unsigned j=0;j<3;++j)for(unsigned i=0;i<8;++i)out[2+8*j+i]=(uint8_t)(ids[j]>>(56-8*i));
    out[26]=m->settings.night_enabled?1:0;
    out[27]=(uint8_t)(m->settings.night_start>>8);out[28]=(uint8_t)m->settings.night_start;
    out[29]=(uint8_t)(m->settings.night_end>>8);out[30]=(uint8_t)m->settings.night_end;
    out[31]=m->expected;out[32]=m->acknowledged;out[33]=m->rejected;out[34]=m->finished?1:0;
    return true;
}
static inline bool katmed_night_decode(const uint8_t *in,size_t length,KatmedNightMessage *m)
{
    if(!in || !m || length!=KATMED_NIGHT_BYTES || in[0]!=1 || in[26]>1 || in[34]>1 || in[35])return false;
    memset(m,0,sizeof(*m));m->op=in[1];uint64_t ids[3]={0};
    for(unsigned j=0;j<3;++j)for(unsigned i=0;i<8;++i)ids[j]=(ids[j]<<8)|in[2+8*j+i];
    m->id.device=ids[0];m->id.epoch=ids[1];m->id.sequence=ids[2];
    m->settings=katmed_display_defaults();m->settings.night_enabled=in[26]!=0;
    m->settings.night_start=(uint16_t)((uint16_t)in[27]<<8|in[28]);
    m->settings.night_end=(uint16_t)((uint16_t)in[29]<<8|in[30]);
    m->expected=in[31];m->acknowledged=in[32];m->rejected=in[33];m->finished=in[34]!=0;
    return katmed_night_message_valid(m);
}
#endif
