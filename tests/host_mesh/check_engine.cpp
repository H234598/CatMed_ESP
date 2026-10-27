#include "katmed_mesh.h"
#include "katmed_night_wire.h"
#include "../../firmware/main/display_ui_mailbox.h"
#include <atomic>
#include <thread>
#include "katmed_mesh_names.h"
#include "katmed_names_wire.h"
#include "../host_names/memory_backend.hpp"
#include "katmed_mesh_payload.h"
#include "katmed_mesh_status.h"
#include "katmed_mesh_interaction.hpp"
#include "mesh_sdk_stubs.h"
#include <cassert>
#include <cstring>
#include <string>
#include <deque>
#include <vector>
#include <fstream>
#include <cstdio>
#include <zlib.h>
extern "C" unsigned char *SHA256(const unsigned char *,size_t,unsigned char *);

namespace {
uint8_t level=0;
raw_msg_process_cb_t receiver=nullptr;
unsigned dropped=0,boot_random=0,failed_sends=0;
bool fail_send=false;
void (*night_interleave_hook)()=nullptr;
unsigned router_configs=0, mesh_starts=0;
bool startup_queue_failure=false, startup_netif_failure=false;
bool startup_platform_done=false, startup_bridge_done=false, startup_mesh_init_done=false;
std::vector<std::string> startup_samples;
void startup_heap_sample(const char *stage) {
    assert(startup_platform_done);
    if (!std::strcmp(stage,"mesh_after_platform"))
        assert(!startup_bridge_done && !startup_mesh_init_done && startup_samples.empty());
    else if (!std::strcmp(stage,"mesh_after_bridge"))
        assert(startup_bridge_done && !startup_mesh_init_done && startup_samples.size()==1);
    else {
        assert(!std::strcmp(stage,"mesh_after_init"));
        assert(startup_bridge_done && startup_mesh_init_done && startup_samples.size()==2);
    }
    assert(mesh_starts==0); /* The final existing Main checkpoint follows start. */
    startup_samples.emplace_back(stage);
}
bool share_time=true;
bool time_shareable(void *) {return share_time;}
uint8_t current_local_nonce[16]{};
unsigned shared_results=0;bool shared_accepted=false;EventId shared_result_id{};
KatmedMeshSharedOutcome shared_outcome=MESH_SHARED_TIMEOUT;
unsigned utc_calls=0;ClockQuality last_quality=CLOCK_UNAVAILABLE;
uint64_t last_observed_mono=0;int64_t last_received_utc=0;
std::vector<std::vector<uint8_t>> sent;
std::vector<uint8_t> old_state_bytes,old_confirm_bytes;
struct HostQueue {unsigned capacity;size_t size;std::deque<std::vector<uint8_t>> values;};
struct Memory {std::vector<uint8_t> blob;bool fail=false;unsigned commits=0;} memory;
Memory history_memory;
Store store{};AppState state{};StoreProvision provision{};
ClockSample now{1000,7,1760460000000LL,true,CLOCK_SYNCHRONIZED};
void shared_result(void *,EventId id,KatmedMeshSharedOutcome outcome){
    ++shared_results;shared_accepted=outcome==MESH_SHARED_ACCEPTED;
    shared_outcome=outcome;shared_result_id=id;
}
void utc_received(void *,uint64_t observed_mono,int64_t utc,ClockQuality quality){
    ++utc_calls;last_observed_mono=observed_mono;
    last_received_utc=utc;last_quality=quality;
}
void advance_now(uint64_t mono){
    assert(mono>=now.monotonic_ms);
    now.utc_ms+=int64_t(mono-now.monotonic_ms);
    now.monotonic_ms=mono;
}
void prepare_due_round(){
    AppEffects effects{};
    assert(app_dismiss_boot_alarm(&state,&now,&effects)==APP_OK);
    assert(!effects.persistence_required&&!effects.broadcast_required&&
           !state.has_last_confirmation);
    EventId id{};assert(store_next_event_id(&store,&id)==STORE_OK);
    AppEvent event{};event.kind=EVENT_SET_MEDICATION_INTERVAL;
    event.id=id;event.sender=id.device;event.round=state.round;
    event.parent_revision=state.revision;event.occurred_at=now;
    event.coordinator_accepted=true;event.value.interval_seconds=1;
    AppResult core=APP_INVALID;
    assert(store_apply_event(&store,&state,&event,&now,false,&effects,&core)==STORE_OK);
    advance_now(now.monotonic_ms+1000);
    assert(app_tick(&state,&now,&effects)==APP_OK&&state.alarm_active&&
           state.start_reason==START_SETTINGS&&!state.has_last_confirmation);
}

StoreResult read_blob(void *arg,uint8_t *out,size_t *n){
    auto &m=*static_cast<Memory *>(arg);
    if(m.blob.empty())return STORE_NOT_FOUND;
    if(*n<m.blob.size())return STORE_FULL;
    std::memcpy(out,m.blob.data(),m.blob.size());*n=m.blob.size();return STORE_OK;
}
StoreResult write_blob(void *arg,const uint8_t *in,size_t n){
    auto &m=*static_cast<Memory *>(arg);
    if(night_interleave_hook)night_interleave_hook();
    if(m.fail)return STORE_COMMIT_UNKNOWN;
    m.blob.assign(in,in+n);++m.commits;return STORE_OK;
}
void inject(const KatmedMeshFrame &f){
    uint8_t bytes[KATMED_MESH_WIRE_MAX];size_t n=0;
    assert(katmed_mesh_wire_encode(&f,provision.secret,bytes,sizeof(bytes),&n));
    uint8_t *response=nullptr;uint32_t out=0;
    assert(receiver(bytes,uint32_t(n),&response,&out,0)==ESP_OK);
    assert(response==nullptr&&out==0);
}
void inject_bytes(const std::vector<uint8_t> &bytes){
    uint8_t *response=nullptr;uint32_t out=0;
    assert(receiver(const_cast<uint8_t *>(bytes.data()),uint32_t(bytes.size()),
        &response,&out,0)==ESP_OK);
    assert(response==nullptr&&out==0);
}
void write_fixture(const char *path){
    std::ofstream stream(path,std::ios::binary|std::ios::trunc);
    assert(stream.good());
    for(const auto *item:{&memory.blob,&old_state_bytes,&old_confirm_bytes}){
        const uint32_t n=uint32_t(item->size());
        stream.write(reinterpret_cast<const char *>(&n),sizeof(n));
        stream.write(reinterpret_cast<const char *>(item->data()),n);
    }
    stream.flush();assert(stream.good());
}
void read_fixture(const char *path){
    std::ifstream stream(path,std::ios::binary);assert(stream.good());
    for(auto *item:{&memory.blob,&old_state_bytes,&old_confirm_bytes}){
        uint32_t n=0;stream.read(reinterpret_cast<char *>(&n),sizeof(n));
        assert(stream.good()&&n>0&&n<10000);
        item->resize(n);
        stream.read(reinterpret_cast<char *>(item->data()),n);
        assert(stream.good());
    }
}
KatmedMeshFrame peer_frame(KatmedMeshKind kind,uint64_t counter){
    KatmedMeshFrame f{};f.kind=kind;
    std::memcpy(f.group,provision.group,16);
    f.id={99,5,counter};f.sender=99;f.parent_revision=state.revision;
    for(unsigned i=0;i<16;++i)f.session_nonce[i]=uint8_t(i+40);
    if(kind!=MESH_HELLO)std::memcpy(f.receiver_nonce,current_local_nonce,16);
    f.session_counter=counter;return f;
}
unsigned count_kind(KatmedMeshKind kind){
    unsigned total=0;
    for(const auto &s:sent){KatmedMeshFrame f{};
        if(katmed_mesh_wire_decode(s.data(),s.size(),provision.group,provision.secret,&f)&&
           f.kind==kind)++total;
    }
    return total;
}
KatmedMeshFrame latest_kind(KatmedMeshKind kind){
    for(size_t i=sent.size();i>0;--i){
        KatmedMeshFrame frame{};
        if(katmed_mesh_wire_decode(sent[i-1].data(),sent[i-1].size(),
            provision.group,provision.secret,&frame)&&frame.kind==kind)return frame;
    }
    assert(false);return {};
}
}

esp_err_t esp_event_loop_create_default(){startup_platform_done=true;return ESP_OK;}
esp_err_t esp_event_handler_register(esp_event_base_t,int32_t,esp_event_handler_t,void *){return ESP_OK;}
esp_err_t esp_netif_init(){return startup_netif_failure?ESP_FAIL:ESP_OK;}
void esp_bridge_create_all_netif(){startup_bridge_done=true;}
/* Standalone Mesh harness does not link the separate diagnostic component. */
extern "C" void katmed_dhcp_diag_bind_sta(){}
void esp_fill_random(void *out,size_t n){auto *p=static_cast<uint8_t *>(out);for(size_t i=0;i<n;++i)p[i]=uint8_t(1+boot_random+i);++boot_random;}
const esp_app_desc_t *esp_app_get_description(){static esp_app_desc_t app{{'d','9','4','1','f','a','e',0}};return &app;}
void esp_mesh_lite_init(esp_mesh_lite_config_t *){startup_mesh_init_done=true;}
void esp_mesh_lite_start(){++mesh_starts;}
esp_err_t esp_mesh_lite_connect(){return ESP_OK;}
uint8_t esp_mesh_lite_get_level(){return level;}
esp_err_t esp_mesh_lite_disconnect(){level=0;return ESP_OK;}
esp_err_t esp_mesh_lite_set_argot(uint32_t){return ESP_OK;}
esp_err_t esp_mesh_lite_set_softap_info(const char *ssid,const char *){assert(std::strlen(ssid)==23 && !std::strcmp(ssid,"Katmed_ESP_optout_nomap"));return ESP_OK;}
esp_err_t esp_mesh_lite_set_router_config(mesh_lite_sta_config_t *){++router_configs;return ESP_OK;}
esp_err_t esp_mesh_lite_raw_msg_action_list_register(const esp_mesh_lite_raw_msg_action_t *actions){receiver=actions[0].raw_process;return ESP_OK;}
esp_err_t esp_mesh_lite_send_msg(int,esp_mesh_lite_msg_config_t *request){
    if(night_interleave_hook)night_interleave_hook();
    if(fail_send){++failed_sends;return ESP_FAIL;}
    sent.emplace_back(request->raw_msg.data,request->raw_msg.data+request->raw_msg.size);
    return ESP_OK;
}
esp_err_t esp_mesh_lite_send_broadcast_raw_msg_to_child(const uint8_t *,size_t){return ESP_OK;}
esp_err_t esp_mesh_lite_send_raw_msg_to_root(const uint8_t *,size_t){return ESP_OK;}
QueueHandle_t xQueueCreate(unsigned capacity,size_t size){return startup_queue_failure?nullptr:new HostQueue{capacity,size,{}};}
int xQueueSend(QueueHandle_t q,const void *data,unsigned){
    auto &queue=*static_cast<HostQueue *>(q);
    if(queue.values.size()>=queue.capacity){++dropped;return 0;}
    const auto *p=static_cast<const uint8_t *>(data);
    queue.values.emplace_back(p,p+queue.size);return pdTRUE;
}
int xQueueReceive(QueueHandle_t q,void *data,unsigned){
    auto &queue=*static_cast<HostQueue *>(q);
    if(queue.values.empty())return 0;
    std::memcpy(data,queue.values.front().data(),queue.size);
    queue.values.pop_front();return pdTRUE;
}
int xQueueReset(QueueHandle_t q){static_cast<HostQueue *>(q)->values.clear();return pdTRUE;}

int main(int argc,char **argv){
    uint8_t catalog[32]{};catalog[0]=43;
    if(argc==3&&std::strcmp(argv[1],"cold_consume")==0){
        read_fixture(argv[2]);boot_random=50;
        now.monotonic_ms=2000;now.domain=77;now.utc_ms+=1000;
    }
    StoreRecovery recovery{};
    StoreBackend backend{&memory,read_blob,write_blob};
    assert(store_open(&store,backend,11,catalog,75,&recovery)==STORE_OK);
    for(unsigned i=0;i<16;++i)provision.group[i]=uint8_t(i+1);
    for(unsigned i=0;i<32;++i)provision.secret[i]=uint8_t(i+30);
    std::memcpy(provision.ssid,"testssid",8);provision.ssid_length=8;
    std::memcpy(provision.password,"password",8);provision.password_length=8;
    std::memcpy(provision.time_source,"pool.ntp.org",12);provision.time_source_length=12;
    provision.configured=true;store.record.provision=provision;
    std::memcpy(store.record.bound_group,provision.group,16);
    store.record.group_origin=STORE_GROUP_ORIGIN_FIRST_BOUND;
    assert(app_init(&state,&recovery.shared,&now,recovery.boot_identity,75,catalog)==APP_OK);
    katmed_mesh_history_backend(StoreBackend{&history_memory,read_blob,write_blob});
    if(argc==2&&std::strcmp(argv[1],"status_fair")==0){
        MeshHistory seed{};seed.generation=1;std::memcpy(seed.group,provision.group,16);
        mesh_history_add(&seed,11);for(uint64_t id=100;id<355;++id)mesh_history_add(&seed,id);
        uint8_t bytes[MESH_HISTORY_BLOB];assert(mesh_history_encode(&seed,bytes));
        history_memory.blob.assign(bytes,bytes+sizeof(bytes));
    }
    if(argc==2&&std::strcmp(argv[1],"interaction_previous")==0){
        MeshHistory seed{};seed.generation=1;std::memcpy(seed.group,provision.group,16);
        mesh_history_add(&seed,11);mesh_history_add(&seed,88);
        uint8_t bytes[MESH_HISTORY_BLOB];assert(mesh_history_encode(&seed,bytes));history_memory.blob.assign(bytes,bytes+sizeof(bytes));
    }
    const bool mesh_only=argc==2 && !std::strcmp(argv[1],"mesh_only");
    const bool offline=argc==2 && !std::strcmp(argv[1],"offline");
    if(mesh_only || offline) {
        assert(store_save_setup(&store,&provision)==STORE_OK);
        provision.network_mode=mesh_only ? STORE_NETWORK_MESH_ONLY : STORE_NETWORK_OFFLINE;
        std::memset(provision.ssid,0,sizeof(provision.ssid));provision.ssid_length=0;
        std::memset(provision.password,0,sizeof(provision.password));provision.password_length=0;
        std::memset(provision.time_source,0,sizeof(provision.time_source));provision.time_source_length=0;
        assert(store_save_setup(&store,&provision)==STORE_OK && store.record.wifi_profile_count==1);
    }
    KatmedMeshServices services{&store,&state,nullptr,shared_result,utc_received,time_shareable,nullptr};
    const bool startup_case=argc==2 && !std::strncmp(argv[1],"startup_",8);
    if(startup_case) {
        services.startup_heap_sample=startup_heap_sample;
        startup_queue_failure=!std::strcmp(argv[1],"startup_queue_failure");
        startup_netif_failure=!std::strcmp(argv[1],"startup_netif_failure");
        if(startup_queue_failure || startup_netif_failure) {
            assert(katmed_mesh_start(&provision,&services)==MESH_UNAVAILABLE);
            assert(startup_samples.empty() && !startup_platform_done && !startup_bridge_done && !startup_mesh_init_done);
            assert(mesh_starts==0);return 0;
        }
    }
    if(offline) {assert(katmed_mesh_start(&provision,&services)==MESH_UNAVAILABLE && !mesh_starts && !router_configs);return 0;}
    assert(katmed_mesh_start(&provision,&services)==MESH_ACCEPTED);
    if(startup_case) {
        assert(startup_samples==std::vector<std::string>({"mesh_after_platform","mesh_after_bridge","mesh_after_init"}));
        assert(mesh_starts==1);
        assert(katmed_mesh_start(&provision,&services)==MESH_UNAVAILABLE);
        assert(startup_samples.size()==3 && mesh_starts==1);return 0;
    }
    assert(startup_samples.empty()); /* Optional callback absent on existing paths. */
    if(mesh_only) {assert(mesh_starts==1 && router_configs==0);return 0;}
    if(argc==2&&!std::strcmp(argv[1],"names_hostname_retry_netif")){
        NamesMemory bytes;
        assert(katmed_mesh_names_backend(bytes.backend(),11,provision.group)==NAMES_OK);
        host_station_present=host_ap_present=true;level=ROOT;
        host_hostname_failures_remaining=100;
        katmed_mesh_poll(&now);advance_now(now.monotonic_ms+1);
        const unsigned failed=host_hostname_set_calls;
        assert(failed>0&&!host_hostname[0]&&!host_ap.hostname[0]);
        host_hostname_failures_remaining=0;
        for(unsigned i=0;i<8;++i){katmed_mesh_poll(&now);advance_now(now.monotonic_ms+1);}
        char value[32];bool assigned=false,uncertain=true;
        katmed_mesh_device_name(value,sizeof(value),&assigned,&uncertain);
        assert(assigned&&!uncertain&&!std::strcmp(value,"KatmedESP-001"));
        assert(!std::strcmp(host_hostname,value)&&!std::strcmp(host_ap.hostname,value));
        assert(host_hostname_set_successes==2&&host_hostname_set_calls==failed+2);
        const unsigned stable=host_hostname_set_calls;
        for(unsigned i=0;i<5;++i){katmed_mesh_poll(&now);advance_now(now.monotonic_ms+1);}
        assert(host_hostname_set_calls==stable);
        host_station_handle=&host_replacement;
        katmed_mesh_poll(&now);
        assert(!std::strcmp(host_replacement.hostname,value)&&
               host_hostname_set_calls==stable+1);
        const unsigned changed=host_hostname_set_calls;
        katmed_mesh_poll(&now);assert(host_hostname_set_calls==changed);
        std::puts("SDK setter retries after failure, applies once to replacement netif and skips unchanged STA/AP names OK");
        return 0;
    }
    #include "check_names_barrier.inc"
    #include "check_names_leaf.inc"
    if(argc==2 && !std::strcmp(argv[1],"night_interleave")) {
        level=ROOT;katmed_mesh_poll(&now);
        auto challenge=latest_kind(MESH_HELLO);std::memcpy(current_local_nonce,challenge.session_nonce,16);
        auto hello=peer_frame(MESH_HELLO,1);
        assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,hello.payload,sizeof(hello.payload),&hello.payload_length));
        inject(hello);katmed_mesh_poll(&now);assert(katmed_mesh_connected());
        static DisplayUiMailbox box=DISPLAY_MB_INITIALIZER;
        static std::atomic<unsigned> stage{0},done{0};static std::atomic<bool> quit{false};
        static unsigned observations=0;
        display_mb_publish(&box,katmed_display_defaults(),KatmedNightCopyStatus{});
        std::thread ui([&]{
            while(!quit.load(std::memory_order_acquire)){
                unsigned n=stage.load(std::memory_order_acquire);
                if(n>done.load(std::memory_order_relaxed)){
                    KatmedDisplaySettings settings;KatmedNightCopyStatus status;
                    display_mb_snapshot(&box,&settings,&status);
                    assert(katmed_display_valid(settings));
                    assert(status.acknowledged+status.rejected<=status.expected && !(status.active&&status.finished));
                    UiRequest second{};second.kind=UI_REQUEST_COPY_NIGHT;
                    assert(!display_mb_submit(&box,&second)); // processing/result belongs to original action
                    ++observations;done.store(n,std::memory_order_release);
                }else std::this_thread::yield();
            }
        });
        night_interleave_hook=[](){unsigned n=stage.fetch_add(1,std::memory_order_release)+1;
            while(done.load(std::memory_order_acquire)<n)std::this_thread::yield();};
        UiRequest intent{};intent.kind=UI_REQUEST_COPY_NIGHT;uint32_t first_sequence=0;
        for(unsigned operation=0;operation<2;++operation){
            assert(display_mb_submit(&box,&intent));UiRequest consumed{};uint32_t sequence;
            assert(display_mb_take(&box,&consumed,&sequence) && consumed.kind==UI_REQUEST_COPY_NIGHT);
            if(!operation)first_sequence=sequence;
            else assert(!display_mb_complete(&box,first_sequence,UI_SUBMIT_COMMITTED));
            assert(katmed_mesh_copy_night(now.monotonic_ms)==MESH_ACCEPTED);
            assert(display_mb_complete(&box,sequence,UI_SUBMIT_COMMITTED));
            katmed_mesh_poll(&now);auto apply=latest_kind(MESH_NIGHT);KatmedNightMessage m{};
            assert(katmed_night_decode(apply.payload,apply.payload_length,&m));
            if(!operation){
                auto ack=peer_frame(MESH_NIGHT,2);m.op=3;m.expected=1;m.acknowledged=1;m.finished=true;
                EventId correct=m.id;++m.id.sequence;assert(katmed_night_encode(&m,ack.payload));ack.payload_length=KATMED_NIGHT_BYTES;
                inject(ack);katmed_mesh_poll(&now);KatmedNightCopyStatus status{};katmed_mesh_night_status(&status);assert(!status.acknowledged);
                m.id=correct;ack.session_counter=3;assert(katmed_night_encode(&m,ack.payload));inject(ack);katmed_mesh_poll(&now);
                katmed_mesh_night_status(&status);assert(status.finished && status.acknowledged==1 && status.expected==1);
            }else{level=2;katmed_mesh_poll(&now);}
            KatmedNightCopyStatus status{};katmed_mesh_night_status(&status);
            if(operation)assert(status.finished && !status.active && !status.membership_known && !status.acknowledged);
            display_mb_publish(&box,store.record.display,status);
            night_interleave_hook();UiSubmitResult result;UiRequest completed;
            assert(display_mb_result(&box,&completed,&result) && result==UI_SUBMIT_COMMITTED && completed.kind==UI_REQUEST_COPY_NIGHT);
            assert(!display_mb_complete(&box,sequence,UI_SUBMIT_REJECTED));
        }
        night_interleave_hook=nullptr;quit.store(true,std::memory_order_release);ui.join();assert(observations>=4);
        std::puts("N-M1 actual two-thread Store/TX interleavings: coherent getter, busy/lifetime/stale completion, wrong/right ACK ID and rekey cancellation PASS");return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"night_solo")) {
        level=ROOT;katmed_mesh_poll(&now);
        assert(katmed_mesh_copy_night(now.monotonic_ms)==MESH_ACCEPTED);
        KatmedNightCopyStatus status{};katmed_mesh_night_status(&status);
        assert(status.local_saved && status.finished && !status.active && !status.expected && !status.acknowledged);
        std::puts("night solo: local save without imaginary remote acknowledgements PASS");return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"night_copy")) {
        level=ROOT;katmed_mesh_poll(&now);
        auto challenge=latest_kind(MESH_HELLO);
        std::memcpy(current_local_nonce,challenge.session_nonce,16);
        auto hello=peer_frame(MESH_HELLO,1);
        assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,hello.payload,sizeof(hello.payload),&hello.payload_length));
        inject(hello);katmed_mesh_poll(&now);assert(katmed_mesh_connected());
        auto settings=katmed_display_defaults();settings.brightness=25;settings.night_enabled=true;
        assert(store_set_display(&store,&settings)==STORE_OK);
        const auto medical=state.revision;
        assert(katmed_mesh_copy_night(now.monotonic_ms)==MESH_ACCEPTED);
        KatmedNightCopyStatus status{};katmed_mesh_night_status(&status);
        assert(status.local_saved && status.active && status.expected==1 && status.acknowledged==0);
        const unsigned active_commits=memory.commits;
        assert(katmed_mesh_copy_night(now.monotonic_ms)==MESH_UNAVAILABLE && memory.commits==active_commits);
        katmed_mesh_poll(&now);
        auto apply=latest_kind(MESH_NIGHT);KatmedNightMessage message{};
        assert(katmed_night_decode(apply.payload,apply.payload_length,&message) && message.op==2);
        assert(message.settings.brightness==100); // brightness is deliberately absent from wire
        auto ack=peer_frame(MESH_NIGHT,2);message.op=3;message.expected=1;message.acknowledged=1;message.finished=true;
        assert(katmed_night_encode(&message,ack.payload));ack.payload_length=KATMED_NIGHT_BYTES;
        auto foreign=ack;foreign.group[0]^=1;inject(foreign);katmed_mesh_poll(&now);
        katmed_mesh_night_status(&status);assert(status.acknowledged==0);
        inject(ack);katmed_mesh_poll(&now);katmed_mesh_night_status(&status);
        assert(status.finished && !status.active && status.acknowledged==1 && status.rejected==0);
        inject(ack);katmed_mesh_poll(&now);katmed_mesh_night_status(&status);assert(status.acknowledged==1);
        assert(state.revision.number==medical.number && store.record.display.brightness==25);
        assert(katmed_mesh_copy_night(now.monotonic_ms)==MESH_ACCEPTED);
        advance_now(now.monotonic_ms+10001);katmed_mesh_poll(&now);katmed_mesh_night_status(&status);
        assert(status.finished && status.acknowledged==0 && status.expected==1); // missing ACK never success
        std::puts("night-copy authenticated actual adapter: explicit action, ACK, replay, foreign group and timeout OK");return 0;
    }
    #include "check_names_engine.inc"
    #include "check_names_recovery.inc"
    if(argc==2&&std::strcmp(argv[1],"name_assignment")==0){
        host_station_present=true;
        level=ROOT;katmed_mesh_poll(&now);
        assert(std::strcmp(store_mesh_name(&store),"KatmedESP-001")==0 &&
               store_mesh_name_index(&store)==1 &&
               std::strcmp(host_hostname,"KatmedESP-001")==0);
        auto challenge=latest_kind(MESH_HELLO);
        std::memcpy(current_local_nonce,challenge.session_nonce,16);
        auto hello=peer_frame(MESH_HELLO,1);
        assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,
            hello.payload,sizeof(hello.payload),&hello.payload_length));
        inject(hello);katmed_mesh_poll(&now);
        assert(katmed_mesh_connected());
        assert(store_mesh_name_has_device(&store,99));
        auto map_frame=latest_kind(MESH_NAME_MAP);
        StoreMeshNameEntry entries[KATMED_MESH_NAME_MAX_ENTRIES]{};uint8_t count=0;
        assert(katmed_mesh_decode_name_map(map_frame.payload,map_frame.payload_length,
                                            entries,&count));
        assert(count==2 && entries[0].index!=entries[1].index);
        std::puts("mesh name assignment: root self/peer persistence, authenticated map and hostname OK");
        return 0;
    }
    if(argc==2&&std::strcmp(argv[1],"solo_settings")==0){
        level=ROOT;katmed_mesh_poll(&now);
        assert(katmed_mesh_is_coordinator()&&!katmed_mesh_connected());
        EventId id{};assert(store_next_event_id(&store,&id)==STORE_OK);
        AppEvent event{};event.kind=EVENT_SET_IMAGE_INTERVAL;event.id=id;
        event.sender=id.device;event.round=state.round;event.parent_revision=state.revision;
        event.occurred_at=now;event.value.interval_seconds=7200;
        AppEffects effects{};
        assert(katmed_mesh_submit_shared(&event,&now,&effects)==MESH_ACCEPTED);
        assert(state.settings.image_seconds==7200&&store.record.shared.image_seconds==7200);
        Store reopened{};StoreRecovery recovered{};
        assert(store_open(&reopened,backend,11,catalog,75,&recovered)==STORE_OK);
        assert(recovered.shared.image_seconds==7200);
        const unsigned before_dismiss=memory.commits;
        assert(app_dismiss_boot_alarm(&state,&now,&effects)==APP_OK);
        assert(!state.has_last_confirmation&&!effects.persistence_required&&
               !effects.broadcast_required&&memory.commits==before_dismiss);
        assert(store_next_event_id(&store,&id)==STORE_OK);
        ManualTimePreview preview{};
        assert(app_preview_local_time(&now,14,30,&preview)==APP_OK);
        event.kind=EVENT_SET_NEXT_LOCAL_TIME;event.id=id;event.sender=id.device;
        event.parent_revision=state.revision;event.round=state.round;
        event.value.local_time.hour=14;event.value.local_time.minute=30;
        event.value.local_time.selected_utc=preview.regular_utc;
        event.value.local_time.selected_effective_utc=preview.effective_utc;
        assert(katmed_mesh_submit_shared(&event,&now,&effects)==MESH_ACCEPTED);
        assert(state.start_reason==START_MANUAL&&
               store.record.history.deadline.regular_utc.ms==preview.regular_utc.ms);
        Memory decoded=memory;
        Store durable{};StoreRecovery durable_recovery{};
        assert(store_open(&durable,{&decoded,read_blob,write_blob},11,catalog,75,&durable_recovery)==STORE_OK);
        assert(durable.record.history.start_reason==START_MANUAL&&
               durable.record.history.deadline.regular_utc.ms==preview.regular_utc.ms);
        const auto stable=state;
        assert(store_next_event_id(&store,&id)==STORE_OK);
        event.kind=EVENT_SET_IMAGE_INTERVAL;event.id=id;event.sender=id.device;
        event.round=state.round;event.parent_revision=state.revision;event.value.interval_seconds=3600;
        level=2;katmed_mesh_poll(&now); /* No authenticated coordinator session. */
        assert(katmed_mesh_submit_shared(&event,&now,&effects)==MESH_UNAVAILABLE);
        level=ROOT;katmed_mesh_poll(&now);
        store.record.provision.group[0]^=1; /* Foreign binding fails current_group. */
        assert(katmed_mesh_submit_shared(&event,&now,&effects)==MESH_UNAVAILABLE);
        store.record.provision.group[0]^=1;
        store.record.provision.configured=false;
        assert(katmed_mesh_submit_shared(&event,&now,&effects)==MESH_UNAVAILABLE);
        store.record.provision.configured=true;
        store.record.provision.secret[0]^=1;
        assert(katmed_mesh_submit_shared(&event,&now,&effects)==MESH_UNAVAILABLE);
        store.record.provision.secret[0]^=1;
        const auto before_failure=memory.blob;
        memory.fail=true;
        assert(katmed_mesh_submit_shared(&event,&now,&effects)==MESH_STORE_ERROR);
        assert(state.settings.image_seconds==stable.settings.image_seconds&&
               state.revision.number==stable.revision.number);
        assert(memory.blob==before_failure);
        std::puts("mesh solo-root settings: durable image/next-time; follower, foreign, unconfigured, commit failure OK");
        return 0;
    }
    #include "check_status_engine.inc"
    #include "check_interaction_engine.inc"
    if(argc==3&&std::strcmp(argv[1],"cold_consume")==0){
        assert(recovery.has_last_confirmation&&state.alarm_active);
        level=ROOT;katmed_mesh_poll(&now);
        KatmedMeshFrame challenge{};
        assert(katmed_mesh_wire_decode(sent.back().data(),sent.back().size(),
            provision.group,provision.secret,&challenge));
        std::memcpy(current_local_nonce,challenge.session_nonce,16);
        auto peer=peer_frame(MESH_HELLO,200);
        assert(katmed_mesh_encode_hello(challenge.session_nonce,catalog,"d941fae",0,
            peer.payload,sizeof(peer.payload),&peer.payload_length));
        inject(peer);katmed_mesh_poll(&now);
        assert(katmed_mesh_connected());
        const auto ack_before=count_kind(MESH_ACK);
        const auto commits_before=memory.commits;
        inject_bytes(old_state_bytes);inject_bytes(old_confirm_bytes);
        katmed_mesh_poll(&now);
        assert(state.alarm_active&&state.round.ordinal==1&&
               count_kind(MESH_ACK)==ack_before&&memory.commits==commits_before);
        KatmedMeshFrame fresh_state{};
        assert(katmed_mesh_wire_decode(old_state_bytes.data(),old_state_bytes.size(),
            provision.group,provision.secret,&fresh_state));
        std::memcpy(fresh_state.receiver_nonce,current_local_nonce,16);
        fresh_state.session_counter=201;
        inject(fresh_state);katmed_mesh_poll(&now);
        assert(!state.alarm_active&&state.has_last_confirmation&&
               state.last_confirmation_id.device==99&&state.round.ordinal==2);
        const auto accepted_revision=state.revision.number;
        KatmedMeshFrame fresh_confirm{};
        assert(katmed_mesh_wire_decode(old_confirm_bytes.data(),old_confirm_bytes.size(),
            provision.group,provision.secret,&fresh_confirm));
        std::memcpy(fresh_confirm.receiver_nonce,current_local_nonce,16);
        fresh_confirm.session_counter=202;
        inject(fresh_confirm);katmed_mesh_poll(&now);
        assert(state.revision.number==accepted_revision&&
               count_kind(MESH_ACK)==ack_before+1);
        KatmedMeshFrame accepted_ack{};
        assert(katmed_mesh_wire_decode(sent.back().data(),sent.back().size(),
            provision.group,provision.secret,&accepted_ack));
        assert(accepted_ack.kind==MESH_ACK&&accepted_ack.payload[24]==1);
        std::puts("mesh cold start drops old frames, recovers committed confirm after fresh STATE OK");
        return 0;
    }
    if(argc==2&&std::strcmp(argv[1],"pending")==0){
        prepare_due_round();
        EventId own{};assert(store_next_event_id(&store,&own)==STORE_OK);
        AppEvent local{};local.kind=EVENT_CONFIRM;local.id=own;
        local.sender=own.device;local.round=state.round;
        local.parent_revision=state.revision;local.occurred_at=now;
        AppEffects effects{};AppResult core=APP_INVALID;
        assert(store_apply_event(&store,&state,&local,&now,true,&effects,&core)==STORE_OK);
        assert(store.record.pending_count==1&&state.confirmation_pending_sync);
        const auto confirmation=state.last_confirmation_id;
        const auto deadline=state.deadline.effective_monotonic_ms;
        const auto round=state.round.ordinal;
        level=ROOT;katmed_mesh_poll(&now);
        assert(store.record.pending_count==0&&!state.confirmation_pending_sync);
        assert(state.round.ordinal==round&&
               state.deadline.effective_monotonic_ms==deadline&&
               state.last_confirmation_id.device==confirmation.device&&
               state.last_confirmation_id.sequence==confirmation.sequence);
        std::puts("mesh root transition reconciles own pending without restart OK");
        return 0;
    }
    if(argc==2&&std::strcmp(argv[1],"send_failure")==0){
        prepare_due_round();
        EventId own{};assert(store_next_event_id(&store,&own)==STORE_OK);
        AppEvent local{};local.kind=EVENT_CONFIRM;local.id=own;
        local.sender=own.device;local.round=state.round;
        local.parent_revision=state.revision;local.occurred_at=now;
        AppEffects effects{};AppResult core=APP_INVALID;
        assert(store_apply_event(&store,&state,&local,&now,true,&effects,&core)==STORE_OK);
        level=2;katmed_mesh_poll(&now);
        KatmedMeshFrame challenge=latest_kind(MESH_HELLO);
        std::memcpy(current_local_nonce,challenge.session_nonce,16);
        auto reply=peer_frame(MESH_HELLO,1);
        assert(katmed_mesh_encode_hello(challenge.session_nonce,catalog,"d941fae",0,
            reply.payload,sizeof(reply.payload),&reply.payload_length));
        inject(reply);katmed_mesh_poll(&now);
        assert(katmed_mesh_connected());
        const unsigned before=count_kind(MESH_CONFIRM);
        fail_send=true;
        advance_now(now.monotonic_ms+1000);katmed_mesh_poll(&now);
        assert(failed_sends>0&&count_kind(MESH_CONFIRM)==before&&
               store.record.pending_count==1&&state.confirmation_pending_sync);
        fail_send=false;
        advance_now(now.monotonic_ms+1000);katmed_mesh_poll(&now);
        assert(count_kind(MESH_CONFIRM)>before&&store.record.pending_count==1);
        std::puts("mesh raw transport send failure retains durable pending OK");
        return 0;
    }
    if(argc==2&&std::strcmp(argv[1],"full")==0){
        prepare_due_round();
        advance_now((UINT64_C(1)<<32)+1000);
        AppEffects effects{};
        assert(app_tick(&state,&now,&effects)==APP_OK);
        for(unsigned i=0;i<KATMED_STORE_PENDING_MAX;++i){
            if(i){
                advance_now(state.deadline.effective_monotonic_ms+1);
                assert(app_tick(&state,&now,&effects)==APP_OK&&state.alarm_active);
            }
            EventId id{};assert(store_next_event_id(&store,&id)==STORE_OK);
            AppEvent local{};local.kind=EVENT_CONFIRM;local.id=id;
            local.sender=id.device;local.round=state.round;
            local.parent_revision=state.revision;local.occurred_at=now;
            AppResult core=APP_INVALID;
            assert(store_apply_event(&store,&state,&local,&now,true,&effects,&core)==STORE_OK);
        }
        assert(store.record.pending_count==KATMED_STORE_PENDING_MAX);
        advance_now(state.deadline.effective_monotonic_ms+1);
        assert(app_tick(&state,&now,&effects)==APP_OK&&state.alarm_active);
        EventId ninth{};assert(store_next_event_id(&store,&ninth)==STORE_OK);
        AppEvent extra{};extra.kind=EVENT_CONFIRM;extra.id=ninth;
        extra.sender=ninth.device;extra.round=state.round;
        extra.parent_revision=state.revision;extra.occurred_at=now;
        AppResult core=APP_INVALID;
        const auto ordinal=state.round.ordinal;
        assert(store_apply_event(&store,&state,&extra,&now,true,&effects,&core)==STORE_FULL);
        assert(state.round.ordinal==ordinal&&state.alarm_active);
        level=2;katmed_mesh_poll(&now);
        KatmedMeshFrame challenge{};
        assert(katmed_mesh_wire_decode(sent.back().data(),sent.back().size(),
            provision.group,provision.secret,&challenge));
        std::memcpy(current_local_nonce,challenge.session_nonce,16);
        auto reply=peer_frame(MESH_HELLO,1);
        assert(katmed_mesh_encode_hello(challenge.session_nonce,catalog,"d941fae",0,
            reply.payload,sizeof(reply.payload),&reply.payload_length));
        inject(reply);katmed_mesh_poll(&now);
        for(unsigned attempt=0;attempt<3;++attempt){
            advance_now(now.monotonic_ms+1000);katmed_mesh_poll(&now);
        }
        bool seen[KATMED_STORE_PENDING_MAX]{};
        unsigned bursts=0;
        for(const auto &bytes:sent){
            KatmedMeshFrame frame{};
            assert(katmed_mesh_wire_decode(bytes.data(),bytes.size(),
                provision.group,provision.secret,&frame));
            if(frame.kind!=MESH_CONFIRM)continue;
            bool matched=false;
            for(unsigned i=0;i<KATMED_STORE_PENDING_MAX;++i){
                if(frame.id.device==store.record.pending[i].id.device&&
                   frame.id.epoch==store.record.pending[i].id.epoch&&
                   frame.id.sequence==store.record.pending[i].id.sequence){
                    seen[i]=true;matched=true;break;
                }
            }
            assert(matched);++bursts;
        }
        assert(bursts>=KATMED_STORE_PENDING_MAX);
        for(bool delivered:seen)assert(delivered);
        assert(store.record.pending_count==KATMED_STORE_PENDING_MAX);
        std::puts("mesh eight-pending outbox and >49-day monotonic OK");
        return 0;
    }
    if(argc==2&&(std::strcmp(argv[1],"night_leaf")==0||std::strcmp(argv[1],"leaf")==0||
                 std::strcmp(argv[1],"leaf_shared_negative")==0||
                 std::strcmp(argv[1],"leaf_shared_timeout")==0||
                 std::strcmp(argv[1],"leaf_running")==0||
                 std::strcmp(argv[1],"leaf_replay")==0||
                 std::strcmp(argv[1],"leaf_ack2")==0||
                 std::strcmp(argv[1],"leaf_ack2_fail")==0||
                 std::strcmp(argv[1],"time")==0||
                 std::strcmp(argv[1],"leaf_distinct")==0)){
        level=2;katmed_mesh_poll(&now);
        assert(!katmed_mesh_connected());
        KatmedMeshFrame challenge{};
        assert(katmed_mesh_wire_decode(sent.back().data(),sent.back().size(),
            provision.group,provision.secret,&challenge));
        std::memcpy(current_local_nonce,challenge.session_nonce,16);
        assert(challenge.kind==MESH_HELLO);
        auto reply=peer_frame(MESH_HELLO,1);
        assert(katmed_mesh_encode_hello(challenge.session_nonce,catalog,"d941fae",0,
            reply.payload,sizeof(reply.payload),&reply.payload_length));
        const unsigned before=count_kind(MESH_HELLO);
        inject(reply);katmed_mesh_poll(&now);
        assert(katmed_mesh_connected());
        assert(count_kind(MESH_HELLO)>=before+1);
        KatmedMeshFrame echo{};
        bool found=false;
        for(size_t i=sent.size();i>0;--i){
            if(!katmed_mesh_wire_decode(sent[i-1].data(),sent[i-1].size(),
                provision.group,provision.secret,&echo))continue;
            if(echo.kind==MESH_HELLO&&echo.recipient==reply.sender&&
               std::memcmp(echo.payload,reply.session_nonce,16)==0){found=true;break;}
        }
        assert(found);
        if(!std::strcmp(argv[1],"night_leaf")) {
            auto local=katmed_display_defaults();local.brightness=17;
            assert(store_set_display(&store,&local)==STORE_OK);
            KatmedNightMessage message{};message.op=2;message.id={99,5,400};
            message.settings=local;message.settings.night_enabled=true;message.settings.night_start=1200;
            auto f=peer_frame(MESH_NIGHT,2);f.payload_length=KATMED_NIGHT_BYTES;
            assert(katmed_night_encode(&message,f.payload));
            auto broken=f;broken.payload[35]=1;unsigned commits=memory.commits;
            inject(broken);katmed_mesh_poll(&now);assert(memory.commits==commits);
            f.session_counter=3;inject(f);katmed_mesh_poll(&now);
            assert(store.record.display.night_start==1200 && store.record.display.brightness==17);
            auto answer=latest_kind(MESH_NIGHT);KatmedNightMessage ack{};
            assert(katmed_night_decode(answer.payload,answer.payload_length,&ack) && ack.op==3 && ack.acknowledged==1);
            local.night_start=1100;assert(store_set_display(&store,&local)==STORE_OK);
            commits=memory.commits;f.session_counter=4;inject(f);katmed_mesh_poll(&now);
            assert(memory.commits==commits && store.record.display.night_start==1100); // retry cannot undo local override
            message.id.sequence=401;message.settings.night_start=1000;
            assert(katmed_night_encode(&message,f.payload));f.session_counter=5;
            const unsigned replies=count_kind(MESH_NIGHT);
            memory.fail=true;inject(f);katmed_mesh_poll(&now);
            assert(count_kind(MESH_NIGHT)==replies); // unhealthy Store forbids authenticated TX; no false ACK
            assert(store.record.display.night_start==1100 && store.uncertain);
            std::puts("night leaf: strict payload, durable ACK, local override/idempotent retry and write failure rejection PASS");return 0;
        }
        if(std::strcmp(argv[1],"leaf_ack2")==0||
           std::strcmp(argv[1],"leaf_ack2_fail")==0){
            prepare_due_round();
            EventId id{};assert(store_next_event_id(&store,&id)==STORE_OK);
            AppEvent local{};local.kind=EVENT_CONFIRM;local.id=id;
            local.sender=id.device;local.round=state.round;
            local.parent_revision=state.revision;local.occurred_at=now;
            AppEffects effects{};AppResult core=APP_INVALID;
            assert(store_apply_event(&store,&state,&local,&now,true,
                &effects,&core)==STORE_OK);
            assert(store.record.pending_count==1&&!state.alarm_active);
            auto ack=peer_frame(MESH_ACK,2);
            for(unsigned i=0;i<8;++i){
                ack.payload[i]=uint8_t(id.device>>(56-8*i));
                ack.payload[8+i]=uint8_t(id.epoch>>(56-8*i));
                ack.payload[16+i]=uint8_t(id.sequence>>(56-8*i));
            }
            ack.payload[24]=2;ack.payload_length=25;
            const bool fail=std::strcmp(argv[1],"leaf_ack2_fail")==0;
            memory.fail=fail;
            inject(ack);katmed_mesh_poll(&now);
            assert(store.record.pending_count==(fail?1:0));
            uint8_t conflicts=0;
            if(!fail){
                assert(store_conflict_count(&store,&conflicts)==STORE_OK&&
                       conflicts==1&&state.divergent);
            }else{
                assert(!state.divergent&&shared_results==0);
                memory.fail=false;
                Store reopened{};StoreRecovery after{};
                assert(store_open(&reopened,backend,11,catalog,75,&after)==STORE_OK);
                assert(reopened.record.pending_count==1&&
                       store_conflict_count(&reopened,&conflicts)==STORE_OK&&
                       conflicts==0);
            }
            std::puts(fail
                ?"mesh leaf ACK-2 uncertain commit retains pending after reopen OK"
                :"mesh leaf ACK-2 atomically journals conflict and removes pending OK");
            return 0;
        }
        if(std::strcmp(argv[1],"time")==0){
            auto time=peer_frame(MESH_TIME_SYNC,2);
            const uint64_t value=uint64_t(now.utc_ms+5000);
            for(unsigned i=0;i<8;++i)
                time.payload[i]=uint8_t(value>>(56-8*i));
            time.payload[8]=CLOCK_HOLDOVER;time.payload_length=9;
            inject(time);katmed_mesh_poll(&now);
            assert(utc_calls==1&&last_quality==CLOCK_HOLDOVER&&
                   last_observed_mono==now.monotonic_ms&&
                   last_received_utc==now.utc_ms+5000);
            inject(time);katmed_mesh_poll(&now);
            assert(utc_calls==1);
            time.session_counter=3;time.payload[8]=CLOCK_SYNCHRONIZED;
            inject(time);katmed_mesh_poll(&now);
            assert(utc_calls==2&&last_quality==CLOCK_SYNCHRONIZED);
            time.session_counter=4;time.payload[8]=CLOCK_UNAVAILABLE;
            inject(time);katmed_mesh_poll(&now);
            assert(utc_calls==2);
            std::puts("mesh authenticated TIME_SYNC quality and replay OK");
            return 0;
        }
        if(std::strcmp(argv[1],"leaf_distinct")==0){
            SharedSettings root_settings=app_default_settings();
            ClockSample root_now{1000,99,now.utc_ms,true,CLOCK_SYNCHRONIZED};
            AppState separate_root{};
            assert(app_init(&separate_root,&root_settings,&root_now,
                EventId{99,5,1},75,catalog)==APP_OK);
            StateSnapshot separate{};
            assert(app_make_snapshot(&separate_root,&root_now,&separate)==APP_OK);
            auto root_state=peer_frame(MESH_STATE,2);
            assert(katmed_mesh_encode_snapshot(&separate,root_state.payload,
                sizeof(root_state.payload),&root_state.payload_length));
            inject(root_state);katmed_mesh_poll(&now);
            assert(state.revision.coordinator==99&&
                   state.round.origin.device==99&&state.alarm_active);
            std::puts("mesh distinct root boot state adopted after fresh HELLO OK");
            return 0;
        }
        if(std::strcmp(argv[1],"leaf_running")==0||
           std::strcmp(argv[1],"leaf_replay")==0){
            prepare_due_round();
            EventId confirmed{};assert(store_next_event_id(&store,&confirmed)==STORE_OK);
            AppEvent med{};med.kind=EVENT_CONFIRM;med.id=confirmed;
            med.sender=confirmed.device;med.round=state.round;
            med.parent_revision=state.revision;med.occurred_at=now;
            med.coordinator_accepted=true;
            AppEffects effect{};AppResult result=APP_INVALID;
            assert(store_apply_event(&store,&state,&med,&now,false,&effect,&result)==STORE_OK);
            assert(!state.alarm_active);
        }
        EventId id{};assert(store_next_event_id(&store,&id)==STORE_OK);
        AppEvent requested{};requested.kind=EVENT_SET_ROTATION_MODE;
        requested.id=id;requested.sender=id.device;requested.round=state.round;
        requested.parent_revision=state.revision;requested.occurred_at=now;
        requested.value.rotation=ROTATION_OFF;
        AppEffects effects{};
        assert(katmed_mesh_submit_shared(&requested,&now,&effects)==MESH_PENDING);
        assert(shared_results==0&&state.settings.rotation!=ROTATION_OFF);
        auto ack=peer_frame(MESH_ACK,2);
        for(unsigned i=0;i<8;++i){
            ack.payload[i]=uint8_t(id.device>>(56-8*i));
            ack.payload[8+i]=uint8_t(id.epoch>>(56-8*i));
            ack.payload[16+i]=uint8_t(id.sequence>>(56-8*i));
        }
        ack.payload[24]=1;ack.payload_length=25;
        if(std::strcmp(argv[1],"leaf_shared_negative")==0){
            ack.payload[24]=0;
            inject(ack);katmed_mesh_poll(&now);
            assert(shared_results==1&&shared_outcome==MESH_SHARED_REJECTED&&
                   shared_result_id.sequence==id.sequence&&
                   state.settings.rotation!=ROTATION_OFF);
            std::puts("mesh explicit negative shared ACK distinguished OK");
            return 0;
        }
        if(std::strcmp(argv[1],"leaf_shared_timeout")==0){
            advance_now(now.monotonic_ms+10000);
            katmed_mesh_poll(&now);
            assert(shared_results==1&&shared_outcome==MESH_SHARED_TIMEOUT&&
                   shared_result_id.sequence==id.sequence&&
                   state.settings.rotation!=ROTATION_OFF);
            /* A late positive ACK alone cannot turn a timeout into success.
             * A later durable snapshot is still adopted without a second callback. */
            inject(ack);katmed_mesh_poll(&now);
            assert(shared_results==1&&shared_outcome==MESH_SHARED_TIMEOUT);
            AppState remote=state;
            AppEvent approved=requested;approved.coordinator_accepted=true;
            assert(app_apply_event(&remote,&approved,&now,&effects)==APP_OK);
            StateSnapshot snapshot{};
            assert(app_make_snapshot(&remote,&now,&snapshot)==APP_OK);
            auto late_state=peer_frame(MESH_STATE,3);
            assert(katmed_mesh_encode_snapshot(&snapshot,late_state.payload,
                sizeof(late_state.payload),&late_state.payload_length));
            inject(late_state);katmed_mesh_poll(&now);
            assert(shared_results==1&&state.settings.rotation==ROTATION_OFF);
            Store reopened{};StoreRecovery recovery{};
            assert(store_open(&reopened,backend,11,catalog,75,&recovery)==STORE_OK);
            assert(recovery.shared.rotation==ROTATION_OFF);
            std::puts("mesh timeout, late ACK and durable late snapshot OK");
            return 0;
        }
        inject(ack);katmed_mesh_poll(&now);
        assert(shared_results==0&&state.settings.rotation!=ROTATION_OFF);
        AppState remote=state;
        AppEvent approved=requested;approved.coordinator_accepted=true;
        assert(app_apply_event(&remote,&approved,&now,&effects)==APP_OK);
        StateSnapshot snapshot{};
        assert(app_make_snapshot(&remote,&now,&snapshot)==APP_OK);
        auto state_frame=peer_frame(MESH_STATE,3);
        assert(katmed_mesh_encode_snapshot(&snapshot,state_frame.payload,
            sizeof(state_frame.payload),&state_frame.payload_length));
        if(std::strcmp(argv[1],"leaf_replay")==0){
            level=0;katmed_mesh_poll(&now);
            level=2;katmed_mesh_poll(&now);
            KatmedMeshFrame next_challenge{};
            assert(katmed_mesh_wire_decode(sent.back().data(),sent.back().size(),
                provision.group,provision.secret,&next_challenge));
            assert(next_challenge.kind==MESH_HELLO&&
                   std::memcmp(next_challenge.session_nonce,current_local_nonce,16)!=0);
            std::memcpy(current_local_nonce,next_challenge.session_nonce,16);
            auto fresh_hello=peer_frame(MESH_HELLO,20);
            assert(katmed_mesh_encode_hello(next_challenge.session_nonce,catalog,"d941fae",0,
                fresh_hello.payload,sizeof(fresh_hello.payload),&fresh_hello.payload_length));
            inject(fresh_hello);katmed_mesh_poll(&now);
            assert(katmed_mesh_connected());
            inject(ack);inject(state_frame);katmed_mesh_poll(&now);
            assert(shared_results==0&&state.settings.rotation!=ROTATION_OFF);
            std::memcpy(ack.receiver_nonce,current_local_nonce,16);
            ack.session_counter=21;
            inject(ack);katmed_mesh_poll(&now);
            assert(shared_results==0&&state.settings.rotation!=ROTATION_OFF);
            std::memcpy(state_frame.receiver_nonce,current_local_nonce,16);
            state_frame.session_counter=22;
        }
        inject(state_frame);katmed_mesh_poll(&now);
        assert(shared_results==1&&shared_accepted&&
               shared_result_id.sequence==id.sequence);
        assert(state.settings.rotation==ROTATION_OFF&&memory.commits>0);
        std::puts("mesh leaf reciprocal HELLO OK");
        return 0;
    }
    level=ROOT;katmed_mesh_poll(&now);
    assert(!katmed_mesh_connected());
    assert(count_kind(MESH_HELLO)>=1);
    KatmedMeshFrame outbound{};
    assert(katmed_mesh_wire_decode(sent.back().data(),sent.back().size(),
                                   provision.group,provision.secret,&outbound));
    std::memcpy(current_local_nonce,outbound.session_nonce,16);
    assert(outbound.kind==MESH_HELLO);
    auto hello=peer_frame(MESH_HELLO,1);
    uint8_t no_echo[16]{};
    assert(katmed_mesh_encode_hello(no_echo,catalog,"d941fae",0,
        hello.payload,sizeof(hello.payload),&hello.payload_length));
    const unsigned hello_before=count_kind(MESH_HELLO);
    inject(hello);katmed_mesh_poll(&now);
    assert(!katmed_mesh_connected());
    assert(count_kind(MESH_HELLO)>=hello_before+1);
    hello.session_counter=2;
    assert(katmed_mesh_encode_hello(outbound.session_nonce,catalog,"d941fae",0,
        hello.payload,sizeof(hello.payload),&hello.payload_length));
    inject(hello);katmed_mesh_poll(&now);
    assert(katmed_mesh_connected());
    if(argc==2 && !std::strcmp(argv[1],"manual_source")) {
        const unsigned before=count_kind(MESH_TIME_SYNC);
        share_time=false;now.quality=CLOCK_HOLDOVER;
        katmed_mesh_notify_local_commit();katmed_mesh_poll(&now);
        assert(count_kind(MESH_TIME_SYNC)==before);
        share_time=true;katmed_mesh_notify_local_commit();katmed_mesh_poll(&now);
        assert(count_kind(MESH_TIME_SYNC)>before);
        return 0;
    }
    if(argc==2&&std::strcmp(argv[1],"hello_replay")==0){
        auto recorded=hello;
        recorded.session_counter=9;
        assert(katmed_mesh_encode_hello(outbound.session_nonce,catalog,"d941fae",0,
            recorded.payload,sizeof(recorded.payload),&recorded.payload_length));
        inject(recorded);katmed_mesh_poll(&now);
        auto announced=recorded;
        announced.session_counter=10;
        assert(katmed_mesh_encode_hello(outbound.session_nonce,catalog,"d941fae",2,
            announced.payload,sizeof(announced.payload),&announced.payload_length));
        inject(announced);katmed_mesh_poll(&now);
        advance_now(10000);
        AppEffects effects{};
        assert(app_tick(&state,&now,&effects)==APP_OK);
        auto confirm=peer_frame(MESH_CONFIRM,11);
        StorePendingConfirm pending{};pending.id=confirm.id;
        pending.sender=confirm.sender;pending.round=state.round;
        pending.parent_revision=state.revision;
        pending.occurred_at={8000,55,now.utc_ms-2000,true,CLOCK_HOLDOVER};
        assert(katmed_mesh_encode_confirm(&pending,2000,confirm.payload,
            sizeof(confirm.payload),&confirm.payload_length));
        inject(confirm);katmed_mesh_poll(&now);
        advance_now(16000);katmed_mesh_poll(&now);
        const unsigned ack_before=count_kind(MESH_ACK);
        inject(recorded);katmed_mesh_poll(&now);
        assert(!state.has_last_confirmation&&count_kind(MESH_ACK)==ack_before);
        advance_now(21000);katmed_mesh_poll(&now);
        assert(!state.has_last_confirmation&&state.divergent&&
               count_kind(MESH_ACK)==ack_before);
        std::puts("mesh stale signed HELLO cannot lower announced outbox count OK");
        return 0;
    }
    if(argc==2&&(std::strcmp(argv[1],"order")==0||
                 std::strcmp(argv[1],"incomplete")==0||
                 std::strcmp(argv[1],"incomparable")==0||
                 std::strcmp(argv[1],"same_domain")==0||
                 std::strcmp(argv[1],"same_domain_no_utc")==0||
                 std::strcmp(argv[1],"single_unknown")==0||
                 std::strcmp(argv[1],"late_peer")==0)){
        const bool incomplete=std::strcmp(argv[1],"incomplete")==0;
        const bool incomparable=std::strcmp(argv[1],"incomparable")==0;
        const bool same_domain=std::strcmp(argv[1],"same_domain")==0;
        const bool same_domain_no_utc=
            std::strcmp(argv[1],"same_domain_no_utc")==0;
        const bool single_unknown=std::strcmp(argv[1],"single_unknown")==0;
        const bool late_peer=std::strcmp(argv[1],"late_peer")==0;
        prepare_due_round();
        advance_now(10000);
        AppEffects effects{};
        assert(app_tick(&state,&now,&effects)==APP_OK);
        auto announced=hello;
        announced.session_counter=3;
        assert(katmed_mesh_encode_hello(outbound.session_nonce,catalog,"d941fae",1,
            announced.payload,sizeof(announced.payload),&announced.payload_length));
        inject(announced);katmed_mesh_poll(&now);
        auto second=peer_frame(MESH_HELLO,1);
        second.sender=100;second.id.device=100;
        second.session_nonce[0]=100;
        assert(katmed_mesh_encode_hello(outbound.session_nonce,catalog,"d941fae",1,
            second.payload,sizeof(second.payload),&second.payload_length));
        if(!late_peer&&!single_unknown){inject(second);katmed_mesh_poll(&now);}
        const auto round=state.round;
        StorePendingConfirm pending{};pending.round=round;
        pending.occurred_at=(incomparable||single_unknown||same_domain_no_utc)
            ?ClockSample{8000,same_domain_no_utc?1u:55u,0,false,CLOCK_UNAVAILABLE}
            :ClockSample{8000,same_domain?1u:55u,now.utc_ms-1000,true,CLOCK_HOLDOVER};
        auto late=peer_frame(MESH_CONFIRM,4);pending.id=late.id;
        assert(katmed_mesh_encode_confirm(&pending,2000,late.payload,
            sizeof(late.payload),&late.payload_length));
        pending.occurred_at=(incomparable||same_domain_no_utc)
            ?ClockSample{9000,same_domain_no_utc?1u:66u,0,false,CLOCK_UNAVAILABLE}
            :ClockSample{9000,same_domain?1u:66u,now.utc_ms-2000,true,CLOCK_HOLDOVER};
        auto early=peer_frame(MESH_CONFIRM,2);
        early.sender=100;early.id.device=100;early.session_nonce[0]=100;
        pending.id=early.id;
        assert(katmed_mesh_encode_confirm(&pending,2000,early.payload,
            sizeof(early.payload),&early.payload_length));
        inject(late);
        if(!late_peer&&!incomplete&&!single_unknown)inject(early);
        katmed_mesh_poll(&now);
        assert(!state.has_last_confirmation);
        advance_now(incomplete?20000:15000);katmed_mesh_poll(&now);
        if(incomplete||incomparable||single_unknown||same_domain_no_utc){
            assert(!state.has_last_confirmation&&state.divergent);
            uint8_t conflicts=0;
            assert(store_conflict_count(&store,&conflicts)==STORE_OK&&conflicts==0);
            assert(count_kind(MESH_ACK)==0);
            std::puts(incomplete
                ?"mesh missing announced recovery entry never commits OK"
                :(same_domain_no_utc
                    ?"mesh equal numeric domains of different peers need UTC OK"
                    :(single_unknown
                    ?"mesh single incomparable clock is durable warning, no ACK OK"
                    :"mesh incomparable monotonic domains retain pending and divergence OK")));
            return 0;
        }
        if(late_peer){
            assert(state.last_confirmation_id.device==99);
            announced.session_counter=5;
            assert(katmed_mesh_encode_hello(outbound.session_nonce,catalog,"d941fae",0,
                announced.payload,sizeof(announced.payload),&announced.payload_length));
            inject(announced);katmed_mesh_poll(&now);
            inject(second);katmed_mesh_poll(&now);
            early.session_counter=3;
            inject(early);katmed_mesh_poll(&now);
            advance_now(20000);katmed_mesh_poll(&now);
            assert(state.last_confirmation_id.device==99);
        }
        else assert(state.last_confirmation_id.device==100);
        assert(state.divergent);
        uint8_t conflicts=0;
        assert(store_conflict_count(&store,&conflicts)==STORE_OK&&conflicts==1);
        StorePendingConfirm conflict{};
        assert(store_conflict_at(&store,0,&conflict)==STORE_OK&&
               conflict.id.device==(late_peer?100u:99u));
        assert(latest_kind(MESH_ACK).payload[24]==2);
        auto forged=late_peer?early:late;
        forged.session_counter=10;
        StorePendingConfirm altered{};uint64_t transmitted_age=0;
        assert(katmed_mesh_decode_confirm(forged.payload,forged.payload_length,
            &altered,&transmitted_age));
        altered.id=forged.id;
        altered.occurred_at.utc_ms+=1000;
        assert(katmed_mesh_encode_confirm(&altered,transmitted_age,forged.payload,
            sizeof(forged.payload),&forged.payload_length));
        const unsigned ack_count=count_kind(MESH_ACK);
        inject(forged);katmed_mesh_poll(&now);
        assert(count_kind(MESH_ACK)==ack_count&&
               store_conflict_count(&store,&conflicts)==STORE_OK&&conflicts==1);
        std::puts(late_peer
            ?"mesh late peer is durable conflict, no silent backdating OK"
            :(same_domain
                ?"mesh cross-device equal domain numbers still sort by UTC OK"
                :"mesh reunion orders offline confirms and records divergence OK"));
        return 0;
    }
    if(argc==2&&std::strcmp(argv[1],"queue_overflow")==0){
        hello.session_counter=3;
        assert(katmed_mesh_encode_hello(outbound.session_nonce,catalog,"d941fae",4,
            hello.payload,sizeof(hello.payload),&hello.payload_length));
        inject(hello);katmed_mesh_poll(&now);
        for(unsigned i=0;i<4;++i){
            auto confirm=peer_frame(MESH_CONFIRM,4+i);
            StorePendingConfirm pending{};
            pending.id=confirm.id;pending.round=state.round;
            pending.occurred_at={now.monotonic_ms,55,now.utc_ms,true,CLOCK_HOLDOVER};
            assert(katmed_mesh_encode_confirm(&pending,2000,confirm.payload,
                sizeof(confirm.payload),&confirm.payload_length));
            inject(confirm);
        }
        assert(dropped==1);
        katmed_mesh_poll(&now);
        assert(!state.has_last_confirmation&&count_kind(MESH_ACK)==0);
        advance_now(11000);katmed_mesh_poll(&now);
        assert(!state.has_last_confirmation&&state.divergent&&
               count_kind(MESH_ACK)==0);
        std::puts("mesh bounded RX queue overflow never gives false commit/ACK OK");
        return 0;
    }
    if(argc==2&&std::strcmp(argv[1],"fanout")==0){
        for(uint64_t device=100;device<=106;++device){
            auto participant=peer_frame(MESH_HELLO,1);
            participant.sender=device;participant.id.device=device;
            participant.session_nonce[0]=uint8_t(device);
            assert(katmed_mesh_encode_hello(outbound.session_nonce,catalog,"d941fae",0,
                participant.payload,sizeof(participant.payload),&participant.payload_length));
            inject(participant);katmed_mesh_poll(&now);
        }
        const auto before_states=count_kind(MESH_STATE);
        const size_t before_packets=sent.size();
        katmed_mesh_notify_local_commit();katmed_mesh_poll(&now);
        assert(count_kind(MESH_STATE)==before_states+8);
        bool recipients[8]{};
        for(size_t i=before_packets;i<sent.size();++i){
            KatmedMeshFrame frame{};
            assert(katmed_mesh_wire_decode(sent[i].data(),sent[i].size(),
                provision.group,provision.secret,&frame));
            if(frame.kind!=MESH_STATE)continue;
            assert(frame.recipient>=99&&frame.recipient<=106);
            recipients[frame.recipient-99]=true;
            assert(sent[i].size()<=KATMED_MESH_WIRE_MAX);
        }
        for(bool seen:recipients)assert(seen);
        auto ninth=peer_frame(MESH_HELLO,1);
        ninth.sender=107;ninth.id.device=107;ninth.session_nonce[0]=107;
        assert(katmed_mesh_encode_hello(outbound.session_nonce,catalog,"d941fae",0,
            ninth.payload,sizeof(ninth.payload),&ninth.payload_length));
        inject(ninth);katmed_mesh_poll(&now);
        const auto after_ninth=count_kind(MESH_STATE);
        katmed_mesh_notify_local_commit();katmed_mesh_poll(&now);
        assert(count_kind(MESH_STATE)==after_ninth+8);
        std::puts("mesh eight-peer authenticated STATE fanout OK");
        return 0;
    }
    prepare_due_round();
    const unsigned ack_before=count_kind(MESH_ACK);
    auto confirm=peer_frame(MESH_CONFIRM,3);
    StorePendingConfirm pending{};pending.id=confirm.id;pending.round=state.round;
    pending.occurred_at={25,55,now.utc_ms,true,CLOCK_HOLDOVER};
    assert(katmed_mesh_encode_confirm(&pending,0,confirm.payload,sizeof(confirm.payload),
                                      &confirm.payload_length));
    const uint64_t before_invalid=state.revision.number;
    auto invalid_clock=confirm;invalid_clock.session_counter=3;
    pending.occurred_at.utc_valid=false;
    pending.occurred_at.quality=CLOCK_UNAVAILABLE;
    assert(katmed_mesh_encode_confirm(&pending,0,invalid_clock.payload,
        sizeof(invalid_clock.payload),&invalid_clock.payload_length));
    inject(invalid_clock);katmed_mesh_poll(&now);
    assert(state.revision.number==before_invalid);
    assert(count_kind(MESH_ACK)==ack_before+1);
    confirm.session_counter=4;
    inject(confirm);katmed_mesh_poll(&now);
    assert(!state.alarm_active&&state.round.ordinal==2);
    assert(count_kind(MESH_ACK)==ack_before+2);
    if(argc==3&&std::strcmp(argv[1],"cold_produce")==0){
        StateSnapshot saved{};
        assert(app_make_snapshot(&state,&now,&saved)==APP_OK);
        auto state_frame=peer_frame(MESH_STATE,100);
        assert(katmed_mesh_encode_snapshot(&saved,state_frame.payload,
            sizeof(state_frame.payload),&state_frame.payload_length));
        size_t n=0;old_state_bytes.resize(KATMED_MESH_WIRE_MAX);
        assert(katmed_mesh_wire_encode(&state_frame,provision.secret,
            old_state_bytes.data(),old_state_bytes.size(),&n));
        old_state_bytes.resize(n);
        confirm.session_counter=101;
        old_confirm_bytes.resize(KATMED_MESH_WIRE_MAX);
        assert(katmed_mesh_wire_encode(&confirm,provision.secret,
            old_confirm_bytes.data(),old_confirm_bytes.size(),&n));
        old_confirm_bytes.resize(n);
        write_fixture(argv[2]);
        std::puts("mesh durable commit / lost-ACK cold fixture OK");
        return 0;
    }
    const uint64_t revision=state.revision.number;
    confirm.session_counter=5;
    inject(confirm);katmed_mesh_poll(&now);
    assert(state.revision.number==revision);
    assert(count_kind(MESH_ACK)==ack_before+3);
    auto competing=confirm;
    competing.id.sequence=6;competing.session_counter=6;
    inject(competing);katmed_mesh_poll(&now);
    assert(state.revision.number==revision);
    assert(count_kind(MESH_ACK)==ack_before+4);
    const KatmedMeshFrame rejected=latest_kind(MESH_ACK);
    assert(rejected.kind==MESH_ACK&&rejected.payload[24]==2);
    uint8_t conflicts=0;
    assert(store_conflict_count(&store,&conflicts)==STORE_OK&&conflicts==1);
    StateSnapshot snap{};
    assert(app_make_snapshot(&state,&now,&snap)==APP_OK);
    auto foreign=peer_frame(MESH_STATE,7);
    snap.state.revision.coordinator=100;
    assert(katmed_mesh_encode_snapshot(&snap,foreign.payload,
        sizeof(foreign.payload),&foreign.payload_length));
    inject(foreign);katmed_mesh_poll(&now);
    assert(state.revision.number==revision);
    assert(!state.catalog_mismatch);
    snap.state.revision.number=revision+1;
    snap.state.catalog_id[0]^=0x80;snap.catalog_id[0]^=0x80;
    foreign.session_counter=8;foreign.id.sequence=8;
    assert(katmed_mesh_encode_snapshot(&snap,foreign.payload,
        sizeof(foreign.payload),&foreign.payload_length));
    inject(foreign);katmed_mesh_poll(&now);
    assert(state.revision.number==revision+1);
    assert(state.catalog_mismatch&&!state.alarm_active);
    assert(state.catalog_id[0]==catalog[0]);
    const auto due_before_clock_correction=state.deadline.effective_monotonic_ms;
    now.monotonic_ms+=1000;now.utc_ms-=3600000;
    now.quality=CLOCK_HOLDOVER;
    AppEffects clock_effects{};
    assert(app_tick(&state,&now,&clock_effects)==APP_OK);
    katmed_mesh_poll(&now);
    /* A UTC correction may conservatively advance the night deadline, but
     * the transport must never restart a fresh twelve-hour interval. */
    assert(state.deadline.effective_monotonic_ms<=due_before_clock_correction);
    auto recorded=confirm;recorded.session_counter=100;
    const auto prior_nonce=std::vector<uint8_t>(current_local_nonce,current_local_nonce+16);
    level=2;katmed_mesh_poll(&now);
    level=ROOT;katmed_mesh_poll(&now);
    KatmedMeshFrame new_challenge{};
    assert(katmed_mesh_wire_decode(sent.back().data(),sent.back().size(),
        provision.group,provision.secret,&new_challenge));
    assert(new_challenge.kind==MESH_HELLO&&
           std::memcmp(new_challenge.session_nonce,prior_nonce.data(),16)!=0);
    std::memcpy(current_local_nonce,new_challenge.session_nonce,16);
    auto rehello=peer_frame(MESH_HELLO,101);
    assert(katmed_mesh_encode_hello(new_challenge.session_nonce,catalog,"d941fae",0,
        rehello.payload,sizeof(rehello.payload),&rehello.payload_length));
    inject(rehello);katmed_mesh_poll(&now);
    assert(katmed_mesh_connected());
    const auto acks_after_rekey=count_kind(MESH_ACK);
    const auto revision_after_rekey=state.revision.number;
    inject(recorded);katmed_mesh_poll(&now);
    assert(count_kind(MESH_ACK)==acks_after_rekey&&
           state.revision.number==revision_after_rekey);
    auto fresh_old_round=recorded;
    std::memcpy(fresh_old_round.receiver_nonce,current_local_nonce,16);
    fresh_old_round.session_counter=102;
    inject(fresh_old_round);katmed_mesh_poll(&now);
    assert(count_kind(MESH_ACK)==acks_after_rekey+1&&
           state.revision.number==revision_after_rekey);
    /* Four valid received frames fit only three queue entries. The fourth
     * must be dropped without executing an unbounded callback workload. */
    for(unsigned i=9;i<13;++i){
        auto repeat=confirm;repeat.session_counter=i;inject(repeat);
    }
    assert(dropped==1);katmed_mesh_poll(&now);
    EventId own{};assert(store_next_event_id(&store,&own)==STORE_OK);
    AppEvent settings{};settings.kind=EVENT_SET_ROTATION_MODE;settings.id=own;
    settings.sender=own.device;settings.round=state.round;
    settings.parent_revision=state.revision;settings.occurred_at=now;
    settings.value.rotation=ROTATION_OFF;
    const RotationMode before=state.settings.rotation;
    memory.fail=true;AppEffects effects{};
    assert(katmed_mesh_submit_shared(&settings,&now,&effects)==MESH_STORE_ERROR);
    assert(state.settings.rotation==before&&!effects.state_changed);
    assert(store.uncertain);
    std::puts("mesh actual IDF adapter / Store / Core host repro OK");
}
