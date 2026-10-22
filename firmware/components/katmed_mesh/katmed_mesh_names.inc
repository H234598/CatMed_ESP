/* Included once in the serial IDF adapter namespace. Authentication, replay
 * counters and current-coordinator filtering have already run at entry. */
struct NamePeer {
    uint8_t base[80]{};
    uint64_t device=0,last_auth=0,last_send=0;
    uint32_t progress=0;
    uint8_t protocol=0,prepared=0,committed=0,retries=0,discovered=0;
};
struct NamePageScratch {
    explicit NamePageScratch(const KatmedMeshFrame &f){
        const size_t count=(f.payload_length-328)/8;
        for(size_t i=0;i<count;++i)ctx.receive.name_page[i]=names_wire_slot(f.payload,i);
    }
    ~NamePageScratch(){ctx.receive.raw={};}
    const uint64_t *ids()const{return ctx.receive.name_page;}
};
static_assert(sizeof(ctx.receive.name_page)<=sizeof(ReceiveScratch),"shared page scratch bound");
NamesStore names;
std::atomic<uint32_t> names_publication{0};
void names_publish_status(){
    bool assigned=false,uncertain=true;names_visible(&names,nullptr,&assigned,&uncertain);
    const uint32_t projection=(assigned?names.visible_index:0)|(uncertain?(1u<<16):0)|(names.opened?(1u<<17):0);
    names_publication.store(projection,std::memory_order_release);
}
struct NamesStatusPublication {
    ~NamesStatusPublication(){
        const uint32_t before=names_publication.load(std::memory_order_relaxed);
        names_publish_status();
        // Bounded open may reveal a previously confirmed View on a later
        // poll, including while a separate candidate remains erroneous.
        // Publish the same name to netif then; avoid a setter on every poll.
        if((before^names_publication.load(std::memory_order_relaxed))&0xffffu)apply_hostname();
    }
};
union NamesJobs {NamesMerge merge;NamesRetarget verify;} names_jobs{};
uint8_t names_job_kind=0;
#define names_verify names_jobs.verify
#define names_merge names_jobs.merge
uint64_t names_merge_last=0;
uint64_t names_verify_controller=0,names_verify_epoch=0;
uint8_t names_probe_cursor=0;
NamePeer name_peers[KATMED_MESH_PEERS]{};
bool names_bound=false,names_round_bound=false;
uint64_t names_started=0;
static_assert(sizeof(names)+sizeof(name_peers)+sizeof(names_jobs)+64<=2560,
              "names adapter static budget");
uint64_t name_le(const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[i])<<(8*i);return v;}
NamePeer *name_peer(uint64_t id){for(auto &p:name_peers)if(p.device==id)return &p;return nullptr;}
void names_reset_rights(){
    if(!names_bound)return;
    if(names_job_kind==1)names_merge_cancel(&names_merge);
    if(names_job_kind==2)names_retarget_cancel(&names_verify);
    std::memset(&names_jobs,0,sizeof(names_jobs));names_job_kind=0;names_merge_last=0;
    names_probe_cursor=0;
    names_cancel_scan(&names);
    names_verify_controller=names_verify_epoch=0;
    for(auto &p:name_peers)p=NamePeer{};
    names_round_bound=false;names_started=ctx.status.now_ms;
}
void names_legacy_map(uint64_t device){if(names_bound&&!root()&&device==ctx.coordinator_device)names.error=NAMES_UNSUPPORTED;}
void names_activity(uint64_t device,uint64_t now){
    if(!names_bound)return;
    NamePeer *p=name_peer(device);
    if(p)p->last_auth=now;
}
bool names_send(uint64_t device,const NamesMessage &m,const uint64_t *ids=nullptr,size_t count=0){
    EventId id{};if(!next_event_id(id))return false;
    // Raw bytes have already been decoded into poll's distinct frame. Reuse
    // that bounded receive slot serially; never add a second frame to stack/BSS.
    auto *f=new(&ctx.receive.name_frame)KatmedMeshFrame(frame_for(MESH_NAME_SYNC,id,device));
    size_t n=0;bool ok=names_wire_encode(&m,ids,count,f->payload,sizeof(f->payload),&n);
    if(ok){f->payload_length=uint16_t(n);ok=send_one(*f);}
    f->~KatmedMeshFrame();ctx.receive.raw={};
    return ok;
}
NamesMessage names_message(uint8_t op,const NamePeer *peer){
    NamesMessage m{};m.opcode=op;
    const uint8_t *t=names_target(&names);
    if(!t)t=names_published(&names);
    if(!t)return m;
    m.controller=name_le(names.ctl+336,8);m.epoch=name_le(names.ctl+88,8);
    if(!m.epoch)m.epoch=1;
    std::memcpy(m.registry,names.ctl+24,16);std::memcpy(m.target,t,80);
    if(peer)std::memcpy(m.base,peer->base,80);
    m.flags=2;
    if(names_phase(&names)==NAMES_IDLE||names_phase(&names)==NAMES_FINALIZING)m.flags|=4;
    /* A receiver may select APPEND only after exact base validation. */
    m.reserved_device=name_le(names.ctl+96,8);m.reserved_index=uint16_t(name_le(names.ctl+104,2));
    if(m.reserved_device)m.flags|=16;
    m.barrier_count=names.ctl[50];m.barrier_flags=1;
    for(unsigned i=0;i<m.barrier_count;++i)m.barrier[i]=name_le(names.ctl+272+8*i,8);
    return m;
}
bool names_verify_binding(const NamesMessage &m){
    if(names_verify.controller!=m.controller||names_verify.epoch!=m.epoch||
       names_verify.barrier_count!=m.barrier_count||
       names_verify.reserve_device!=m.reserved_device||
       names_verify.reserve_index!=m.reserved_index||
       std::memcmp(names_verify.registry,m.registry,16))return false;
    for(unsigned i=0;i<m.barrier_count;++i)if(names_verify.barrier[i]!=m.barrier[i])return false;
    return true;
}
void names_bind_verify(const NamesMessage &m){
    names_verify.controller=m.controller;names_verify.epoch=m.epoch;
    names_verify.barrier_count=m.barrier_count;
    names_verify.reserve_device=m.reserved_device;names_verify.reserve_index=m.reserved_index;
    std::memcpy(names_verify.registry,m.registry,16);
    for(unsigned i=0;i<m.barrier_count;++i)names_verify.barrier[i]=m.barrier[i];
}
void names_hello(uint64_t device,uint64_t now){
    if(!names_bound)return;
    NamePeer *p=name_peer(device);
    if(!p)for(auto &slot:name_peers)if(!slot.device){p=&slot;slot.device=device;break;}
    if(!p)return;
    p->last_auth=now;
    NamesMessage caps{};caps.opcode=NAMES_CAPS;
    (void)names_send(device,caps);
}
bool names_barrier_ack(bool final){
    for(unsigned i=0;i<names.ctl[50];++i){NamePeer *p=name_peer(name_le(names.ctl+272+8*i,8));if(!p||p->protocol!=2||!(final?p->committed:p->prepared))return false;}
    return true;
}
bool names_expire(uint64_t now){
    if(!names_bound||!root()||names.poisoned)return false;
    bool expired=false;
    uint64_t members[8];size_t n=0;
    for(unsigned i=0;i<names.ctl[50];++i){
        const uint64_t id=name_le(names.ctl+272+8*i,8);
        NamePeer *p=name_peer(id);
        const uint64_t last=p?p->last_auth:names_started;
        if(now>=last&&now-last>=30000){expired=true;if(p)p->prepared=p->committed=p->protocol=0;}
        else members[n++]=id;
    }
    for(auto &p:name_peers)if(p.device&&now>=p.last_auth&&now-p.last_auth>=30000){expired=true;p.prepared=p.committed=p.protocol=0;}
    if(!expired)return false;
    /* Never forget/rekey on an uncertain E commit. In particular, never let
     * a smaller RAM mask falsely satisfy the old durable round. */
    if(names_set_barrier(&names,members,n,names.local_device)!=NAMES_OK)return false;
    return rekey();
}
__attribute__((noinline)) void names_tick(uint64_t now){
    if(!names_bound||!names.opened)return;
    if(names.opening){
        if(names_reopen(&names)!=NAMES_OK)return;
        if(!rekey()){katmed_mesh_stop();return;}send_hello();return;
    }
    if(names.poisoned){
        if(names.error!=NAMES_COMMIT_UNKNOWN||names_reopen(&names)!=NAMES_OK)return;
        if(!rekey()){katmed_mesh_stop();return;}send_hello();return;
    }
    if(!root()){
        if((names_phase(&names)==NAMES_RX_APPEND||names_phase(&names)==NAMES_RX_REPLACE)&&
           names_target(&names)&&(name_le(names.ctl+52,4)==name_le(names_target(&names)+64,4)||
             (names.scan_kind==4&&!names.scan.digest.mode&&names.scan.digest.slots>name_le(names.ctl+52,4))))
            (void)names_resume(&names);
        NamePeer *p=name_peer(ctx.coordinator_device);
        if(!p||p->protocol!=2)return;
        const NamesPhase phase=names_phase(&names);
        const uint64_t interval=(phase==NAMES_IDLE)?10000:1000;
        if(now-p->last_send<interval)return;
        if(phase==NAMES_IDLE){NamesMessage h{};h.opcode=NAMES_HEARTBEAT;(void)names_send(p->device,h);}
        const uint8_t *target=names_target(&names);if(!target)target=names_published(&names);
        if(target){
            NamesMessage request=names_message(phase==NAMES_PREPARED?NAMES_PREPARED_ACK:NAMES_REQUEST,p);
            if(names_published(&names))std::memcpy(request.base,names_published(&names),80);
            request.progress=phase==NAMES_IDLE||phase==NAMES_PREPARED?uint32_t(name_le(target+64,4)):uint32_t(name_le(names.ctl+52,4));
            std::memcpy(request.progress_hash,phase==NAMES_IDLE||phase==NAMES_PREPARED?target+32:names.ctl+56,32);
            (void)names_send(p->device,request);
        }
        p->last_send=now;return;
    }
    if(names.error==NAMES_CONFLICT||(names.poisoned&&names.error==NAMES_CORRUPT))return;
    if(names.fresh){uint8_t registry[16];esp_fill_random(registry,16);if(names_initialize(&names,registry)!=NAMES_OK)return;}
    if(names_phase(&names)==NAMES_LOCAL_PREPARE&&names.ctl[49]==4){
        if(names_job_kind!=1){names_jobs.merge=NamesMerge{};names_job_kind=1;}
        if(!names_merge.active&&names_merge_resume(&names,&names_merge)!=NAMES_OK)return;
    }else if(names_phase(&names)==NAMES_LOCAL_PREPARE){if(names_resume(&names)!=NAMES_OK)return;}
    if(names_job_kind==1&&names_merge.active){
        if(names_merge.stage!=0){
            uint64_t members[8];size_t count=0;
            for(auto &p:name_peers)if(p.device&&p.protocol==2)members[count++]=p.device;
            const NamesResult step=names_merge_advance(&names,&names_merge,members,count);
            if(step==NAMES_BUSY)return;
            if(step!=NAMES_OK){names.error=step;names_merge_cancel(&names_merge);return;}
        }
        if(!names_merge.active)return;
        if(!names_merge_last||now-names_merge_last>=1000){
            NamePeer *source=name_peer(names_merge.source_device);
            if(source&&source->protocol==2){
                NamesMessage request=names_message(NAMES_REQUEST,source);
                request.controller=names.local_device;request.epoch=name_le(names.ctl+88,8);if(!request.epoch)request.epoch=1;
                std::memcpy(request.target,names_merge.source,80);request.progress=names_merge.progress;
                (void)names_send(source->device,request);names_merge_last=now;
            }
        }
        return;
    }
    if((names_phase(&names)==NAMES_RX_APPEND||names_phase(&names)==NAMES_RX_REPLACE)&&names_target(&names)&&
       (name_le(names.ctl+52,4)==name_le(names_target(&names)+64,4)||
        (names.scan_kind==4&&!names.scan.digest.mode&&names.scan.digest.slots>name_le(names.ctl+52,4)))){
        if(names_resume(&names)!=NAMES_OK)return;
    }
    if(!names_round_bound){
        uint64_t members[8];size_t n=0;
        for(size_t i=0;i<names.ctl[50];++i){uint64_t id=name_le(names.ctl+272+8*i,8);if(id!=names.local_device)members[n++]=id;}
        if(names_set_barrier(&names,members,n,names.local_device)!=NAMES_OK)return;
        names_round_bound=true;
    }
    if(names_phase(&names)==NAMES_PREPARED&&names_barrier_ack(false)){
        if(names_publish(&names)!=NAMES_OK)return;
        const NamesResult r=names_confirm(&names,names_published(&names));
        if(r!=NAMES_OK&&r!=NAMES_NEED_SYNC)return;
        apply_hostname();
    }
    if(names_phase(&names)==NAMES_FINALIZING){
        const NamesResult r=names_confirm(&names,names_published(&names));
        if(r!=NAMES_OK&&r!=NAMES_NEED_SYNC)return;
        apply_hostname();
    }
    if(names_phase(&names)==NAMES_FINALIZING&&names_barrier_ack(true)){
        if(names_finish(&names)!=NAMES_OK)return;
        for(auto &p:name_peers){p.prepared=p.committed=p.retries=0;p.progress=0;p.last_send=0;}
    }
    if(names_phase(&names)==NAMES_IDLE){
        uint64_t members[8];size_t count=0;
        for(auto &p:name_peers)if(p.device&&p.protocol==2)members[count++]=p.device;
        uint16_t own=names.visible_index,pending=0;
        if(!own&&names_lookup(&names,names.local_device,&own,&pending)!=NAMES_OK)return;
        uint64_t next=own?0:names.local_device;
        if(!next)for(unsigned attempt=0;attempt<KATMED_MESH_PEERS;++attempt){
            const unsigned at=(names_probe_cursor+attempt)%KATMED_MESH_PEERS;
            auto &p=name_peers[at];
            if(!p.device||p.protocol!=2||!p.discovered)continue;
            uint16_t assigned=0,reserved=0;
            if(names_lookup(&names,p.device,&assigned,&reserved)!=NAMES_OK){names_probe_cursor=uint8_t(at);return;}
            names_probe_cursor=uint8_t((at+1)%KATMED_MESH_PEERS);
            if(!assigned){next=p.device;break;}
        }
        if(next){
            if(names_reserve(&names,next,members,count)!=NAMES_OK)return;
            for(auto &p:name_peers){p.prepared=p.committed=p.retries=0;p.progress=0;p.last_send=0;}
        }
    }
    for(auto &p:name_peers){
        if(!p.device||p.protocol!=2||p.retries>=5)continue;
        if(p.last_send&&now-p.last_send<1000)continue;
        uint8_t op=names_phase(&names)==NAMES_FINALIZING?NAMES_COMMIT:NAMES_MANIFEST;
        if(names_phase(&names)==NAMES_RX_APPEND||names_phase(&names)==NAMES_RX_REPLACE)op=NAMES_REQUEST;
        if(op==NAMES_COMMIT&&p.committed)continue;
        if(op==NAMES_MANIFEST&&p.prepared)continue;
        if(op==NAMES_MANIFEST&&p.progress){
            uint64_t ids[89];size_t count=89;const uint32_t start=p.progress-1;
            if(names_read_page(&names,names_target(&names)!=nullptr,start,ids,&count)==NAMES_OK&&count){NamesMessage page=names_message(NAMES_PAGE,&p);page.start=start;(void)names_send(p.device,page,ids,count);p.last_send=now;++p.retries;continue;}
        }
        NamesMessage m=names_message(op,&p);
        if(op==NAMES_REQUEST){m.progress=uint32_t(name_le(names.ctl+52,4));if(names_published(&names))std::memcpy(m.base,names_published(&names),80);}
        if(!names_descriptor_valid(m.target))continue;
        (void)names_send(p.device,m);p.last_send=now;++p.retries;
    }
}
__attribute__((noinline)) void names_frame(const KatmedMeshFrame &f,uint64_t now){
    if(!names_bound||names.opening)return;
    if(names.poisoned){if(names_reopen(&names)!=NAMES_OK)return;if(!rekey()){katmed_mesh_stop();return;}send_hello();return;}
    NamesMessage m{};
    if(!names_wire_decode(f.payload,f.payload_length,&m)){
        if(!root()&&f.sender==ctx.coordinator_device&&f.payload_length==328&&f.payload[1]==NAMES_CAPS)names.error=NAMES_UNSUPPORTED;
        return;
    }
    NamePeer *peer=name_peer(f.sender);
    if(!peer)return;
    peer->last_auth=now;
    if(m.opcode==NAMES_CAPS){if(names.error==NAMES_UNSUPPORTED)names.error=NAMES_OK;if(peer->protocol!=2){peer->retries=0;peer->last_send=0;}peer->protocol=2;return;}
    if(m.opcode==NAMES_HEARTBEAT)return;
    if(peer->protocol!=2)return;
    if(root()){
        if(m.opcode==NAMES_MANIFEST&&m.controller==names.local_device&&
           !std::memcmp(m.registry,names.ctl+24,16)&&!names_target(&names)&&
           names_published(&names)&&std::memcmp(m.target,names_published(&names),80)){
            if(names_job_kind==1&&names_merge.active){
                if(f.sender==names_merge.source_device&&!std::memcmp(m.target,names_merge.source,80)){peer->retries=0;names_merge_last=0;return;}
                NamesMessage error=m;error.opcode=NAMES_ERROR;error.status=NAMES_BUSY;(void)names_send(f.sender,error);return;
            }
            if(names_job_kind!=1){names_jobs.merge=NamesMerge{};names_job_kind=1;}
            if(names_merge_begin(&names,&names_merge,f.sender,m.target)==NAMES_OK){peer->discovered=1;names_merge_last=0;for(auto &p:name_peers){p.prepared=p.committed=p.retries=0;p.progress=0;p.last_send=0;}}
            return;
        }
        if(m.opcode==NAMES_PAGE&&names_job_kind==1&&names_merge.active&&f.sender==names_merge.source_device&&
           m.controller==names.local_device&&!std::memcmp(m.registry,names.ctl+24,16)&&
           !std::memcmp(m.target,names_merge.source,80)&&m.epoch==name_le(names.ctl+88,8)){
            // A delayed/duplicate source response is not a new merge offer.
            // Keep exact S and both pass cursors; request current progress.
            if(m.start!=names_merge.progress){names_merge_last=0;return;}
            NamePageScratch page(f);uint64_t members[8];size_t count=0,n=(f.payload_length-328)/8;
            for(auto &p:name_peers)if(p.device&&p.protocol==2)members[count++]=p.device;
            NamesResult r=names_merge_page(&names,&names_merge,m.start,page.ids(),n,members,count);
            names_merge_last=0;
            if(r!=NAMES_OK&&r!=NAMES_BUSY){names.error=r;names_merge_cancel(&names_merge);NamesMessage error=m;error.opcode=NAMES_ERROR;error.status=uint16_t(r);error.start=0;(void)names_send(f.sender,error);}
            return;
        }
        const uint8_t *t=names_target(&names);if(!t)t=names_published(&names);
        if(m.opcode==NAMES_ERROR&&m.controller==names.local_device&&m.epoch==name_le(names.ctl+88,8)&&!std::memcmp(m.registry,names.ctl+24,16)&&
           ((t&&!std::memcmp(m.target,t,80))||(names_job_kind==1&&names_merge.active&&f.sender==names_merge.source_device&&!std::memcmp(m.target,names_merge.source,80)))){names.error=NamesResult(m.status);return;}
        if(t&&m.opcode==NAMES_REQUEST&&(m.controller!=names.local_device||m.epoch!=name_le(names.ctl+88,8)||std::memcmp(m.target,t,80))&&!std::memcmp(m.registry,names.ctl+24,16)){
            NamesMessage current=names_message(NAMES_MANIFEST,peer);peer->retries=0;peer->last_send=now;(void)names_send(f.sender,current);return;
        }
        if(!t||m.controller!=names.local_device||m.epoch!=name_le(names.ctl+88,8)||
           std::memcmp(m.registry,names.ctl+24,16)||std::memcmp(m.target,t,80))return;
        if(m.opcode==NAMES_PAGE&&(names_phase(&names)==NAMES_RX_APPEND||names_phase(&names)==NAMES_RX_REPLACE)){
            NamePageScratch page(f);const size_t n=(f.payload_length-328)/8;
            const NamesResult r=names_receive_page(&names,m.start,page.ids(),n);
            peer->retries=0;peer->last_send=0;if(r!=NAMES_OK&&r!=NAMES_BUSY)names.error=r;
            return;
        }
        if(m.opcode==NAMES_REQUEST){
            peer->discovered=1;
            if(names_phase(&names)==NAMES_IDLE){
                uint16_t confirmed=0,reserved=0;
                if(names_lookup(&names,f.sender,&confirmed,&reserved)!=NAMES_OK)return;
                if(!confirmed)return; // Fresh JOIN_PENDING: reserve through the normal barrier.
            }
            std::memcpy(peer->base,m.base,80);peer->progress=m.progress+1;peer->retries=0;peer->last_send=now;
            uint64_t ids[89];size_t n=89;
            if(names_read_page(&names,names_target(&names)!=nullptr,m.progress,ids,&n)!=NAMES_OK)return;
            if(!n){NamesMessage commit=names_message(names_phase(&names)==NAMES_FINALIZING||names_phase(&names)==NAMES_IDLE?NAMES_COMMIT:NAMES_MANIFEST,peer);(void)names_send(f.sender,commit);return;}
            NamesMessage page=names_message(NAMES_PAGE,peer);page.start=m.progress;if(m.flags&8)page.flags|=8;
            (void)names_send(f.sender,page,ids,n);return;
        }
        if(std::memcmp(m.base,peer->base,80)||m.barrier_flags!=1||m.barrier_count!=names.ctl[50]||
           m.reserved_device!=name_le(names.ctl+96,8)||m.reserved_index!=name_le(names.ctl+104,2))return;
        for(unsigned i=0;i<m.barrier_count;++i)if(m.barrier[i]!=name_le(names.ctl+272+8*i,8))return;
        if((m.opcode==NAMES_PREPARED_ACK||m.opcode==NAMES_COMMIT_ACK)&&
           (m.progress!=name_le(t+64,4)||std::memcmp(m.progress_hash,t+32,32)))return;
        if(m.opcode==NAMES_PREPARED_ACK){peer->progress=0;peer->prepared=1;peer->retries=0;peer->last_send=0;if(names_phase(&names)==NAMES_IDLE){NamesMessage commit=names_message(NAMES_COMMIT,peer);(void)names_send(f.sender,commit);}}
        if(m.opcode==NAMES_COMMIT_ACK&&(names_phase(&names)==NAMES_FINALIZING||names_phase(&names)==NAMES_IDLE)){peer->committed=1;peer->retries=0;}
        return;
    }
    if(f.sender!=ctx.coordinator_device||m.controller!=f.sender)return;
    if(m.opcode==NAMES_ERROR){names.error=NamesResult(m.status);return;}
    if(m.opcode==NAMES_REQUEST){
        const uint8_t *p=names_published(&names);
        bool target=false;
        if((!p||std::memcmp(p,m.target,80))&&names_target(&names)&&(names.ctl[51]&16)&&!std::memcmp(names_target(&names),m.target,80)){p=names_target(&names);target=true;}
        if(!p||std::memcmp(p,m.target,80)||std::memcmp(m.registry,names.ctl+24,16))return;
        uint64_t ids[89];size_t n=89;
        if(names_read_page(&names,target,m.progress,ids,&n)!=NAMES_OK||!n)return;
        NamesMessage page=m;page.opcode=NAMES_PAGE;page.start=m.progress;
        (void)names_send(f.sender,page,ids,n);return;
    }
    if(m.opcode==NAMES_MANIFEST){
        const uint8_t *p=names_published(&names);
        if(p)std::memcpy(peer->base,p,80);else std::memset(peer->base,0,80);
        const uint8_t *required=names_target(&names);
        if((m.flags&4)&&required&&(names.ctl[51]&16)&&std::memcmp(required,m.target,80)&&name_le(required+68,4)>=name_le(m.target+68,4)){
            NamesMessage offer=m;offer.opcode=NAMES_MANIFEST;std::memcpy(offer.target,required,80);offer.flags=2;offer.reserved_device=0;offer.reserved_index=0;(void)names_send(f.sender,offer);return;
        }
        if((m.flags&4)&&p&&std::memcmp(p,m.target,80)&&name_le(p+68,4)>=name_le(m.target+68,4)&&!names_target(&names)){
            NamesMessage offer=m;offer.opcode=NAMES_MANIFEST;std::memcpy(offer.target,p,80);
            offer.flags=2;offer.reserved_device=0;offer.reserved_index=0;
            (void)names_send(f.sender,offer);return;
        }
        bool admitted=false;for(unsigned i=0;i<m.barrier_count;++i)if(m.barrier[i]==names.local_device)admitted=true;
        if(names.fresh&&m.reserved_device!=names.local_device&&!admitted){
            NamesMessage request=m;request.opcode=NAMES_REQUEST;request.progress=0;
            std::memset(request.base,0,80);(void)names_send(f.sender,request);return;
        }
        const bool append=p&&!std::memcmp(p+16,m.target+16,16)&&name_le(p+64,4)<=name_le(m.target+64,4);
        NamesResult r=names_receive_begin(&names,m.registry,m.target,append,f.sender,m.epoch,m.barrier,m.barrier_count,m.reserved_device,m.reserved_index);
        if(r==NAMES_BUSY&&names_target(&names)&&std::memcmp(names_target(&names),m.target,80)){
            if(names_job_kind!=2||!names_verify.active||names_verify_controller!=f.sender||names_verify_epoch!=m.epoch||
               std::memcmp(names_verify.target,m.target,80)||!names_verify_binding(m)){
                if(names_job_kind!=2){names_jobs.verify=NamesRetarget{};names_job_kind=2;}
                if(names_retarget_begin(&names,&names_verify,m.target)!=NAMES_OK)return;
                names_bind_verify(m);
                names_verify_controller=f.sender;names_verify_epoch=m.epoch;
            }
            NamesMessage verify=m;verify.opcode=NAMES_REQUEST;verify.flags|=8;
            std::memcpy(verify.base,peer->base,80);verify.progress=names_verify.progress;
            (void)names_send(f.sender,verify);return;
        }
        if(r!=NAMES_OK){names.error=r;NamesMessage error=m;error.opcode=NAMES_ERROR;error.status=uint16_t(r);error.start=0;(void)names_send(f.sender,error);return;}
        NamesMessage reply=m;std::memcpy(reply.base,peer->base,80);
        if(names_phase(&names)==NAMES_PREPARED||(p&&!std::memcmp(p,m.target,80))){
            // A fresh session has no bound receiver base at the root yet.
            // Establish it with REQUEST before granting a prepared right.
            reply.opcode=std::memcmp(m.base,peer->base,80)?NAMES_REQUEST:NAMES_PREPARED_ACK;
            reply.progress=uint32_t(name_le(m.target+64,4));std::memcpy(reply.progress_hash,m.target+32,32);
        }
        else {reply.opcode=NAMES_REQUEST;reply.progress=uint32_t(name_le(names.ctl+52,4));std::memcpy(reply.progress_hash,names.ctl+56,32);}
        (void)names_send(f.sender,reply);return;
    }
    if(m.opcode==NAMES_PAGE&&(m.flags&8)){
        if(names_job_kind!=2||!names_verify.active||names_verify_controller!=f.sender||names_verify_epoch!=m.epoch||
           std::memcmp(m.target,names_verify.target,80)||!names_verify_binding(m)||
           std::memcmp(m.base,peer->base,80)||m.barrier_flags!=1)return;
        if(m.start!=names_verify.progress){
            NamesMessage request=m;request.opcode=NAMES_REQUEST;request.start=0;
            request.progress=names_verify.progress;(void)names_send(f.sender,request);return;
        }
        NamePageScratch page(f);const size_t n=(f.payload_length-328)/8;
        const NamesResult verified=names_retarget_page(&names,&names_verify,m.start,page.ids(),n);
        if(verified==NAMES_BUSY)return;
        if(verified!=NAMES_OK){names_retarget_cancel(&names_verify);names.error=verified;NamesMessage error=m;error.opcode=NAMES_ERROR;error.status=uint16_t(verified);error.start=0;(void)names_send(f.sender,error);return;}
        NamesMessage request=m;request.opcode=NAMES_REQUEST;request.start=0;
        std::memcpy(request.base,peer->base,80);
        if(names_verify.complete){
            if(names_retarget_accept(&names,&names_verify,f.sender,m.epoch,m.barrier,m.barrier_count)!=NAMES_OK)return;
            request.flags&=uint16_t(~8);request.progress=0;
        }else request.progress=names_verify.progress;
        (void)names_send(f.sender,request);return;
    }
    const uint8_t *t=names_target(&names);const uint8_t *p=names_published(&names);
    const bool samep=p&&!std::memcmp(p,m.target,80);
    if(std::memcmp(m.registry,names.ctl+24,16)||(!samep&&(!t||std::memcmp(t,m.target,80))))return;
    if(!samep&&(m.epoch!=name_le(names.ctl+88,8)||m.controller!=name_le(names.ctl+336,8)))return;
    if(m.opcode==NAMES_PAGE){
        NamePageScratch page(f);const size_t n=(f.payload_length-328)/8;
        const NamesResult page_result=names_receive_page(&names,m.start,page.ids(),n);
        if(page_result==NAMES_BUSY)return;
        if(page_result!=NAMES_OK)return;
        NamesMessage reply=m;std::memcpy(reply.base,peer->base,80);reply.start=0;
        reply.opcode=names_phase(&names)==NAMES_PREPARED?NAMES_PREPARED_ACK:NAMES_REQUEST;
        reply.progress=uint32_t(name_le(names.ctl+52,4));std::memcpy(reply.progress_hash,names.ctl+56,32);
        (void)names_send(f.sender,reply);return;
    }
    if(m.opcode==NAMES_COMMIT){
        if(!samep&&names_publish(&names)!=NAMES_OK)return;
        const NamesResult projection=names_confirm(&names,names_published(&names));
        if(projection!=NAMES_OK&&projection!=NAMES_NEED_SYNC)return;
        if(names_finish(&names)!=NAMES_OK)return;
        apply_hostname();NamesMessage reply=m;reply.opcode=NAMES_COMMIT_ACK;reply.progress=uint32_t(name_le(m.target+64,4));std::memcpy(reply.progress_hash,m.target+32,32);
        (void)names_send(f.sender,reply);
    }
}

#undef names_verify
#undef names_merge
