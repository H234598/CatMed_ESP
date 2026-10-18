#include "katmed_mesh.h"
#include "katmed_dhcp_diag.h"
#include "katmed_mesh_payload.h"
#include "katmed_mesh_session.h"
#include "katmed_mesh_status.h"
#include "katmed_mesh_interaction.hpp"

#include <atomic>
#include <cstring>
#include "esp_app_desc.h"
#include "esp_bridge.h"
#include "esp_event.h"
#include "esp_mesh_lite.h"
#include "esp_mesh_lite_espnow.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Mesh-Lite 1.0.2 selects ESP-NOW v2 payloads with IDF 5.5.5 on both targets.
 * Fail the target build rather than truncate an authenticated snapshot. */
static_assert(KATMED_MESH_WIRE_MAX < ESPNOW_PAYLOAD_MAX_LEN,
              "Mesh-Lite raw payload cannot carry a complete Katmed snapshot");

namespace {
constexpr uint32_t kMessageId=0x4b4d5301u;
constexpr uint64_t kStateIntervalMs=5000;
constexpr uint64_t kPendingIntervalMs=1000;
constexpr uint64_t kSharedTimeoutMs=10000;
constexpr uint64_t kRecoveryDiscoveryMs=5000;
constexpr uint64_t kRecoveryLimitMs=10000;
constexpr unsigned kRxQueue=3;
constexpr unsigned kPendingBurst=3;
constexpr uint64_t kIdBlock=256;
constexpr unsigned kMaxCandidates=KATMED_MESH_PEERS*KATMED_STORE_PENDING_MAX;
struct Packet { uint16_t length; uint8_t bytes[KATMED_MESH_WIRE_MAX]; };
struct Candidate { StorePendingConfirm pending; uint64_t age_ms_at_send; };
struct DiagnosticCounters {
    std::atomic<uint32_t> sdk_send_failures{0}, rx_queue_drops{0},
        observed_level_changes{0}, ip_got_transitions{0}, ip_lost_transitions{0},
        authenticated_session_up{0}, authenticated_session_down{0};
};
void saturating_increment(std::atomic<uint32_t> &counter){
    uint32_t old=counter.load(std::memory_order_relaxed);
    while(old!=UINT32_MAX&&!counter.compare_exchange_weak(
              old,old+1,std::memory_order_relaxed)){}
}
struct Context {
    KatmedMeshServices services{};
    StoreProvision provision{};
    QueueHandle_t queue=nullptr;
    bool started=false, active=false, dirty=false;
    std::atomic<bool> has_ip{false};
    bool shared_waiting=false, shared_acked=false;
    uint64_t coordinator_device=0;
    uint64_t tx_counter=0;
    uint8_t last_level=0;
    DiagnosticCounters diagnostics{};
    bool diagnostic_session_live=false;
    KatmedMeshSessions sessions{};
    MeshStatusState status{};
    AppEvent shared_event{};
    uint64_t shared_since=0, last_state=0, last_pending=0;
    uint64_t next_id=0, remaining_ids=0;
    uint8_t pending_cursor=0;
    bool pending_urgent=false;
    Candidate candidates[kMaxCandidates]{};
    uint8_t candidate_count=0;
    uint64_t candidate_started=0;
    bool recovery_candidates=false;
    char ap_ssid[33]{}, ap_password[64]{};
    Packet receive{};
    uint8_t tx[KATMED_MESH_WIRE_MAX]{};
};
static_assert(sizeof(Candidate)*kMaxCandidates<=8192,
              "Mesh recovery candidates exceed the bounded static budget");
static_assert(sizeof(Context)<=12288,
              "Mesh adapter static context exceeds the bounded RAM budget");
static_assert(sizeof(Packet)<=1216,
              "Mesh raw receive queue entry exceeds the bounded RAM budget");
Context ctx;
MeshHistory history;
katmed::Interaction interaction;
static_assert(sizeof(Context)+sizeof(MeshHistory)+sizeof(katmed::Interaction)+sizeof(EventId)+KATMED_MESH_PEERS*(sizeof(uint64_t)+sizeof(bool))+2*sizeof(uint64_t)<=16384,"mesh static aggregate bounded to 16KiB; RX queue separately bounded");
EventId interaction_id{};
uint64_t pending_admission[KATMED_MESH_PEERS]{};
bool admission_verified[KATMED_MESH_PEERS]{};
uint64_t greeting_id=0,greeting_until=0;
bool root();
bool linked();
void bind_interaction(uint64_t now){
    const uint64_t self=ctx.services.store?ctx.services.store->record.device:0;
    const bool r=root();uint64_t authority=r?self:ctx.coordinator_device,epoch=0;
    const uint8_t *nonce=r?ctx.sessions.local_nonce:nullptr;
    if(!r)for(const auto &p:ctx.sessions.peers)if(p.live&&p.device==authority)nonce=p.nonce;
    if(nonce)for(unsigned i=0;i<8;++i)epoch=(epoch<<8)|nonce[i];
    bool fresh=r;
    if(!r)for(unsigned i=0;i<KATMED_MESH_PEERS;++i)if(ctx.sessions.peers[i].live&&ctx.sessions.peers[i].device==authority)
        fresh=mesh_status_fresh(now,ctx.status.peers[i].hello_ms,ctx.status.peers[i].hello_valid);
    bool connected=linked()&&fresh;
    interaction.bind(self,authority,epoch,r,connected,now);
}

bool id_equal(EventId a,EventId b) {
    return a.device==b.device&&a.epoch==b.epoch&&a.sequence==b.sequence;
}
bool recent(const AppState &state,EventId id) {
    if(state.has_last_confirmation&&id_equal(state.last_confirmation_id,id))return true;
    for(unsigned i=0;i<state.recent_event_count;++i)
        if(id_equal(state.recent_events[i],id))return true;
    return false;
}
bool current_group() {
    return ctx.active && ctx.services.store && ctx.services.state &&
        store_get_status(ctx.services.store,nullptr,nullptr,nullptr)==STORE_OK &&
        ctx.services.store->record.provision.configured &&
        std::memcmp(ctx.provision.group,ctx.services.store->record.provision.group,16)==0 &&
        std::memcmp(ctx.provision.secret,ctx.services.store->record.provision.secret,32)==0;
}
bool root() { return ctx.active && esp_mesh_lite_get_level()==ROOT; }
bool linked() { return current_group() && esp_mesh_lite_get_level()>=ROOT; }
bool session_live(){
    if(!root())return ctx.coordinator_device!=0;
    for(const auto &peer:ctx.sessions.peers)if(peer.live)return true;
    return false;
}
void note_session_transition(){
    const bool live=linked()&&session_live();
    if(live==ctx.diagnostic_session_live)return;
    ctx.diagnostic_session_live=live;
    if(live)saturating_increment(ctx.diagnostics.authenticated_session_up);
    else saturating_increment(ctx.diagnostics.authenticated_session_down);
}
bool rekey(){
    uint8_t nonce[16];esp_fill_random(nonce,sizeof(nonce));
    if(!katmed_mesh_sessions_init(&ctx.sessions,nonce))return false;
    ctx.coordinator_device=0;ctx.tx_counter=0;ctx.dirty=true;interaction_id=EventId{};
    interaction.bind(ctx.services.store?ctx.services.store->record.device:0,0,0,false,false,ctx.status.now_ms);std::memset(pending_admission,0,sizeof(pending_admission));std::memset(admission_verified,0,sizeof(admission_verified));
    const uint64_t observed=ctx.status.now_ms;
    const bool clock_valid=ctx.status.clock_valid;
    ctx.status=MeshStatusState{};ctx.status.now_ms=observed;
    ctx.status.clock_valid=clock_valid;ctx.status.started_ms=observed;
    ctx.pending_cursor=0;
    ctx.candidate_count=0;ctx.recovery_candidates=false;
    /* An ACK belongs to the old root session. Keep the original UI timeout
     * and request, but require fresh acceptance after the new handshake. */
    ctx.shared_acked=false;
    if(ctx.queue)xQueueReset(ctx.queue);
    note_session_transition();
    return true;
}
bool next_event_id(EventId &id) {
    if(ctx.remaining_ids==0) {
        EventId first{};
        if(store_reserve_event_ids(ctx.services.store,uint32_t(kIdBlock),&first)!=STORE_OK)
            return false;
        ctx.next_id=first.sequence;ctx.remaining_ids=kIdBlock;
    }
    id={ctx.services.store->record.device,ctx.services.store->record.epoch,ctx.next_id++};
    --ctx.remaining_ids;
    return true;
}
uint64_t u64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;++i)v=(v<<8)|p[i];return v;}
void p64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;++i)p[i]=uint8_t(v>>(56-8*i));}
void p32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(24-8*i));}
uint32_t u32(const uint8_t *p){return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
bool send_one(KatmedMeshFrame &frame){
    if(!linked()||ctx.tx_counter==UINT64_MAX)return false;
    if(frame.kind!=MESH_HELLO){
        uint64_t target=frame.recipient;
        if(!root()&&target==0)target=ctx.coordinator_device;
        if(target==0)return false;
        const KatmedMeshPeer *peer=nullptr;
        for(const auto &candidate:ctx.sessions.peers)
            if(candidate.live&&candidate.device==target){peer=&candidate;break;}
        if(!peer)return false;
        frame.recipient=target;
        std::memcpy(frame.receiver_nonce,peer->nonce,16);
    }
    std::memcpy(frame.group,ctx.provision.group,16);
    std::memcpy(frame.session_nonce,ctx.sessions.local_nonce,16);
    frame.session_counter=++ctx.tx_counter;
    size_t length=0;
    if(!katmed_mesh_wire_encode(&frame,ctx.provision.secret,ctx.tx,sizeof(ctx.tx),&length))
        return false;
    esp_mesh_lite_msg_config_t request{};
    request.raw_msg.msg_id=kMessageId;
    request.raw_msg.expect_resp_msg_id=0;
    request.raw_msg.max_retry=0;
    request.raw_msg.data=ctx.tx;
    request.raw_msg.size=length;
    request.raw_msg.raw_resend=root() ? esp_mesh_lite_send_broadcast_raw_msg_to_child
                                      : esp_mesh_lite_send_raw_msg_to_root;
    if(esp_mesh_lite_send_msg(ESP_MESH_LITE_RAW_MSG,&request)!=ESP_OK){
        saturating_increment(ctx.diagnostics.sdk_send_failures);
        return false;
    }
    return true;
}
bool send(KatmedMeshFrame &frame){
    if(frame.kind!=MESH_HELLO&&root()&&frame.recipient==0){
        bool any=false,ok=true;
        for(const auto &peer:ctx.sessions.peers){
            if(!peer.live)continue;
            frame.recipient=peer.device;
            any=true;if(!send_one(frame))ok=false;
        }
        frame.recipient=0;
        return any&&ok;
    }
    return send_one(frame);
}
KatmedMeshFrame frame_for(KatmedMeshKind kind,EventId id,uint64_t recipient=0){
    KatmedMeshFrame f{};f.kind=kind;f.id=id;f.sender=id.device;
    f.parent_revision=ctx.services.state->revision;f.recipient=recipient;return f;
}
int status_peer(uint64_t id){
    for(unsigned i=0;i<KATMED_MESH_PEERS;++i)
        if(ctx.sessions.peers[i].live&&ctx.sessions.peers[i].device==id)return int(i);
    return -1;
}
bool fresh_hello(unsigned i){
    const auto &m=ctx.status.peers[i];
    return ctx.sessions.peers[i].live&&mesh_status_fresh(ctx.status.now_ms,m.hello_ms,m.hello_valid);
}
uint8_t local_status_flags(){
    uint8_t flags=MS_PREHISTORY;
    if(history.saturated||history.count==256)flags|=MS_SATURATED;
    if(history.error)flags|=MS_STORE_ERROR;
    if(!ctx.status.clock_valid||ctx.status.now_ms-ctx.status.started_ms<MESH_STATUS_FRESH_MS)
        flags|=MS_REBUILD;
    unsigned occupied=0;for(const auto &p:ctx.sessions.peers)if(p.live)++occupied;
    if(occupied==KATMED_MESH_PEERS||ctx.status.now_ms<ctx.status.warning_until_ms)flags|=MS_CAPACITY;
    return flags;
}
uint8_t status_quality(){
    uint8_t flags=local_status_flags();
    if(root()){
        for(unsigned i=0;i<KATMED_MESH_PEERS;++i){
            if(!fresh_hello(i))continue;
            const auto &m=ctx.status.peers[i];
            if(!mesh_status_complete(&m,ctx.status.now_ms))flags|=MS_REBUILD;
            if(mesh_status_fresh(ctx.status.now_ms,m.status_ms,m.status_valid))
                flags|=m.flags&uint8_t(MS_STORE_ERROR|MS_REBUILD|MS_CAPACITY|MS_SATURATED);
        }
    }else{
        const int i=status_peer(ctx.coordinator_device);
        if(i<0||!fresh_hello(unsigned(i))||
           !mesh_status_complete(&ctx.status.peers[i],ctx.status.now_ms))flags|=MS_REBUILD;
        if(i>=0&&mesh_status_fresh(ctx.status.now_ms,ctx.status.peers[i].status_ms,
                                   ctx.status.peers[i].status_valid))
            flags|=ctx.status.root_flags&uint8_t(MS_STORE_ERROR|MS_REBUILD|MS_CAPACITY|MS_SATURATED);
    }
    return flags;
}
void root_current(){
    auto &s=ctx.status;s.current_count=1;s.current[0]=ctx.services.store->record.device;
    for(unsigned i=0;i<KATMED_MESH_PEERS;++i)if(fresh_hello(i))
        s.current[s.current_count++]=ctx.sessions.peers[i].device;
    for(unsigned i=1;i<s.current_count;++i){
        const uint64_t id=s.current[i];unsigned j=i;
        while(j&&s.current[j-1]>id){s.current[j]=s.current[j-1];--j;}s.current[j]=id;
    }
}
void send_status(){
    auto &s=ctx.status;
    if(s.now_ms-s.last_tx_ms<1000)return;
    s.last_tx_ms=s.now_ms;
    if(s.advertised_generation!=history.generation){
        for(auto &m:s.peers)m.cursor=0;
        s.advertised_generation=history.generation;
    }
    int chosen=-1;
    if(root()){
        root_current();
        for(unsigned step=0;step<KATMED_MESH_PEERS;++step){
            const unsigned i=(s.recipient_cursor+step)%KATMED_MESH_PEERS;
            if(fresh_hello(i)){chosen=int(i);s.recipient_cursor=uint8_t((i+1)%KATMED_MESH_PEERS);break;}
        }
    }else{chosen=status_peer(ctx.coordinator_device);if(chosen>=0&&!fresh_hello(unsigned(chosen)))chosen=-1;}
    if(chosen<0)return;
    auto &m=s.peers[chosen];const unsigned pages=history.count?(history.count+99)/100:1;
    if(m.cursor>=pages)m.cursor=0;
    EventId id{};if(!next_event_id(id))return;
    auto f=frame_for(MESH_STATUS,id,ctx.sessions.peers[chosen].device);
    f.payload_length=mesh_status_encode(&history,uint8_t(root()?(status_quality()|MS_CURRENT):local_status_flags()),
        m.cursor,root()?ctx.services.store->record.device:ctx.coordinator_device,
        s.current,root()?s.current_count:0,f.payload);
    if(f.payload_length&&send_one(f))m.cursor=uint8_t((m.cursor+1)%pages);
}
void receive_status(const KatmedMeshFrame &f){
    const int index=status_peer(f.sender);if(index<0||!fresh_hello(unsigned(index)))return;
    MeshStatusPage page{};
    if(!mesh_status_decode(f.payload,f.payload_length,!root(),f.sender,&page))return;
    if(root()&&page.root!=ctx.services.store->record.device)return;
    auto &m=ctx.status.peers[index];
    if(!mesh_status_merge(&history,&m,&page,ctx.status.now_ms))return;
    if(root()&&page.stride==9){
        // A peer's own durable prior admission is negative evidence for a new join,
        // not authority to import child admission facts into the root history.
        for(unsigned j=0;j<page.page_ids;++j)if(u64(page.ids+9*j)==f.sender&&page.ids[9*j+8]){
            for(unsigned candidate=0;candidate<KATMED_MESH_PEERS;++candidate)
                if(pending_admission[candidate]==f.sender){pending_admission[candidate]=0;admission_verified[candidate]=false;}
        }
    }
    if(!root()){
        ctx.status.current_count=page.current_count;ctx.status.current_valid=false;
        ctx.status.root_flags=page.flags;
        for(unsigned i=0;i<page.current_count;++i){
            ctx.status.current[i]=u64(page.current+8*i);
            if(ctx.status.current[i]==ctx.services.store->record.device)ctx.status.current_valid=true;
        }
        if(!ctx.status.current_valid)ctx.status.root_flags|=MS_CAPACITY;
    }
}
void send_ack(EventId subject,uint64_t destination,uint8_t status){
    EventId id{};if(!next_event_id(id))return;
    auto f=frame_for(MESH_ACK,id,destination);
    p64(f.payload,subject.device);p64(f.payload+8,subject.epoch);
    p64(f.payload+16,subject.sequence);f.payload[24]=status;
    f.payload_length=25;(void)send(f);
}
void send_state(const ClockSample *now){
    if(!linked()||!session_live())return;
    StateSnapshot snap{};
    if(app_make_snapshot(ctx.services.state,now,&snap)!=APP_OK)return;
    EventId id{};if(!next_event_id(id))return;
    auto f=frame_for(MESH_STATE,id);
    if(!katmed_mesh_encode_snapshot(&snap,f.payload,sizeof(f.payload),&f.payload_length))return;
    (void)send(f);
}
void send_time(const ClockSample *now){
    if(!root()||!session_live()||!now->utc_valid||
       now->quality==CLOCK_UNAVAILABLE)return;
    EventId id{};if(!next_event_id(id))return;
    auto f=frame_for(MESH_TIME_SYNC,id);
    p64(f.payload,uint64_t(now->utc_ms));f.payload[8]=uint8_t(now->quality);
    f.payload_length=9;(void)send(f);
}
void send_hello(uint64_t recipient=0,const uint8_t *echo=nullptr){
    EventId id{};if(!next_event_id(id))return;
    auto f=frame_for(MESH_HELLO,id,recipient);
    uint8_t zero[16]{};
    uint8_t pending=0;
    if(store_get_status(ctx.services.store,nullptr,&pending,nullptr)!=STORE_OK)return;
    if(katmed_mesh_encode_hello(echo?echo:zero,ctx.services.state->catalog_id,
        esp_app_get_description()->version,pending,
        f.payload,sizeof(f.payload),&f.payload_length))
        (void)send(f);
}
void send_pending(const ClockSample *now){
    if(root()||!linked()||!session_live())return;
    uint8_t count=0;
    if(store_get_status(ctx.services.store,nullptr,&count,nullptr)!=STORE_OK)return;
    for(unsigned slot=0;slot<count&&slot<kPendingBurst;++slot){
        const unsigned i=(ctx.pending_cursor+slot)%count;
        StorePendingConfirm pending{};
        if(store_pending_at(ctx.services.store,i,&pending)!=STORE_OK)break;
        auto f=frame_for(MESH_CONFIRM,pending.id);
        f.parent_revision=pending.parent_revision;
        uint64_t age=UINT64_MAX;
        if(now->domain==pending.occurred_at.domain&&
           now->monotonic_ms>=pending.occurred_at.monotonic_ms)
            age=now->monotonic_ms-pending.occurred_at.monotonic_ms;
        if(katmed_mesh_encode_confirm(&pending,age,f.payload,
                                      sizeof(f.payload),&f.payload_length))
            (void)send(f);
    }
    if(count)ctx.pending_cursor=uint8_t((ctx.pending_cursor+kPendingBurst)%count);
}
void reconcile_own_pending(){
    if(!root()||!current_group())return;
    uint8_t count=0;
    if(store_get_status(ctx.services.store,nullptr,&count,nullptr)!=STORE_OK)return;
    for(unsigned i=0;i<count;){
        StorePendingConfirm pending{};
        if(store_pending_at(ctx.services.store,i,&pending)!=STORE_OK)break;
        /* The local reducer has already applied this confirmation. A new
         * coordinator only clears its durable outbox after verifying the
         * exact event remains the current committed confirmation. */
        if(ctx.services.state->has_last_confirmation&&
           id_equal(ctx.services.state->last_confirmation_id,pending.id)&&
           store_ack_pending(ctx.services.store,ctx.services.state,
                             pending.id,true)==STORE_OK){
            --count;ctx.dirty=true;continue;
        }
        ++i;
    }
}
bool parse_clock(const uint8_t *p,size_t n,ClockSample &v){
    if(n!=26||p[24]>1||p[25]>CLOCK_SYNCHRONIZED)return false;
    v.monotonic_ms=u64(p);v.domain=u64(p+8);
    uint64_t bits=u64(p+16);std::memcpy(&v.utc_ms,&bits,8);
    v.utc_valid=p[24]==1;v.quality=ClockQuality(p[25]);
    return v.domain!=0 && (v.utc_valid || v.quality!=CLOCK_SYNCHRONIZED);
}
void write_clock(uint8_t *p,ClockSample v){
    p64(p,v.monotonic_ms);p64(p+8,v.domain);p64(p+16,uint64_t(v.utc_ms));
    p[24]=v.utc_valid?1:0;p[25]=uint8_t(v.quality);
}
bool parse_shared(const KatmedMeshFrame &f,AppEvent &event){
    if(f.payload_length<27)return false;
    const uint8_t kind=f.payload[0];
    if(kind<EVENT_SET_MEDICATION_INTERVAL||kind>EVENT_SET_NEXT_LOCAL_TIME)return false;
    event.kind=AppEventKind(kind);event.id=f.id;event.sender=f.sender;
    event.round=ctx.services.state->round;event.parent_revision=f.parent_revision;
    event.coordinator_accepted=true;
    if(!parse_clock(f.payload+1,26,event.occurred_at))return false;
    const uint8_t *value=f.payload+27;
    if(kind==EVENT_SET_MEDICATION_INTERVAL||kind==EVENT_SET_IMAGE_INTERVAL){
        if(f.payload_length!=31)return false;
        event.value.interval_seconds=u32(value);
        return event.value.interval_seconds!=0;
    }
    if(kind==EVENT_SET_ROTATION_MODE){
        if(f.payload_length!=28||value[0]>ROTATION_RANDOM)return false;
        event.value.rotation=RotationMode(value[0]);return true;
    }
    if(f.payload_length!=45||value[0]>23||value[1]>59)return false;
    event.value.local_time.hour=value[0];event.value.local_time.minute=value[1];
    event.value.local_time.selected_utc={true,int64_t(u64(value+2))};
    event.value.local_time.selected_effective_utc={true,int64_t(u64(value+10))};
    return true;
}
bool encode_shared(const AppEvent &event,KatmedMeshFrame &f){
    if(event.kind<EVENT_SET_MEDICATION_INTERVAL||event.kind>EVENT_SET_NEXT_LOCAL_TIME)return false;
    f.payload[0]=uint8_t(event.kind);write_clock(f.payload+1,event.occurred_at);
    uint8_t *value=f.payload+27;
    if(event.kind==EVENT_SET_MEDICATION_INTERVAL||event.kind==EVENT_SET_IMAGE_INTERVAL){
        p32(value,event.value.interval_seconds);f.payload_length=31;
    }else if(event.kind==EVENT_SET_ROTATION_MODE){
        value[0]=uint8_t(event.value.rotation);f.payload_length=28;
    }else{
        value[0]=event.value.local_time.hour;value[1]=event.value.local_time.minute;
        p64(value+2,uint64_t(event.value.local_time.selected_utc.ms));
        p64(value+10,uint64_t(event.value.local_time.selected_effective_utc.ms));
        f.payload_length=45;
    }
    return true;
}
bool same_clock(ClockSample a,ClockSample b){
    return a.monotonic_ms==b.monotonic_ms&&a.domain==b.domain&&
           a.utc_ms==b.utc_ms&&a.utc_valid==b.utc_valid&&a.quality==b.quality;
}
bool same_pending(const StorePendingConfirm &a,const StorePendingConfirm &b){
    return id_equal(a.id,b.id)&&a.sender==b.sender&&
           id_equal(a.round.origin,b.round.origin)&&a.round.ordinal==b.round.ordinal&&
           a.parent_revision.number==b.parent_revision.number&&
           a.parent_revision.coordinator==b.parent_revision.coordinator&&
           same_clock(a.occurred_at,b.occurred_at);
}
/* 0 means absent, 1 an exact durable repeat, 2 a conflicting reuse of an ID. */
int known_conflict(const StorePendingConfirm &pending){
    uint8_t count=0;
    if(store_conflict_count(ctx.services.store,&count)!=STORE_OK)return 2;
    for(unsigned i=0;i<count;++i){
        StorePendingConfirm conflict{};
        if(store_conflict_at(ctx.services.store,i,&conflict)==STORE_OK&&
           id_equal(conflict.id,pending.id))return same_pending(conflict,pending)?1:2;
    }
    return 0;
}
void collect_confirm(const KatmedMeshFrame &f,const ClockSample *now){
    StorePendingConfirm pending{};uint64_t age=UINT64_MAX;
    if(!katmed_mesh_decode_confirm(f.payload,f.payload_length,&pending,&age))return;
    pending.id=f.id;pending.sender=f.sender;pending.parent_revision=f.parent_revision;
    if(ctx.services.state->has_last_confirmation&&
       id_equal(ctx.services.state->last_confirmation_id,f.id)){
        send_ack(f.id,f.sender,1);return;
    }
    const int archived=known_conflict(pending);
    if(archived){if(archived==1)send_ack(f.id,f.sender,2);return;}
    for(unsigned i=0;i<ctx.candidate_count;++i)
        if(id_equal(ctx.candidates[i].pending.id,f.id))return;
    if(ctx.candidate_count==kMaxCandidates){
        (void)store_mark_divergent(ctx.services.store,ctx.services.state);
        return;
    }
    if(ctx.candidate_count==0){
        ctx.candidate_started=now->monotonic_ms;
        ctx.recovery_candidates=false;
    }
    ctx.candidates[ctx.candidate_count++]={pending,age};
    if(age==UINT64_MAX||age>1000)ctx.recovery_candidates=true;
}
/* Return 2 when different clocks have no common monotonic domain or both
 * trustworthy UTC readings. No fabricated wall time is used for ordering. */
int compare_candidates(const Candidate &a,const Candidate &b){
    const ClockSample &x=a.pending.occurred_at,&y=b.pending.occurred_at;
    int order=0;
    if(a.pending.sender==b.pending.sender&&
       a.pending.id.epoch==b.pending.id.epoch&&x.domain==y.domain){
        if(x.monotonic_ms<y.monotonic_ms)order=-1;
        else if(x.monotonic_ms>y.monotonic_ms)order=1;
    }else if(x.utc_valid&&y.utc_valid){
        if(x.utc_ms<y.utc_ms)order=-1;
        else if(x.utc_ms>y.utc_ms)order=1;
    }else return 2;
    if(order)return order;
    const EventId p=a.pending.id,q=b.pending.id;
    if(p.device!=q.device)return p.device<q.device?-1:1;
    if(p.epoch!=q.epoch)return p.epoch<q.epoch?-1:1;
    if(p.sequence!=q.sequence)return p.sequence<q.sequence?-1:1;
    return 0;
}
bool candidates_complete(){
    for(const auto &peer:ctx.sessions.peers){
        if(!peer.live||peer.advertised_pending==0)continue;
        unsigned have=0;
        for(unsigned i=0;i<ctx.candidate_count;++i)
            if(ctx.candidates[i].pending.sender==peer.device)++have;
        if(have<peer.advertised_pending)return false;
    }
    return true;
}
void reconcile_candidates(const ClockSample *now){
    if(!root()||ctx.candidate_count==0)return;
    const uint64_t age=now->monotonic_ms>=ctx.candidate_started
        ?now->monotonic_ms-ctx.candidate_started:0;
    if(!candidates_complete()){
        if(age>=kRecoveryLimitMs){
            (void)store_mark_divergent(ctx.services.store,ctx.services.state);
            ctx.candidate_count=0;ctx.recovery_candidates=false;
        }
        return;
    }
    if(ctx.recovery_candidates&&age<kRecoveryDiscoveryMs)return;
    unsigned order[kMaxCandidates];
    for(unsigned i=0;i<ctx.candidate_count;++i)order[i]=i;
    for(unsigned i=1;i<ctx.candidate_count;++i){
        unsigned j=i;
        while(j>0&&compare_candidates(ctx.candidates[order[j]],
                                      ctx.candidates[order[j-1]])<0){
            const unsigned v=order[j];order[j]=order[j-1];order[j-1]=v;--j;
        }
    }
    for(unsigned i=0;i<ctx.candidate_count;++i){
        for(unsigned j=i+1;j<ctx.candidate_count;++j){
            const int cmp=compare_candidates(ctx.candidates[order[i]],
                                             ctx.candidates[order[j]]);
            if(cmp==2||cmp>0){
                (void)store_mark_divergent(ctx.services.store,ctx.services.state);
                ctx.candidate_count=0;ctx.recovery_candidates=false;
                return;
            }
        }
    }
    const unsigned count=ctx.candidate_count;
    for(unsigned index=0;index<count;++index){
        const StorePendingConfirm &pending=ctx.candidates[order[index]].pending;
        AppEvent event{};event.kind=EVENT_CONFIRM;event.id=pending.id;
        event.sender=pending.sender;event.round=pending.round;
        event.parent_revision=pending.parent_revision;
        event.occurred_at=pending.occurred_at;event.coordinator_accepted=true;
        AppEffects effects{};AppResult core=APP_INVALID;
        const StoreResult result=store_apply_event(ctx.services.store,ctx.services.state,
                                                  &event,now,false,&effects,&core);
        if(result==STORE_OK){
            send_ack(pending.id,pending.sender,1);ctx.dirty=true;
        }else if(result==STORE_DUPLICATE&&recent(*ctx.services.state,pending.id)){
            send_ack(pending.id,pending.sender,1);
        }else if(result==STORE_STALE&&(core==APP_STALE||core==APP_CONFLICT)){
            const StoreResult archived=store_record_conflict(ctx.services.store,
                                        ctx.services.state,&pending,now);
            if(archived==STORE_OK||archived==STORE_DUPLICATE){
                send_ack(pending.id,pending.sender,2);ctx.dirty=true;
            }
        }else if(result==STORE_INVALID&&
                 (core==APP_TIME_UNAVAILABLE||core==APP_OVERFLOW)){
            (void)store_mark_divergent(ctx.services.store,ctx.services.state);
        }else if(result==STORE_INVALID){
            send_ack(pending.id,pending.sender,0);
        }
        if(store_get_status(ctx.services.store,nullptr,nullptr,nullptr)!=STORE_OK)break;
    }
    ctx.candidate_count=0;ctx.recovery_candidates=false;
}
void receive(const KatmedMeshFrame &f,const ClockSample *now){
    if(f.recipient!=0&&f.recipient!=ctx.services.store->record.device)return;
    if(f.sender==ctx.services.store->record.device)return;
    if(f.kind==MESH_HELLO){
        uint8_t echo[16],advertised=0;bool catalog_mismatch=false;
        if(!katmed_mesh_decode_hello(f.payload,f.payload_length,
           ctx.services.state->catalog_id,esp_app_get_description()->version,
           echo,&catalog_mismatch,&advertised))return;
        if(std::memcmp(echo,ctx.sessions.local_nonce,16)==0&&
           katmed_mesh_sessions_nonce_changed(&ctx.sessions,f.sender,f.session_nonce)){
            /* A peer booted again. Rotate our challenge so an old HELLO
             * echoing the former challenge cannot resurrect its old session. */
            if(!rekey()){katmed_mesh_stop();return;}
            send_hello();
            return;
        }
        bool already=false;
        for(const auto &peer:ctx.sessions.peers)
            if(peer.live&&peer.device==f.sender&&
               std::memcmp(peer.nonce,f.session_nonce,16)==0){already=true;break;}
        const bool verified=katmed_mesh_sessions_accept_hello(&ctx.sessions,
            f.sender,f.session_nonce,echo);
        if(!verified){
            if(std::memcmp(echo,ctx.sessions.local_nonce,16)==0){
                bool full=true;for(const auto &p:ctx.sessions.peers)if(!p.live)full=false;
                if(full)ctx.status.warning_until_ms=now->monotonic_ms+MESH_STATUS_FRESH_MS;
            }
            send_hello(f.sender,f.session_nonce);return;
        }
        if(!katmed_mesh_sessions_check(&ctx.sessions,f.sender,
                                       f.session_nonce,f.session_counter,true))return;
        if(catalog_mismatch)
            ctx.services.state->catalog_mismatch=true;
        for(auto &peer:ctx.sessions.peers)
            if(peer.live&&peer.device==f.sender)
                peer.advertised_pending=advertised;
        if(!root()){
            if(ctx.coordinator_device&&ctx.coordinator_device!=f.sender)
                katmed_mesh_sessions_forget(&ctx.sessions,ctx.coordinator_device);
            if(ctx.coordinator_device!=f.sender){
                ctx.status.current_valid=false;
                for(auto &m:ctx.status.peers){m.mask=0;m.status_valid=false;m.generation=0;}
            }
            ctx.coordinator_device=f.sender;
        }
        const int status_index=status_peer(f.sender);
        if(status_index>=0){auto &m=ctx.status.peers[status_index];
            m.hello_ms=now->monotonic_ms;m.hello_valid=true;}
        bool known=false;for(unsigned i=0;i<history.count;++i)if(history.ids[i]==f.sender)known=true;
        if(root()&&!known&&!history.error){for(auto &candidate:pending_admission)if(!candidate||candidate==f.sender){candidate=f.sender;break;}}
        mesh_history_add(&history,f.sender);
        /* Both sides must prove they heard the other's fresh challenge.
         * A leaf that has just verified the root must return its echo so
         * the root can authenticate this leaf before sending STATE. */
        if(!already)send_hello(f.sender,f.session_nonce);
        if(root())ctx.dirty=true;
        note_session_transition();
        return;
    }
    /* A recorded frame from an earlier boot lacks the fresh challenge nonce.
     * Within this live session, counters reject replay even when EventIds are
     * reserved in separate durable blocks. */
    if(std::memcmp(f.receiver_nonce,ctx.sessions.local_nonce,16)!=0)return;
    if(!katmed_mesh_sessions_check(&ctx.sessions,f.sender,f.session_nonce,
                                   f.session_counter,true))return;
    if(!root()&&f.sender!=ctx.coordinator_device)return;
    if(f.kind==MESH_INTERACTION){
        bind_interaction(now->monotonic_ms);
        katmed::InteractionMessage message;
        if(katmed::interaction_decode(f.payload,f.payload_length,message)){
            interaction.receive(f.sender,message,now->monotonic_ms);
            if(root()&&message.op==1)for(unsigned i=0;i<KATMED_MESH_PEERS;++i)if(pending_admission[i]==f.sender){
                if(message.flags&8)admission_verified[i]=true;
                else {pending_admission[i]=0;admission_verified[i]=false;}
            }
        }
        return;
    }
    if(f.kind==MESH_STATUS){receive_status(f);return;}
    if(f.kind==MESH_TIME_SYNC){
        if(root()||f.sender!=ctx.coordinator_device||f.payload_length!=9||
           f.payload[8]<CLOCK_HOLDOVER||
           f.payload[8]>CLOCK_SYNCHRONIZED||!ctx.services.utc_received)return;
        uint64_t bits=u64(f.payload);int64_t utc;std::memcpy(&utc,&bits,8);
        ctx.services.utc_received(ctx.services.context,now->monotonic_ms,utc,
                                  ClockQuality(f.payload[8]));
        return;
    }
    if(f.kind==MESH_CONFIRM){
        if(!root())return;
        collect_confirm(f,now);
        return;
    }
    if(f.kind==MESH_SET_SHARED_SETTINGS){
        if(!root())return;
        AppEvent event{};
        if(!parse_shared(f,event))return;
        AppEffects effects{};AppResult core=APP_INVALID;
        const StoreResult result=store_apply_event(ctx.services.store,ctx.services.state,
                                                  &event,now,false,&effects,&core);
        const bool accepted=result==STORE_OK||
            (result==STORE_DUPLICATE&&recent(*ctx.services.state,f.id));
        send_ack(f.id,f.sender,accepted?1:0);
        if(result==STORE_OK)ctx.dirty=true;
        return;
    }
    if(f.kind==MESH_ACK){
        if(root()||f.sender!=ctx.coordinator_device||f.payload_length!=25||
           f.payload[24]>2)return;
        EventId subject{u64(f.payload),u64(f.payload+8),u64(f.payload+16)};
        if(ctx.shared_waiting&&id_equal(subject,ctx.shared_event.id)){
            if(f.payload[24]==1)ctx.shared_acked=true;
            else{
                ctx.shared_waiting=false;
                if(ctx.services.shared_result)
                    ctx.services.shared_result(ctx.services.context,subject,false);
            }
        }
        if(f.payload[24]==1){
            const StoreResult result=store_ack_pending(ctx.services.store,
                                                      ctx.services.state,subject,true);
            (void)result; /* A missing pending entry is normal for shared ACKs. */
        }else if(f.payload[24]==2){
            if(store_reject_pending(ctx.services.store,ctx.services.state,
                                    subject)==STORE_OK)ctx.dirty=true;
        }
        return;
    }
    if(f.kind==MESH_STATE){
        StateSnapshot snapshot{};
        if(!katmed_mesh_decode_snapshot(f.payload,f.payload_length,&snapshot))return;
        /* The reducer rebases medication state across catalog mismatches and
         * retains the local catalog with a neutral-image mismatch marker. */
        AppEvent event{};event.kind=EVENT_ADOPT_SNAPSHOT;event.id=f.id;
        event.sender=f.sender;event.round=ctx.services.state->round;
        event.parent_revision=ctx.services.state->revision;
        event.occurred_at=*now;event.coordinator_accepted=true;
        event.value.snapshot=&snapshot;
        AppEffects effects{};AppResult core=APP_INVALID;
        const StoreResult result=store_apply_event(ctx.services.store,ctx.services.state,
                                                  &event,now,false,&effects,&core);
        if(result==STORE_OK&&!root())ctx.coordinator_device=f.sender;
        if(result==STORE_OK&&ctx.shared_waiting&&ctx.shared_acked&&
           recent(*ctx.services.state,ctx.shared_event.id)){
            const EventId id=ctx.shared_event.id;ctx.shared_waiting=false;
            if(ctx.services.shared_result)
                ctx.services.shared_result(ctx.services.context,id,true);
        }
        return;
    }
}
esp_err_t raw_received(uint8_t *data,uint32_t len,uint8_t **out_data,
                       uint32_t *out_len,uint32_t seq){
    (void)seq;
    if(out_data)*out_data=nullptr;
    if(out_len)*out_len=0;
    if(!ctx.active||!ctx.queue||!data||len>KATMED_MESH_WIRE_MAX||
       len<KATMED_MESH_WIRE_HEADER+KATMED_MESH_WIRE_TAG)return ESP_OK;
    Packet packet{};packet.length=uint16_t(len);std::memcpy(packet.bytes,data,len);
    if(xQueueSend(ctx.queue,&packet,0)!=pdTRUE)
        saturating_increment(ctx.diagnostics.rx_queue_drops);
    return ESP_OK;
}
void ip_event(void *,esp_event_base_t base,int32_t id,void *){
    if(base==IP_EVENT){
        if(id==IP_EVENT_STA_GOT_IP){
            if(!ctx.has_ip)
                saturating_increment(ctx.diagnostics.ip_got_transitions);
            ctx.has_ip=true;
        }
        if(id==IP_EVENT_STA_LOST_IP){
            if(ctx.has_ip)
                saturating_increment(ctx.diagnostics.ip_lost_transitions);
            ctx.has_ip=false;
        }
    }
}
void hex(char *out,const uint8_t *bytes,size_t n){
    const char alphabet[]="0123456789abcdef";
    for(size_t i=0;i<n;++i){out[2*i]=alphabet[bytes[i]>>4];out[2*i+1]=alphabet[bytes[i]&15];}
    out[2*n]=0;
}
}

extern "C" KatmedMeshResult katmed_mesh_start(const StoreProvision *provision,
                                                const KatmedMeshServices *services){
    if(ctx.started||!provision||!services||!services->store||!services->state||
       !provision->configured||store_get_status(services->store,nullptr,nullptr,nullptr)!=STORE_OK||
       std::memcmp(provision->group,services->store->record.provision.group,16)!=0||
       std::memcmp(provision->secret,services->store->record.provision.secret,32)!=0)
        return MESH_UNAVAILABLE;
    ctx.services=*services;ctx.provision=*provision;
    ctx.status=MeshStatusState{};
    mesh_history_open(&history,provision->group,services->store->scratch);
    if(mesh_history_add(&history,services->store->record.device))history.self_initial=services->store->record.device;
    mesh_history_commit(&history,services->store->scratch);
    uint8_t nonce[16];esp_fill_random(nonce,sizeof(nonce));
    if(!katmed_mesh_sessions_init(&ctx.sessions,nonce))return MESH_UNAVAILABLE;
    ctx.queue=xQueueCreate(kRxQueue,sizeof(Packet));
    if(!ctx.queue)return MESH_UNAVAILABLE;
    if(esp_netif_init()!=ESP_OK)return MESH_UNAVAILABLE;
    const esp_err_t loop=esp_event_loop_create_default();
    if(loop!=ESP_OK&&loop!=ESP_ERR_INVALID_STATE)return MESH_UNAVAILABLE;
    esp_bridge_create_all_netif();
    katmed_dhcp_diag_bind_sta();
    esp_mesh_lite_config_t config=ESP_MESH_LITE_DEFAULT_INIT();
    config.mesh_id=provision->group[0]?provision->group[0]:1;
    config.vendor_id[0]=provision->group[1];config.vendor_id[1]=provision->group[2];
    config.max_node_number=8;config.join_mesh_ignore_router_status=true;
    config.join_mesh_without_configured_wifi=true;config.leaf_node=false;
    ctx.ap_ssid[0]='K';ctx.ap_ssid[1]='M';
    hex(ctx.ap_ssid+2,provision->group,15);
    hex(ctx.ap_password,provision->secret,31);
    config.softap_ssid=ctx.ap_ssid;config.softap_password=ctx.ap_password;
    esp_mesh_lite_init(&config);
    (void)esp_mesh_lite_set_argot((uint32_t(provision->group[3])<<24)|
        (uint32_t(provision->group[4])<<16)|(uint32_t(provision->group[5])<<8)|
        provision->group[6]);
    (void)esp_mesh_lite_set_softap_info(ctx.ap_ssid,ctx.ap_password);
    if(provision->ssid_length){
        mesh_lite_sta_config_t router{};
        std::memcpy(router.ssid,provision->ssid,provision->ssid_length);
        std::memcpy(router.password,provision->password,provision->password_length);
        if(esp_mesh_lite_set_router_config(&router)!=ESP_OK)return MESH_UNAVAILABLE;
    }
    static const esp_mesh_lite_raw_msg_action_t actions[]={
        {kMessageId,0,raw_received},{0,0,nullptr}
    };
    if(esp_mesh_lite_raw_msg_action_list_register(actions)!=ESP_OK)return MESH_UNAVAILABLE;
    if(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,ip_event,nullptr)!=ESP_OK||
       esp_event_handler_register(IP_EVENT,IP_EVENT_STA_LOST_IP,ip_event,nullptr)!=ESP_OK)
        return MESH_UNAVAILABLE;
    ctx.started=true;ctx.active=true;ctx.dirty=true;
    esp_mesh_lite_start();
    return MESH_ACCEPTED;
}
extern "C" void katmed_mesh_stop(void){
    if(!ctx.started)return;
    interaction.bind(ctx.services.store?ctx.services.store->record.device:0,0,0,false,false,ctx.status.now_ms);greeting_id=0;greeting_until=0;
    ctx.active=false;ctx.has_ip=false;ctx.shared_waiting=false;
    ctx.coordinator_device=0;ctx.remaining_ids=0;
    ctx.sessions=KatmedMeshSessions{};
    note_session_transition();
    if(ctx.queue)xQueueReset(ctx.queue);
    (void)esp_mesh_lite_disconnect();
}
extern "C" bool katmed_mesh_radio_channel(uint8_t *channel){
    if(!channel)return false;
    *channel=0;
    if(!current_group())return false;
    uint8_t primary=0;
    wifi_second_chan_t secondary;
    if(esp_wifi_get_channel(&primary,&secondary)==ESP_OK&&primary>=1&&primary<=14)
        *channel=primary;
    return true;
}
extern "C" void katmed_mesh_history_backend(StoreBackend backend){history.backend=backend;}
extern "C" void katmed_mesh_counts(KatmedMeshCounts *out){
    if(!out)return;
    *out=KatmedMeshCounts{};
    if(!current_group())return;
    out->active=true;out->history=history.count;out->quality=status_quality();
    if(!linked()){out->quality|=MS_REBUILD;return;}
    if(root()){
        root_current();out->current=ctx.status.current_count;
        out->current_valid=ctx.status.clock_valid;
    }else{
        const int i=status_peer(ctx.coordinator_device);
        out->current=ctx.status.current_count;
        out->current_valid=i>=0&&fresh_hello(unsigned(i))&&ctx.status.current_valid&&
            mesh_status_fresh(ctx.status.now_ms,ctx.status.peers[i].status_ms,ctx.status.peers[i].status_valid);
    }
    out->history_complete=(out->quality&uint8_t(MS_SATURATED|MS_STORE_ERROR|MS_REBUILD|MS_CAPACITY))==0;
}
extern "C" bool katmed_mesh_connected(void){return linked()&&session_live();}
extern "C" bool katmed_mesh_is_coordinator(void){return current_group()&&root();}
extern "C" bool katmed_mesh_root_has_ip(void){return current_group()&&root()&&ctx.has_ip;}
extern "C" bool katmed_mesh_diagnostics(KatmedMeshDiagnostics *out){
    if(!out||!ctx.started)return false;
    *out={ctx.diagnostics.sdk_send_failures.load(std::memory_order_relaxed),
          ctx.diagnostics.rx_queue_drops.load(std::memory_order_relaxed),
          ctx.diagnostics.observed_level_changes.load(std::memory_order_relaxed),
          ctx.diagnostics.ip_got_transitions.load(std::memory_order_relaxed),
          ctx.diagnostics.ip_lost_transitions.load(std::memory_order_relaxed),
          ctx.diagnostics.authenticated_session_up.load(std::memory_order_relaxed),
          ctx.diagnostics.authenticated_session_down.load(std::memory_order_relaxed),
          0,0,0,0};
    esp_netif_t *station=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    esp_netif_dhcp_status_t dhcp;
    if(station && esp_netif_get_ip_info(station,&ip)==ESP_OK &&
       esp_netif_dhcpc_get_status(station,&dhcp)==ESP_OK){
        uint32_t status;
        switch(dhcp){
        case ESP_NETIF_DHCP_INIT: status=0;break;
        case ESP_NETIF_DHCP_STARTED: status=1;break;
        case ESP_NETIF_DHCP_STOPPED: status=2;break;
        default:return true;
        }
        out->station_valid=1;
        out->station_up=esp_netif_is_netif_up(station);
        out->station_ipv4_nonzero=ip.ip.addr!=0;
        out->station_dhcp_status=status;
    }
    return true;
}
extern "C" void katmed_mesh_notify_local_commit(void){
    if(current_group()){ctx.dirty=true;ctx.pending_urgent=true;}
}
extern "C" KatmedMeshResult katmed_mesh_submit_shared(const AppEvent *event,
                                                        const ClockSample *now,
                                                        AppEffects *effects){
    if(effects)*effects=AppEffects{};
    if(!event||!now||!linked()||!session_live())return MESH_UNAVAILABLE;
    if(root()){
        AppEvent accepted=*event;accepted.coordinator_accepted=true;
        AppResult core=APP_INVALID;
        const StoreResult result=store_apply_event(ctx.services.store,ctx.services.state,
                                                  &accepted,now,false,effects,&core);
        if(result==STORE_OK){ctx.dirty=true;return MESH_ACCEPTED;}
        return result==STORE_UNAVAILABLE||result==STORE_IO||result==STORE_COMMIT_UNKNOWN
            ? MESH_STORE_ERROR:MESH_REJECTED;
    }
    if(ctx.shared_waiting)return MESH_UNAVAILABLE;
    auto f=frame_for(MESH_SET_SHARED_SETTINGS,event->id);
    f.parent_revision=event->parent_revision;
    if(!encode_shared(*event,f)||!send(f))return MESH_UNAVAILABLE;
    ctx.shared_event=*event;ctx.shared_waiting=true;ctx.shared_acked=false;
    ctx.shared_since=now->monotonic_ms;
    return MESH_PENDING;
}
extern "C" void katmed_mesh_poll(const ClockSample *now){
    if(!now||!current_group())return;
    if(!ctx.status.clock_valid){ctx.status.clock_valid=true;ctx.status.started_ms=now->monotonic_ms;}
    ctx.status.now_ms=now->monotonic_ms;
    const uint32_t drops=ctx.diagnostics.rx_queue_drops.load(std::memory_order_relaxed);
    const uint32_t sends=ctx.diagnostics.sdk_send_failures.load(std::memory_order_relaxed);
    if(drops!=ctx.status.observed_drops||sends!=ctx.status.observed_sends)
        ctx.status.warning_until_ms=now->monotonic_ms+MESH_STATUS_FRESH_MS;
    ctx.status.observed_drops=drops;ctx.status.observed_sends=sends;
    const uint8_t level=esp_mesh_lite_get_level();
    if(level!=ctx.last_level){
        ctx.last_level=level;
        saturating_increment(ctx.diagnostics.observed_level_changes);
        if(!rekey()){
            katmed_mesh_stop();return;
        }
    }
    for(unsigned i=0;i<kRxQueue;++i){
        if(xQueueReceive(ctx.queue,&ctx.receive,0)!=pdTRUE)break;
        KatmedMeshFrame frame{};
        if(katmed_mesh_wire_decode(ctx.receive.bytes,ctx.receive.length,
                                   ctx.provision.group,ctx.provision.secret,&frame))
            receive(frame,now);
    }
    /* The serial main loop owns both stores; never write from raw_receive. */
    mesh_history_commit(&history,ctx.services.store->scratch);
    if(root()&&ctx.status.clock_valid&&now->monotonic_ms-ctx.status.started_ms>=MESH_STATUS_FRESH_MS&&!history.error&&
       !(status_quality()&uint8_t(MS_STORE_ERROR|MS_REBUILD|MS_CAPACITY|MS_SATURATED))){
        for(unsigned i=0;i<KATMED_MESH_PEERS;++i)if(pending_admission[i]&&admission_verified[i]){(void)mesh_history_admit(&history,pending_admission[i]);pending_admission[i]=0;admission_verified[i]=false;}
        mesh_history_commit(&history,ctx.services.store->scratch);
    }
    uint64_t new_member=0;
    if(mesh_history_unprocessed(&history,&new_member)&&mesh_history_processed(&history,new_member)){
        mesh_history_commit(&history,ctx.services.store->scratch);
        if(!history.error&&!history.dirty){if(new_member==history.self_initial)history.self_initial=0;greeting_id=new_member;greeting_until=now->monotonic_ms+1800000;}
    }
    bind_interaction(now->monotonic_ms);
    uint64_t destination=0;katmed::InteractionMessage message;
    if(interaction.transmit(now->monotonic_ms,destination,message)){
        if(message.op==1&&!history.error&&!history.read_failed&&history.self_initial==ctx.services.store->record.device)message.flags|=8;
        if(!interaction_id.device)(void)next_event_id(interaction_id);
        if(interaction_id.device){auto frame=frame_for(MESH_INTERACTION,interaction_id,destination);
            frame.payload_length=uint16_t(katmed::interaction_encode(message,frame.payload));
            if(frame.payload_length)(void)send_one(frame);}
    }
    reconcile_candidates(now);
    if(ctx.shared_waiting&&now->monotonic_ms-ctx.shared_since>=kSharedTimeoutMs){
        EventId id=ctx.shared_event.id;ctx.shared_waiting=false;
        if(ctx.services.shared_result)
            ctx.services.shared_result(ctx.services.context,id,false);
    }
    if(!linked())return;
    reconcile_own_pending();
    send_status();
    if(ctx.dirty||now->monotonic_ms-ctx.last_state>=kStateIntervalMs){
        send_hello();
        send_state(now);
        if(root())send_time(now);
        ctx.last_state=now->monotonic_ms;ctx.dirty=false;
    }
    if(ctx.pending_urgent||now->monotonic_ms-ctx.last_pending>=kPendingIntervalMs){
        send_pending(now);ctx.last_pending=now->monotonic_ms;
        ctx.pending_urgent=false;
        if(ctx.shared_waiting&&!root()){
            auto f=frame_for(MESH_SET_SHARED_SETTINGS,ctx.shared_event.id);
            f.parent_revision=ctx.shared_event.parent_revision;
            if(encode_shared(ctx.shared_event,f))(void)send(f);
        }
    }
}

extern "C" bool katmed_mesh_interaction_input(const KatmedInteractionInput *in,uint64_t now){
    if(!in)return false;
    if(in->kind==KATMED_INTERACTION_DISMISS_GREETING){
        if(in->target_id!=greeting_id||!greeting_id)return false;
        greeting_until=0;return true;
    }
    bind_interaction(now);
    return interaction.input(*in,now);
}
extern "C" void katmed_mesh_interaction_status(KatmedInteractionStatus *out,uint64_t now){
    if(!out)return;
    bind_interaction(now);interaction.status(*out,now);
    out->greeting_id=greeting_id;out->greeting_until_ms=greeting_until;
    out->greeting_visible=greeting_id&&now<greeting_until&&!out->alarm_active&&current_group();
    out->known_devices=history.count;
}
