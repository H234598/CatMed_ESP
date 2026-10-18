/* The real transient engine behind the stable C adapter, linked to main.c's
 * actual queue. No always-successful adapter stub in this probe. */
#include "katmed_mesh_interaction.hpp"
#include <cassert>
using katmed::Interaction;
using katmed::InteractionMessage;
static Interaction sender, receiver;
static unsigned accepted, rejected;
static uint64_t current_time;
static void step(uint64_t now)
{
    sender.bind(1,1,1,true,true,now);
    receiver.bind(2,1,1,false,true,now);
    InteractionMessage message;
    uint64_t destination;
    if (receiver.transmit(now,destination,message)) {
        assert(destination==1);
        assert(sender.receive(2,message,now));
    }
    if (sender.transmit(now,destination,message)) {
        assert(destination==2);
        assert(receiver.receive(1,message,now));
    }
    current_time=now;
}
extern "C" void main_engine_start(void)
{
    step(1);
    step(500); /* a real current poll precedes delivery of a queued older ENTER */
}
extern "C" bool katmed_mesh_interaction_input(const KatmedInteractionInput *in,uint64_t now)
{
    sender.bind(1,1,1,true,true,now); /* same binding boundary as the actual adapter */
    bool result=sender.input(*in,now);
    if(result)++accepted;else ++rejected;
    return result;
}
extern "C" void katmed_mesh_interaction_status(KatmedInteractionStatus *out,uint64_t now)
{
    sender.bind(1,1,1,true,true,now);
    sender.status(*out,now);
}
extern "C" bool main_engine_entered(void)
{
    KatmedInteractionStatus status;
    sender.status(status,current_time);
    return status.menu_open&&status.menu_entry==7&&accepted==1&&rejected==0;
}
extern "C" void main_engine_prepare_grant(void)
{
    for(uint64_t now=525;now<=3000;now+=25)step(now);
    KatmedInteractionInput down{};
    down.kind=KATMED_INTERACTION_DOWN;down.menu_entry=7;down.contact_id=1;
    assert(katmed_mesh_interaction_input(&down,3000));
    for(uint64_t now=3025;now<=3100;now+=25)step(now);
    KatmedInteractionStatus status;sender.status(status,3100);assert(status.owns_token);
    down.kind=KATMED_INTERACTION_UP;
    assert(katmed_mesh_interaction_input(&down,3100));
    for(uint64_t now=3125;now<=3150;now+=25)step(now);
    step(3500); /* the next entire 180-ms contact was queued before this poll */
}
extern "C" bool main_engine_received_180ms(void)
{
    for(uint64_t now=3525;now<=3800;now+=25)step(now);
    KatmedInteractionStatus status;receiver.status(status,3800);
    bool found=false;
    for(unsigned n=0;n<status.impulse_count;++n)
        if(status.impulses[n].duration_ms==180)found=true;
    return found&&rejected==0;
}
extern "C" bool main_engine_closed(void)
{
    step(3525);
    KatmedInteractionStatus status;
    sender.status(status,3525);
    return !status.menu_open&&!status.key_down&&!status.owns_token;
}
