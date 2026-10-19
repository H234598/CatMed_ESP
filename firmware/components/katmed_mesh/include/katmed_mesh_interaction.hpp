#ifndef KATMED_MESH_INTERACTION_HPP
#define KATMED_MESH_INTERACTION_HPP
#include "katmed_interaction.h"
#include <cstring>
#include <climits>
#include <initializer_list>
namespace katmed {
/* Fixed application payload, inside the existing authenticated session envelope.
 * Counters here order ephemeral state; they are not medication EventIds. */
struct Pulse {uint64_t id=0,rev=0;uint32_t duration=0,age=0;};
struct InteractionMessage {
    uint64_t query=0,challenge=0,entry=0,contact=0,grant=0,closed=0;
    uint64_t owner=0,request=0,revision=0,impulse=0,done=0,ack=0;
    uint8_t op=0,flags=0,count=0;
    Pulse pulses[4]{};
};
inline unsigned interaction_encode(const InteractionMessage &m,uint8_t *b){
    if(m.count>4||m.op<1||m.op>2)return 0;
    b[0]=1;b[1]=m.op;b[2]=m.flags;b[3]=m.count;unsigned n=4;
    const uint64_t fields[]={m.query,m.challenge,m.entry,m.contact,m.grant,m.closed,m.owner,m.request,m.revision,m.impulse,m.done,m.ack};
    for(uint64_t v:fields)for(int j=7;j>=0;--j)b[n++]=uint8_t(v>>(8*j));
    for(unsigned i=0;i<m.count;++i){const auto &p=m.pulses[i];
        for(uint64_t v:{p.id,p.rev,uint64_t(p.duration),uint64_t(p.age)})for(int j=7;j>=0;--j)b[n++]=uint8_t(v>>(8*j));}
    return n;
}
inline bool interaction_decode(const uint8_t *b,unsigned n,InteractionMessage &m){
    if(!b||n<100||b[0]!=1||b[1]<1||b[1]>2||b[2]&~15u||b[3]>4||n!=100u+32u*b[3])return false;
    m=InteractionMessage{};m.op=b[1];m.flags=b[2];m.count=b[3];unsigned at=4;
    auto get=[&](){uint64_t v=0;for(unsigned j=0;j<8;++j)v=(v<<8)|b[at++];return v;};
    uint64_t *fields[]={&m.query,&m.challenge,&m.entry,&m.contact,&m.grant,&m.closed,&m.owner,&m.request,&m.revision,&m.impulse,&m.done,&m.ack};
    for(auto *v:fields)*v=get();
    for(unsigned i=0;i<m.count;++i){auto &p=m.pulses[i];p.id=get();p.rev=get();uint64_t d=get(),a=get();
        if(!p.id||!p.rev||d>UINT32_MAX||a>3000)return false;
        p.duration=uint32_t(d);p.age=uint32_t(a);}
    return m.query!=0;
}
class Interaction {
    struct Record {Pulse pulse{};uint64_t ended=0;};
    struct Peer {uint64_t id=0,entry=0,query=0,challenge=0,issued=0,last=0,done=0,request=0;bool reply=false;};
    Peer peers[8]{};
    Record own[4]{},received[4]{},relay[4]{};
    uint8_t own_n=0,rx_n=0,relay_n=0;
    uint64_t local=0,authority=0,generation=0,clock=0,settle=0;
    bool is_root=false,online=false,alarm=false,menu=false,held=false,dirty=false,synced=false;
    bool overflow=false,live=false,waiting=false,prompt=false,needs_ack=false,rx_baseline=true;
    uint64_t entry=0,contact=0,request=0,local_grant=0,closed=0;
    uint64_t own_impulse=0,own_revision=0,down_at=0,edge_time=0,last_up=0,prompt_at=0,prompt_serial=0;
    uint64_t qseq=0,pending=0,pending_at=0,last_tx=0,remote_challenge=0;
    uint64_t owner=0,grant=0,grant_seq=0,owner_entry=0,owner_request=0,lease_end=0;
    uint64_t root_revision=0,root_impulse=0,root_last_pulse=0,root_down_until=0;
    uint64_t own_lease_end=0,seen_grant=0,consumed=0,live_seen=0,white_end=0;
    uint64_t completion=0,completion_root=0,completion_serial=0,acknowledged=0,root_completion=0,local_done=0,root_last_up=0;
    uint8_t cursor=0;
    uint64_t loss_until=0,remote_loss_until=0;
    void lost(){loss_until=add(clock,3000);overflow=true;}
    void remote_loss(uint64_t now){remote_loss_until=add(now,3000);overflow=true;}
    static uint64_t add(uint64_t a,uint64_t b){return a>UINT64_MAX-b?UINT64_MAX:a+b;}
    static uint32_t age(uint64_t now,uint64_t then){return now>=then&&now-then<=UINT32_MAX?uint32_t(now-then):UINT32_MAX;}
    void append(Record *r,uint8_t &n,const Record &p){if(n==4){for(unsigned i=1;i<4;++i)r[i-1]=r[i];--n;lost();}r[n++]=p;}
    void close_local(){closed=local_grant>closed?local_grant:closed;local_grant=0;held=false;request=0;waiting=false;prompt=false;own_n=0;dirty=true;}
    void close_root(bool finished=true){
        if(!owner)return;
        if(finished){
            if(root_completion!=UINT64_MAX)++root_completion;
            for(auto &p:peers)if(p.id&&p.entry&&p.id!=owner)p.done=root_completion;
            if(menu&&local!=owner)local_done=root_completion;
        }
        owner=0;owner_entry=0;owner_request=0;relay_n=0;root_impulse=0;root_revision=0;root_down_until=0;root_last_pulse=0;root_last_up=0;
        if(local_grant)close_local();
    }
    void begin_local(uint64_t now){
        if(!held||!local_grant)return;
        if(own_impulse==UINT64_MAX||own_revision==UINT64_MAX){close_local();return;}
        ++own_impulse;++own_revision;down_at=now;dirty=true;
    }
    void expire(uint64_t now){
        if(now<clock)return;
        clock=now;
        overflow=now<loss_until||now<remote_loss_until;
        auto prune=[&](Record *records,uint8_t &n,uint32_t limit){
            unsigned kept=0;for(unsigned i=0;i<n;++i)if(age(now,records[i].ended)<=limit)records[kept++]=records[i];
            n=uint8_t(kept);
        };
        prune(own,own_n,1000);prune(relay,relay_n,2500);
        if(live&&now>=white_end){live=false;consumed=live_seen>consumed?live_seen:consumed;rx_baseline=true;synced=false;waiting=false;pending=0;}
        if(local_grant&&(!online||now>=own_lease_end))close_local();
        if(is_root&&owner&&now>=lease_end)close_root(false);
        if(local_grant&&!held){
            if(!prompt&&now-last_up>=10000){if(prompt_serial==UINT64_MAX){close_local();return;}++prompt_serial;prompt=true;prompt_at=now;}
            if(prompt&&now-prompt_at>=30000){close_local();if(is_root)close_root();}
        }
        for(unsigned i=0;i<rx_n;){if(now>=received[i].ended){consumed=received[i].pulse.id>consumed?received[i].pulse.id:consumed;for(unsigned j=i+1;j<rx_n;++j)received[j-1]=received[j];
                --rx_n;lost();}else ++i;}
    }
    InteractionMessage control(uint64_t now){
        InteractionMessage m;m.op=1;m.query=pending;m.challenge=remote_challenge;m.entry=menu?entry:0;m.contact=contact;m.request=request;
        m.grant=local_grant;m.closed=closed;m.flags=uint8_t((held?1:0)|(alarm?2:0)|(now<loss_until?4:0));
        m.revision=own_revision;m.impulse=held&&local_grant?own_impulse:0;m.ack=acknowledged;
        for(unsigned i=0;i<own_n;++i)if(age(now,own[i].ended)<=1000){m.pulses[m.count]=own[i].pulse;m.pulses[m.count++].age=age(now,own[i].ended);}
        return m;
    }
    void apply(uint64_t device,Peer *p,const InteractionMessage &m,uint64_t now,uint32_t transport_age){
        if(p){if(p->entry!=m.entry)p->done=0;p->entry=m.entry;p->last=now;if(m.ack>=p->done)p->done=0;}
        else if(m.ack>=local_done)local_done=0;
        if(owner==device&&(m.closed>=grant||!m.entry||(m.flags&2))){close_root(!(m.flags&2));return;}
        if(!owner&&m.entry&&m.request&&(m.flags&1)&&!(m.flags&2)&&now>=settle&&(!p||!p->done)){
            if(grant_seq==UINT64_MAX)return;
            owner=device;grant=++grant_seq;owner_entry=m.entry;owner_request=m.request;lease_end=add(now,2000);
            root_revision=0;root_impulse=0;root_last_pulse=0;relay_n=0;root_last_up=now;
        }
        if(owner!=device||m.entry!=owner_entry||m.grant!=grant)return;
        lease_end=add(now,2000);
        if(m.flags&4)remote_loss(now);
        if(m.revision>=root_revision){root_revision=m.revision;root_impulse=(m.flags&1)?m.impulse:0;
            root_down_until=root_impulse?add(now,750-transport_age):0;}
        for(unsigned i=0;i<m.count;++i){auto pulse=m.pulses[i];
            const uint64_t total=uint64_t(pulse.age)+transport_age;
            if(pulse.id<=root_last_pulse||total>1000)continue;
            if(pulse.id!=root_last_pulse+1)lost();
            root_last_pulse=pulse.id;
            // Root-local output needs no replay queue when no current peer can receive it.
            bool receiver=device!=local;
            for(const auto &peer:peers)if(peer.id&&peer.id!=device&&age(now,peer.last)<=1500)receiver=true;
            if(receiver)append(relay,relay_n,Record{pulse,now-total});
            root_last_up=now;
        }
    }
    InteractionMessage project(Peer *p,uint64_t query,uint64_t now){
        if(p&&age(now,p->last)>1500){p->entry=0;p->done=0;}
        InteractionMessage m;m.op=2;m.query=query;m.owner=owner;m.grant=owner?grant:0;m.request=owner_request;m.contact=owner_entry;
        m.entry=p?p->entry:entry;m.done=p?p->done:local_done;m.revision=root_revision;m.impulse=root_impulse;
        m.flags=uint8_t((root_impulse&&now<root_down_until?1:0)|(overflow?4:0));
        if(p){if(p->challenge!=UINT64_MAX)++p->challenge;p->issued=now;m.challenge=p->challenge;}
        for(unsigned i=0;i<relay_n;++i){const uint32_t a=age(now,relay[i].ended);if(a>2500)continue;m.pulses[m.count]=relay[i].pulse;m.pulses[m.count++].age=a;}
        return m;
    }
    void accept(const InteractionMessage &m,uint64_t now,uint32_t transit){
        synced=true;remote_challenge=m.challenge;
        if(m.grant!=seen_grant){seen_grant=m.grant;consumed=0;live_seen=0;rx_n=0;live=false;}
        if(rx_baseline){
            for(unsigned i=0;i<m.count;++i)if(m.pulses[i].id>consumed)consumed=m.pulses[i].id;
            if(m.impulse>consumed)consumed=m.impulse;
            live=false;rx_n=0;rx_baseline=false;
        }
        owner=m.owner;grant=m.grant;
        if(menu&&m.entry==entry&&m.done>acknowledged){
            if(!needs_ack){if(completion_serial==UINT64_MAX)return;completion=++completion_serial;}
            completion_root=m.done;needs_ack=true;
        }
        if(m.owner==local){
            if(m.grant>closed&&menu&&!alarm&&m.contact==entry&&(local_grant==m.grant||(held&&m.request==request))){
                own_lease_end=add(now,1500-transit);
                if(local_grant!=m.grant){local_grant=m.grant;own_impulse=0;own_revision=0;own_n=0;last_up=now;begin_local(now);}
            }
            live=false;rx_n=0;return;
        }
        if(local_grant)close_local();
        if(alarm){for(unsigned i=0;i<m.count;++i)if(m.pulses[i].id>consumed)consumed=m.pulses[i].id;live=false;rx_n=0;return;}
        if((m.flags&1)&&m.impulse>consumed){live=true;live_seen=m.impulse;white_end=add(now,750-transit);}
        else {if(live)consumed=live_seen>consumed?live_seen:consumed;live=false;}
        for(unsigned i=0;i<rx_n;){
            if(received[i].pulse.id<=(live?live_seen:consumed)){
                consumed=received[i].pulse.id>consumed?received[i].pulse.id:consumed;
                for(unsigned j=i+1;j<rx_n;++j)received[j-1]=received[j];
                --rx_n;lost();
            }else ++i;
        }
        for(unsigned i=0;i<m.count;++i){auto p=m.pulses[i];uint64_t a=uint64_t(p.age)+transit;
            if(p.id<=consumed||p.id==live_seen||a>=3000)continue;
            bool duplicate=false;for(unsigned j=0;j<rx_n;++j)if(received[j].pulse.id==p.id)duplicate=true;
            if(duplicate)continue;
            p.age=uint32_t(a);
            if(rx_n==4)consumed=received[0].pulse.id>consumed?received[0].pulse.id:consumed;
            append(received,rx_n,Record{p,add(now,3000-a)});
        }
        if(!is_root&&(m.flags&4))remote_loss(now);
    }
public:
    void bind(uint64_t self,uint64_t root_id,uint64_t epoch,bool root_role,bool connected,uint64_t now){
        if(local!=self||authority!=root_id||generation!=epoch||is_root!=root_role||online!=connected){
            bool a=alarm,opened=menu;uint64_t e=entry,qs=qseq,gs=grant_seq,ps=prompt_serial,cs=completion_serial;*this=Interaction{};qseq=qs;grant_seq=gs;prompt_serial=ps;completion_serial=cs;local=self;authority=root_id;generation=epoch;is_root=root_role;online=connected;
            alarm=a;menu=opened;entry=e;clock=now;settle=add(now,2000);dirty=true;synced=root_role&&connected;
        }expire(now);
        if(is_root&&online){auto m=control(now);apply(local,nullptr,m,now,0);auto r=project(nullptr,1,now);accept(r,now,0);}
    }
    bool input(const KatmedInteractionInput &in,uint64_t now){
        if(now<clock)return false;
        expire(now);
        uint64_t captured=now;
        if(in.kind==KATMED_INTERACTION_DOWN||in.kind==KATMED_INTERACTION_UP){
            if(in.contact_time_valid)captured=in.contact_time_ms;
            if(captured>now||captured<edge_time)return false;
            if(in.kind==KATMED_INTERACTION_UP&&local_grant&&captured<down_at)return false;
        }
        if(in.kind==KATMED_INTERACTION_ALARM){if(alarm==in.active)return true;alarm=in.active;rx_baseline=true;if(alarm){close_local();live=false;rx_n=0;if(is_root&&owner==local)close_root(false);}else {synced=false;waiting=false;pending=0;}return true;}
        if(in.kind==KATMED_INTERACTION_ENTER){if(!in.menu_entry||menu)return false;entry=in.menu_entry;menu=true;local_done=0;contact=0;needs_ack=false;completion=0;acknowledged=0;dirty=true;return true;}
        if(in.kind==KATMED_INTERACTION_CONSUME_IMPULSE){for(unsigned i=0;i<rx_n;++i)if(i==0&&in.grant_id==seen_grant&&in.impulse_id==received[i].pulse.id){consumed=in.impulse_id>consumed?in.impulse_id:consumed;for(unsigned j=i+1;j<rx_n;++j)received[j-1]=received[j];--rx_n;return true;}return false;}
        if(!menu||in.menu_entry!=entry)return false;
        if(in.kind==KATMED_INTERACTION_LEAVE){close_local();menu=false;entry=0;needs_ack=false;completion=0;dirty=true;return true;}
        if(in.kind==KATMED_INTERACTION_ACK_FINISHED){if(!needs_ack||in.target_id!=completion)return false;acknowledged=completion_root;needs_ack=false;dirty=true;return true;}
        if(in.kind==KATMED_INTERACTION_FINISH){if(!local_grant||!prompt||in.target_id!=prompt_serial)return false;close_local();return true;}
        if(in.kind==KATMED_INTERACTION_CONTINUE){if(!local_grant||!prompt||in.target_id!=prompt_serial)return false;prompt=false;last_up=now;return true;}
        if(in.kind==KATMED_INTERACTION_CANCEL){if((in.grant_id&&in.grant_id!=local_grant)||(in.contact_id&&in.contact_id!=contact))return false;close_local();return true;}
        if(in.kind==KATMED_INTERACTION_DOWN){
            if(!online||!synced||alarm||needs_ack||held||!in.contact_id||in.contact_id<=contact||(owner&&owner!=local))return false;
            edge_time=captured;contact=in.contact_id;request=contact;held=true;waiting=!local_grant;prompt=false;
            if(local_grant)begin_local(captured);
            dirty=true;return true;
        }
        if(in.kind==KATMED_INTERACTION_UP){
            if(!held||in.contact_id!=contact)return false;
            if(local_grant){if(own_revision==UINT64_MAX){close_local();return false;}++own_revision;
                Pulse p{own_impulse,own_revision,age(captured,down_at),0};append(own,own_n,Record{p,captured});}
            edge_time=captured;held=false;last_up=now;waiting=false;if(!local_grant)request=0;dirty=true;
            // A local coordinator has already reached the authority: consume its
            // completion immediately instead of retaining an outbound retry copy.
            if(is_root&&local_grant){auto m=control(now);apply(local,nullptr,m,now,0);own_n=0;}
            return true;
        }return false;
    }
    bool receive(uint64_t sender,const InteractionMessage &m,uint64_t now){
        expire(now);if(!online)return false;
        if(is_root){if(m.op!=1)return false;Peer *p=nullptr;for(auto &v:peers)if(v.id==sender)p=&v;
            if(!p)for(auto &v:peers)if(!v.id){v.id=sender;p=&v;break;}
            if(!p)return false;
            p->query=m.query;p->reply=true;
            if(m.challenge&&m.challenge==p->challenge&&age(now,p->issued)<=750)apply(sender,p,m,now,age(now,p->issued));
            return true;
        }
        if(m.op!=2||sender!=authority||!pending||m.query!=pending||age(now,pending_at)>750)return false;
        uint32_t transit=age(now,pending_at);pending=0;waiting=false;accept(m,now,transit);return true;
    }
    bool transmit(uint64_t now,uint64_t &destination,InteractionMessage &m){
        expire(now);if(!online)return false;
        if(is_root){for(unsigned j=0;j<8;++j){auto &p=peers[(cursor+j)%8];if(p.reply){p.reply=false;cursor=uint8_t((cursor+j+1)%8);destination=p.id;m=project(&p,p.query,now);return true;}}return false;}
        if(pending&&age(now,pending_at)<=750)return false;
        if(!dirty&&now-last_tx<100)return false;
        if(qseq==UINT64_MAX)return false;
        pending=++qseq;pending_at=now;last_tx=now;destination=authority;m=control(now);dirty=false;return true;
    }
    void status(KatmedInteractionStatus &out,uint64_t now){
        expire(now);out=KatmedInteractionStatus{};out.connected=online;out.synchronized=synced;out.menu_open=menu;out.alarm_active=alarm;
        out.authority_id=authority;out.authority_generation=generation;out.menu_entry=entry;out.sender_id=owner;out.grant_id=local_grant?local_grant:grant;
        out.owns_token=local_grant!=0;out.key_down=held&&local_grant;out.receive_white=live&&!alarm&&owner!=local;out.receive_until_ms=white_end;
        out.impulse_id=local_grant?own_impulse:live_seen;out.state_revision=local_grant?own_revision:root_revision;
        out.can_request=online&&synced&&menu&&!alarm&&!needs_ack&&(!owner||owner==local);out.finish_prompt=prompt;out.prompt_id=prompt_serial;
        out.sender_finished=needs_ack;out.completion_id=completion;out.overflow=overflow;
        out.mode=!online?KATMED_INTERACTION_OFFLINE:!synced?KATMED_INTERACTION_SYNCING:needs_ack?KATMED_INTERACTION_FINISHED:local_grant?KATMED_INTERACTION_OWNER:owner?KATMED_INTERACTION_REMOTE:waiting?KATMED_INTERACTION_REQUESTING:KATMED_INTERACTION_READY;
        out.impulse_count=rx_n;for(unsigned i=0;i<rx_n;++i){const auto &p=received[i];out.impulses[i]={owner,seen_grant,p.pulse.id,p.pulse.rev,p.ended,p.pulse.duration};}
    }
};
static_assert(sizeof(InteractionMessage)<=256,"bounded transient wire object");
static_assert(sizeof(Interaction)<=1536,"bounded complete transient engine");
}
#endif
