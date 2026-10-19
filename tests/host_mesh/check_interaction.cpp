#include "katmed_mesh_interaction.hpp"
#include <cassert>
#include <cstdio>
using katmed::Interaction;
using katmed::InteractionMessage;
struct Mesh {
 Interaction root,a,b;uint64_t now=1;bool deliver_b=true;
 Mesh(){tick(3000);}
 void step(){
  root.bind(11,11,1,true,true,now);a.bind(22,11,1,false,true,now);b.bind(33,11,1,false,true,now);
  uint64_t dest;InteractionMessage m;
  if(a.transmit(now,dest,m)){assert(dest==11);assert(root.receive(22,m,now));}
  if(b.transmit(now,dest,m)){assert(dest==11);assert(root.receive(33,m,now));}
  if(root.transmit(now,dest,m)){
   uint8_t bytes[256];unsigned n=katmed::interaction_encode(m,bytes);InteractionMessage parsed;
   assert(n<=256&&katmed::interaction_decode(bytes,n,parsed));
   if(dest==22)assert(a.receive(11,parsed,now));
   else if(deliver_b)assert(b.receive(11,parsed,now));
  }
 }
 void tick(uint64_t n){uint64_t end=now+n;while(now<end){step();now+=25;}}
 KatmedInteractionStatus status(Interaction &e){KatmedInteractionStatus s;e.status(s,now);return s;}
 bool input(Interaction &e,KatmedInteractionInputKind k,uint64_t c=0,uint64_t target=0){
  KatmedInteractionInput i{};i.kind=k;i.menu_entry=7;i.contact_id=c;i.target_id=target;return e.input(i,now);
 }
 void grant(){assert(input(a,KATMED_INTERACTION_ENTER));assert(input(a,KATMED_INTERACTION_DOWN,1));tick(300);assert(status(a).owns_token);}
};
int main(){
 {
  Interaction solo; uint64_t now=3001;
  solo.bind(11,11,1,true,true,1);solo.bind(11,11,1,true,true,now);
  auto input=[&](KatmedInteractionInputKind kind,uint64_t contact=0){
   KatmedInteractionInput in{};in.kind=kind;in.menu_entry=7;in.contact_id=contact;
   return solo.input(in,now);
  };
  assert(input(KATMED_INTERACTION_ENTER));
  for(unsigned k=1;k<=1000;++k){
   assert(input(KATMED_INTERACTION_DOWN,k));
   solo.bind(11,11,1,true,true,now);
   now+=10;assert(input(KATMED_INTERACTION_UP,k));
   solo.bind(11,11,1,true,true,now);now+=10;
   KatmedInteractionStatus s{};solo.status(s,now);
   assert(s.owns_token&&!s.key_down&&!s.overflow);
  }
  for(unsigned n=0;n<400;++n){now+=25;solo.bind(11,11,1,true,true,now);}KatmedInteractionStatus s{};solo.status(s,now);assert(s.finish_prompt);
  for(unsigned n=0;n<1200;++n){now+=25;solo.bind(11,11,1,true,true,now);}solo.status(s,now);assert(!s.owns_token&&!s.finish_prompt);
  assert(input(KATMED_INTERACTION_DOWN,1001));solo.bind(11,11,1,true,true,now);solo.status(s,now);assert(s.owns_token&&s.key_down);
  KatmedInteractionInput alarm{};alarm.kind=KATMED_INTERACTION_ALARM;alarm.active=true;
  assert(solo.input(alarm,now));solo.status(s,now);assert(!s.owns_token&&!s.key_down&&!s.receive_white);
  alarm.active=false;assert(solo.input(alarm,now));solo.bind(11,11,1,true,true,now);
  assert(input(KATMED_INTERACTION_DOWN,1002));solo.bind(11,11,1,true,true,now);
  assert(input(KATMED_INTERACTION_LEAVE));solo.bind(11,11,1,true,true,now);
  assert(!input(KATMED_INTERACTION_UP,1002));
  for(unsigned n=0;n<1800;++n){now+=25;solo.bind(11,11,1,true,true,now);}
  solo.status(s,now);assert(!s.menu_open&&!s.owns_token&&!s.key_down&&!s.finish_prompt);
  assert(input(KATMED_INTERACTION_ENTER));assert(input(KATMED_INTERACTION_DOWN,1003));
  solo.bind(11,11,1,true,true,now);assert(input(KATMED_INTERACTION_UP,1003));
  solo.bind(11,11,1,true,true,now);solo.status(s,now);assert(s.owns_token&&!s.key_down&&!s.overflow);

 }

 {
  Mesh m;m.deliver_b=false;
  assert(m.input(m.a,KATMED_INTERACTION_ENTER));assert(m.input(m.a,KATMED_INTERACTION_DOWN,1));
  while(!m.status(m.a).owns_token)m.tick(25);
  m.tick(150);m.now+=5; // grant accepted at now-25: exactly 180ms effective first hold
  assert(m.input(m.a,KATMED_INTERACTION_UP,1));m.tick(200);m.deliver_b=true;m.tick(600);
  auto s=m.status(m.b);assert(!s.menu_open&&s.impulse_count==1&&s.impulses[0].duration_ms==180);
  auto pulse=s.impulses[0];KatmedInteractionInput consume{};consume.kind=KATMED_INTERACTION_CONSUME_IMPULSE;consume.grant_id=pulse.grant_id;consume.impulse_id=pulse.impulse_id;
  assert(m.b.input(consume,m.now));m.tick(300);assert(m.status(m.b).impulse_count==0);
  assert(m.input(m.a,KATMED_INTERACTION_DOWN,2));m.tick(200);assert(m.status(m.b).receive_white);assert(m.status(m.root).receive_white);
  assert(m.input(m.a,KATMED_INTERACTION_UP,2));m.tick(200);assert(!m.status(m.b).receive_white&&m.status(m.b).impulse_count==0); // no double playback
  assert(m.input(m.a,KATMED_INTERACTION_FINISH,0,m.status(m.a).grant_id)==false);
  m.tick(10000);assert(m.status(m.a).finish_prompt);
  auto old_prompt=m.status(m.a).prompt_id;
  assert(m.input(m.a,KATMED_INTERACTION_CONTINUE,0,old_prompt));assert(!m.status(m.a).finish_prompt);
  m.tick(10000);assert(m.status(m.a).finish_prompt&&m.status(m.a).prompt_id!=old_prompt);
  assert(!m.input(m.a,KATMED_INTERACTION_FINISH,0,old_prompt));m.tick(30000);assert(!m.status(m.a).owns_token);
 }
 {
  Mesh m;assert(m.input(m.b,KATMED_INTERACTION_ENTER));m.tick(200);m.grant();assert(!m.input(m.b,KATMED_INTERACTION_DOWN,1));
  assert(m.input(m.a,KATMED_INTERACTION_UP,1));assert(m.input(m.a,KATMED_INTERACTION_LEAVE));m.tick(300);
  auto s=m.status(m.b);assert(s.sender_finished&&s.menu_open);
  assert(!m.input(m.b,KATMED_INTERACTION_ACK_FINISHED,0,s.completion_id+1));
  assert(m.input(m.b,KATMED_INTERACTION_ACK_FINISHED,0,s.completion_id));m.tick(200);assert(m.status(m.b).can_request);
  assert(m.input(m.b,KATMED_INTERACTION_LEAVE));m.tick(200);assert(!m.status(m.b).sender_finished&&m.status(m.b).synchronized);
 }
 {
  Mesh m;m.grant();auto before=m.status(m.a);KatmedInteractionInput alarm{};alarm.kind=KATMED_INTERACTION_ALARM;alarm.active=true;
  assert(m.a.input(alarm,m.now));m.tick(200);assert(!m.status(m.a).owns_token&&!m.status(m.b).receive_white);
  // Delayed grant cannot revive a locally closed token.
  InteractionMessage old;old.op=2;old.query=999;old.owner=22;old.grant=before.grant_id;old.entry=7;old.request=1;assert(!m.a.receive(11,old,m.now));
  m.b.bind(33,99,2,false,true,m.now);assert(!m.status(m.b).receive_white&&m.status(m.b).impulse_count==0);
 }
 {
  Mesh m;assert(m.input(m.a,KATMED_INTERACTION_ENTER));assert(m.input(m.a,KATMED_INTERACTION_DOWN,1));assert(m.input(m.a,KATMED_INTERACTION_UP,1));m.tick(500);assert(!m.status(m.a).owns_token&&!m.status(m.b).receive_white);
 }
 {
  Interaction rx;rx.bind(33,11,1,false,true,1);uint64_t target;InteractionMessage query;
  auto reply=[&](uint64_t now,uint64_t first,unsigned count,bool down){
   assert(rx.transmit(now,target,query));InteractionMessage m;m.op=2;m.query=query.query;m.challenge=now;m.owner=22;m.grant=1;m.flags=down?1:0;m.impulse=down?first:0;
   m.count=uint8_t(count);for(unsigned i=0;i<count;++i)m.pulses[i]={first+i,2*(first+i),180,0};
   assert(rx.receive(11,m,now));
  };
  reply(1,0,0,false);
  reply(201,1,4,false);KatmedInteractionStatus s;rx.status(s,201);assert(s.impulse_count==4);
  reply(401,5,4,false);rx.status(s,401);assert(s.overflow&&s.impulse_count==4&&s.impulses[0].impulse_id==5);
  reply(601,9,0,true);rx.status(s,601);assert(s.receive_white&&s.impulse_count==0);
  rx.status(s,1601);assert(!s.receive_white); // no lost UP can leave white forever
  reply(1401,9,0,true);rx.status(s,1601);assert(!s.receive_white); // timeout tombstone, no rewhite
  KatmedInteractionInput alarm{};alarm.kind=KATMED_INTERACTION_ALARM;alarm.active=true;
  assert(rx.input(alarm,1701));reply(1801,10,1,false);alarm.active=false;assert(rx.input(alarm,1901));
  reply(2001,11,1,false);rx.status(s,2001);assert(!s.receive_white&&s.impulse_count==0); // alarm-resync baseline
  reply(2201,12,1,false);rx.status(s,2201);assert(s.impulse_count==1&&s.impulses[0].impulse_id==12);
  InteractionMessage decoded;uint8_t bytes[256];query.op=1;query.count=4;for(unsigned i=0;i<4;++i)query.pulses[i]={i+1,i+1,180,0};
  auto n=katmed::interaction_encode(query,bytes);assert(n==228&&katmed::interaction_decode(bytes,n,decoded));
  bytes[3]=5;assert(!katmed::interaction_decode(bytes,n,decoded));bytes[3]=4;assert(!katmed::interaction_decode(bytes,n-1,decoded));
 }
 {
  Mesh m;m.grant();assert(m.input(m.a,KATMED_INTERACTION_UP,1));m.tick(400);
  for(unsigned k=2;k<=6;++k){assert(m.input(m.a,KATMED_INTERACTION_DOWN,k));m.tick(180);assert(m.input(m.a,KATMED_INTERACTION_UP,k));m.tick(2000);assert(!m.status(m.a).overflow&&!m.status(m.root).overflow);}
 }
 {
  Mesh m;m.grant();assert(m.input(m.a,KATMED_INTERACTION_UP,1));m.tick(1100);
  for(unsigned k=2;k<=6;++k){assert(m.input(m.a,KATMED_INTERACTION_DOWN,k));m.now+=10;assert(m.input(m.a,KATMED_INTERACTION_UP,k));m.now+=10;}
  assert(m.status(m.a).overflow); // five fresh untransmitted completions genuinely exceed four slots
  m.tick(10000);assert(!m.status(m.a).overflow&&!m.status(m.root).overflow);
  assert(m.input(m.a,KATMED_INTERACTION_DOWN,7));m.tick(100);
  assert(m.input(m.a,KATMED_INTERACTION_UP,7));m.tick(200);
  assert(!m.status(m.a).key_down);
 }
 {
  Mesh m;assert(m.input(m.b,KATMED_INTERACTION_ENTER));m.tick(300);m.grant();
  for(unsigned i=0;i<120;++i){m.root.bind(11,11,1,true,true,m.now);m.b.bind(33,11,1,false,true,m.now);uint64_t dest;InteractionMessage msg;
   if(m.b.transmit(m.now,dest,msg))assert(m.root.receive(33,msg,m.now));
   if(m.root.transmit(m.now,dest,msg)&&dest==33)assert(m.b.receive(11,msg,m.now));
   m.now+=25;}
  auto s=m.status(m.b);assert(!s.sender_finished&&!s.receive_white&&s.completion_id==0);
 }
 {
  Mesh m;m.grant();assert(m.input(m.a,KATMED_INTERACTION_UP,1));m.tick(100);
  uint64_t captured=m.now;m.tick(300);
  KatmedInteractionInput edge{};edge.menu_entry=7;edge.contact_id=2;edge.contact_time_valid=true;edge.kind=KATMED_INTERACTION_DOWN;edge.contact_time_ms=captured;
  assert(m.a.input(edge,m.now));edge.kind=KATMED_INTERACTION_UP;edge.contact_time_ms=captured+180;
  assert(m.a.input(edge,m.now));uint64_t dest;InteractionMessage control;assert(m.a.transmit(m.now,dest,control));
  assert(control.count&&control.pulses[control.count-1].duration==180);
  edge.kind=KATMED_INTERACTION_DOWN;edge.contact_id=3;edge.contact_time_ms=m.now;
  assert(!m.a.input(edge,m.now-1)); // capture time never rewinds delivery FSM clock
  edge.kind=KATMED_INTERACTION_DOWN;edge.contact_id=3;edge.contact_time_ms=m.now+1;assert(!m.a.input(edge,m.now));
  edge.contact_time_ms=captured-1;assert(!m.a.input(edge,m.now));
 }
 {
  Mesh m;assert(m.input(m.root,KATMED_INTERACTION_ENTER));assert(m.input(m.root,KATMED_INTERACTION_DOWN,1));m.tick(100);
  assert(m.status(m.root).owns_token);assert(m.input(m.root,KATMED_INTERACTION_UP,1));m.tick(300);
  uint64_t captured=m.now;m.tick(3500);
  KatmedInteractionInput edge{};edge.menu_entry=7;edge.contact_id=2;edge.contact_time_valid=true;
  edge.kind=KATMED_INTERACTION_DOWN;edge.contact_time_ms=captured;assert(m.root.input(edge,m.now));
  edge.kind=KATMED_INTERACTION_UP;edge.contact_time_ms=captured+180;assert(m.root.input(edge,m.now));
  m.tick(300);assert(!m.status(m.b).impulse_count); // old capture cannot gain a new display budget
 }
 std::printf("interaction: three participants, first completed impulse, all-screen/root RX, no duplicates, next contact, token/DONE, 10/30s, cancel/alarm/root-change passed; RAM=%zu wire=%zu\n",sizeof(Interaction),sizeof(InteractionMessage));
}
