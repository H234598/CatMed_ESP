/* Existing serial Mesh adapter, existing authenticated transport/retry timing.
 * Fixed recipient snapshot: acknowledgements never stand for unseen devices. */
struct NightJob {
    KatmedNightCopyStatus status{};
    EventId id{};
    KatmedDisplaySettings settings{};
    uint64_t origin=0,root_device=0,started=0,last_send=0;
    uint8_t root_nonce[16]{};
    struct Target {uint64_t device=0;uint8_t outcome=0;uint64_t sent=0;} targets[KATMED_MESH_PEERS];
    uint8_t count=0;
};
NightJob night;
EventId night_seen[KATMED_MESH_PEERS]{};
static_assert(sizeof(Context)+sizeof(MeshHistory)+sizeof(katmed::Interaction)+sizeof(EventId)+KATMED_MESH_PEERS*(sizeof(uint64_t)+sizeof(bool))+2*sizeof(uint64_t)+sizeof(night)+sizeof(night_seen)<=16384,"Mesh including night-copy state remains within existing 16KiB static budget");
void night_cancel(){
    if(night.status.active){night.status.active=false;night.status.finished=true;night.status.membership_known=false;}
    std::memset(night_seen,0,sizeof(night_seen));
}
bool night_send(uint64_t destination,KatmedNightMessage message){
    EventId id{};if(!next_event_id(id))return false;
    auto f=frame_for(MESH_NIGHT,id,destination);
    if(!katmed_night_encode(&message,f.payload))return false;
    f.payload_length=KATMED_NIGHT_BYTES;return send_one(f);
}
KatmedNightMessage night_message(uint8_t op){
    KatmedNightMessage m{};m.op=op;m.id=night.id;m.settings=night.settings;
    if(op==4){m.expected=night.status.expected;m.acknowledged=night.status.acknowledged;
        m.rejected=night.status.rejected;m.finished=night.status.finished;}
    return m;
}
void night_report(){
    if(night.origin && night.origin!=ctx.services.store->record.device)
        (void)night_send(night.origin,night_message(4));
}
bool night_begin_root(EventId id,uint64_t origin,KatmedDisplaySettings settings,uint64_t now){
    const StoreResult saved=store_apply_night_copy(ctx.services.store,&settings,id);
    if(saved!=STORE_OK && saved!=STORE_DUPLICATE)return false;
    night=NightJob{};night.id=id;night.origin=origin;night.settings=settings;night.started=now;
    night.root_device=ctx.services.store->record.device;
    night.status.local_saved=true;night.status.active=true;night.status.membership_known=true;
    if(origin!=night.root_device){night.status.expected=1;night.status.acknowledged=1;}
    for(unsigned i=0;i<KATMED_MESH_PEERS;++i)if(fresh_hello(i) && ctx.sessions.peers[i].device!=origin){
        auto &target=night.targets[night.count++];target.device=ctx.sessions.peers[i].device;++night.status.expected;
    }
    if(!night.count){night.status.active=false;night.status.finished=true;}
    night_report();return true;
}
void night_receive(const KatmedMeshFrame &f,uint64_t now){
    KatmedNightMessage m{};if(!katmed_night_decode(f.payload,f.payload_length,&m))return;
    if(root() && m.op==1 && m.id.device==f.sender){
        if(id_equal(m.id,night.id)){night_report();return;}
        const int index=status_peer(f.sender);if(index<0)return;
        const EventId previous=night_seen[index];
        const bool old=previous.device==m.id.device &&
            (m.id.epoch<previous.epoch || (m.id.epoch==previous.epoch && m.id.sequence<=previous.sequence));
        if(old || night.status.active || !night_begin_root(m.id,f.sender,m.settings,now)){
            m.op=4;m.expected=1;m.acknowledged=0;m.rejected=1;m.finished=true;(void)night_send(f.sender,m);return;
        }
        night_seen[index]=m.id;return;
    }
    if(!root() && m.op==2 && f.sender==ctx.coordinator_device){
        const StoreResult saved=store_apply_night_copy(ctx.services.store,&m.settings,m.id);
        m.op=3;m.expected=1;m.acknowledged=(saved==STORE_OK || saved==STORE_DUPLICATE)?1:0;
        m.rejected=m.acknowledged?0:1;m.finished=true;(void)night_send(f.sender,m);return;
    }
    if(root() && m.op==3 && night.status.active && id_equal(m.id,night.id)){
        for(unsigned i=0;i<night.count;++i){auto &target=night.targets[i];
            if(target.device!=f.sender || target.outcome)continue;
            target.outcome=m.acknowledged?1:2;
            if(m.acknowledged)++night.status.acknowledged;else ++night.status.rejected;
        }
        if(night.status.acknowledged+night.status.rejected==night.status.expected){night.status.active=false;night.status.finished=true;}
        night_report();return;
    }
    if(!root() && m.op==4 && f.sender==night.root_device && id_equal(m.id,night.id)){
        night.status.membership_known=true;night.status.expected=m.expected;
        night.status.acknowledged=m.acknowledged;night.status.rejected=m.rejected;
        night.status.finished=m.finished;night.status.active=!m.finished;
    }
}
void night_tick(uint64_t now){
    if(!night.status.active)return;
    if(now<night.started || now-night.started>=kSharedTimeoutMs){
        night.status.active=false;night.status.finished=true;night_report();return;
    }
    if(!root()){
        const int index=status_peer(night.root_device);
        if(index<0 || ctx.coordinator_device!=night.root_device ||
           std::memcmp(night.root_nonce,ctx.sessions.peers[index].nonce,16)!=0){night_cancel();return;}
        if(!night.last_send || now-night.last_send>=kPendingIntervalMs){
            night.last_send=now;(void)night_send(night.root_device,night_message(1));
        }
        return;
    }
    for(unsigned i=0;i<night.count;++i){auto &target=night.targets[i];
        if(!target.outcome && (!target.sent || now-target.sent>=kPendingIntervalMs)){
            target.sent=now;(void)night_send(target.device,night_message(2));break;
        }
    }
}
