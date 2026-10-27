"""Offline regressions for owner-task cleanup, listener ownership and measurements."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import json
import test_acme_lifecycle as lifecycle
import re
import sys
from test_acme_lifecycle import method, pending_client_fixture, observer_fixture
from test_acme_ram import resource_capture, with_measurements
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import katmed_acme_ram as ram

class Ownerfix3(unittest.TestCase):
    def test_global_and_task_local_lease_reset_and_restart_real_contexts(self):
        lease=(ROOT/'firmware/components/katmed_security/katmed_source_export.c').read_text()
        c='''#include <stdatomic.h>\n#include <stdbool.h>\n#include <limits.h>\n#include <stdlib.h>\n#define PHASE_MASK 7u\n#define LEASE_UNIT 8u\n#define MAX_LEASES (UINT_MAX >> 3u)\nstatic atomic_uint snapshot_state;\nstatic _Thread_local unsigned owned_leases;\n'''
        c+=method(lease,'bool katmed_source_writer_enter(')+method(lease,'void katmed_source_writer_leave(')
        c+='\nunsigned leases(){return atomic_load(&snapshot_state)/LEASE_UNIT;}\nunsigned owned(){return owned_leases;}\n'
        ac=(ROOT/'firmware/components/katmed_acme/katmed_acme.cpp').read_text()
        upstream=(ROOT/'firmware/managed_components/dannybackx__acmeclient/Acme.cpp').read_text()
        web=(ROOT/'firmware/components/katmed_web/katmed_web.c').read_text()
        main_source=(ROOT/'firmware/main/main.c').read_text()
        pre=pending_client_fixture()
        pre=pre.replace('void xSemaphoreTake(SemaphoreHandle_t s,int)', 'int xSemaphoreTake(SemaphoreHandle_t s,int)').replace('assert(s->ready);', 'assert(s->ready);return 1;')
        pre=pre.replace('#include <functional>','#include <functional>\n#include <atomic>\n#include <thread>\n#include <chrono>\n#include <mutex>\n#include <cstdint>')
        pre=pre.replace('#define ESP_LOGI(...) ((void)0)', '#define ESP_LOGI(tag,fmt,...) std::printf("I (1) " tag ": " fmt "\\n",__VA_ARGS__)')
        # All I/O remains synthetic; queued work drains a pending response before unregister.
        pre=pre.replace('static int slots=0,code=0;static bool fail_queue=false,fail_unregister=false;',
            'static std::atomic<int> slots{0};static int code=0;static std::atomic<bool> fail_queue{true},fail_unregister{false},server_alive{true};static std::atomic<int> response_finished{0},queue_failures{0};')
        pre=pre.replace('if(fail_queue)return 1;queued=f;', 'assert(server_alive);if(fail_queue){++queue_failures;return 1;}queued=f;')
        pre=pre.replace('#define KATMED_ACME_PROBE(p) ((void)0)', 'extern "C" void katmed_acme_probe_mark(unsigned);\n#define KATMED_ACME_PROBE(p) katmed_acme_probe_mark(p)')
        pre=pre.replace('class Acme {public:',r'''
class Acme {public:
Acme(){_acme=this;}
void setUrl(const char*){} void setEmail(const char*){}void setAcmeServer(const char*){}
void setFilenamePrefix(const char*){}void setAccountFilename(const char*){}void setDirectoryFilename(const char*){}
void setAccountKeyFilename(const char*){}void setOrderFilename(const char*){}void setCertKeyFilename(const char*){}
void setCertificateFilename(const char*){}void setWebServer(int){}
int getAccountKey(){return 1;}int getCertificateKey(){return 1;}
void GenerateAccountKey(){}void GenerateCertificateKey(){}
bool CreateNewAccount(){return true;}
void CreateNewOrder(){assert(ValidateOrder()==status_pending);}
void RenewCertificate(){}bool HaveValidCertificate(long){return false;}
bool loop(long){return false;}
''')
        # The original logging stubs suppress parameter evaluation. This harness
        # needs real producer records, so supply the upstream static tag too.
        pre=pre.replace('Acme *Acme::_acme=nullptr;', 'Acme *Acme::_acme=nullptr;\n')
        # Upstream logs use variable tags whereas the producer uses literals.
        pre=pre.replace('std::printf("I (1) " tag ": " fmt "\\n",__VA_ARGS__)', 'std::printf("I (1) %s: " fmt "\\n",tag,##__VA_ARGS__)')
        common=r'''
#define ESP_LOGW(...) ((void)0)
using httpd_handle_t=int;
#define KATMED_ACME_RESOURCE_PROBE 1
#define KATMED_ACME_RESOURCE_CLIENT 1
#define KATMED_ACME_RESOURCE_SERVER "http://127.0.0.1:17171/directory"
#define configNUMBER_OF_CORES 1
#define CONFIG_FREERTOS_USE_TICKLESS_IDLE 0
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_DMA 4
#define pdMS_TO_TICKS(x) (x)
#define tskIDLE_PRIORITY 0
#define pdPASS 1
#define pdTRUE 1
using BaseType_t=int;using TaskHandle_t=void*;
extern "C" {bool katmed_source_writer_enter();void katmed_source_writer_leave();unsigned leases();unsigned owned();}
static std::atomic<uint64_t> tick{1};
static thread_local TaskHandle_t current=(void*)1;
TaskHandle_t xTaskGetCurrentTaskHandle(){return current;}
uint64_t esp_timer_get_time(){return tick.fetch_add(1);}
uint64_t ulTaskGetIdleRunTimeCounterForCore(int){return tick.load()/2;}
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
static bool monitor;
static int free_bytes=64000;
int heap_caps_get_free_size(int){return free_bytes;}
int heap_caps_get_minimum_free_size(int){return free_bytes-1000;}
int heap_caps_get_largest_free_block(int){return free_bytes-2000;}
int heap_caps_monitor_local_minimum_free_size_start(){assert(!monitor);monitor=true;return 0;}
int heap_caps_monitor_local_minimum_free_size_stop(){assert(monitor);monitor=false;return 0;}
static std::thread task;
static std::atomic<bool> suspended{false};
static int deleted;
void vTaskDelay(int){std::this_thread::sleep_for(std::chrono::milliseconds(1));tick.fetch_add(1000);}
void vTaskSuspend(void*){assert(owned()==0 && leases()==0 && !monitor);suspended=true;}
enum {eRunning,eSuspended};
int eTaskGetState(void*){return suspended?eSuspended:eRunning;}
int xPortGetCoreID(){return 0;}
void vTaskDelete(void*){assert(suspended);task.join();++deleted;}
int xTaskCreatePinnedToCore(void(*fn)(void*),const char*,int,void*,int,void**h,int){*h=(void*)2;task=std::thread([fn]{current=(void*)2;fn(nullptr);});return pdPASS;}
size_t strlcpy(char*d,const char*s,size_t n){size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l;}
bool katmed_web_acme_host_matches(const char*,const char*){return true;}
int katmed_web_acme_http_server(){return server_alive?1:0;}
bool katmed_acme_atomic_idle(){return true;}
struct KatmedAcmeConfig {const char *identifier,*email,*server,*state_prefix;};
static const char *TAG="fixture";static const char*tag="fixture";
static Acme *client;static std::atomic<TaskHandle_t> client_owner;static std::atomic<bool> acme_abort;
static std::atomic<TaskHandle_t> probe_worker;static bool certificate_changed;static char challenge_host[254];
bool katmed_acme_quiescent();void katmed_acme_resource_probe_observe();
'''
        upstream_methods='\n'.join(method(upstream,x) for x in ('Acme::~Acme()', 'bool Acme::Cleanup()', 'esp_err_t Acme::acme_http_get_handler(', 'void Acme::EnableLocalWebServer()', 'void Acme::DisableLocalWebServer()', 'Acme::return_status Acme::ValidateOrder()'))
        adapter='\n'.join(method(ac,x) for x in ('bool katmed_acme_start(', 'void katmed_acme_poll(', 'void katmed_acme_stop(', 'bool katmed_acme_quiescent(', 'bool katmed_acme_certificate_changed('))
        observer=ac[ac.index('static unsigned probe_cycle;'):ac.index('\n#else\nbool katmed_acme_resource_probe_launch')]
        prepare=method(ac,'bool katmed_acme_prepare_listener_stop(')
        caller_stubs=r'''
struct Runtime {bool reset_requested=true,reset_active=false,store_ready=true;int nvs=0,names_nvs=0;uint64_t reset_retry_due_ms=0;};
static int reset_mutex=1;
int xSemaphoreTake(int,int){return 1;}void xSemaphoreGive(int){}
using KatmedResetJournalBackend=int;
static int journal_saves,mesh_stops,store_closes,names_closes,reset_resumes;
int reset_backend(){return 1;}
bool katmed_reset_begin(int);
void katmed_mesh_stop(){assert(leases()==0 && journal_saves==1);++mesh_stops;}
void store_idf_close_backend(int*){assert(leases()==0 && journal_saves==1);++store_closes;}
void names_idf_close(int*){assert(leases()==0 && journal_saves==1);++names_closes;}
bool katmed_reset_resume(int,bool*){assert(journal_saves==1);++reset_resumes;return false;}
void esp_restart(){assert(false);}
static int listener_stops=0;
static bool redirect_stop_fails=true,https_stop_fails=true;
static int https_stop_calls,key_destroys,nvs_closes;
#define ESP_OK 0
#define ESP_FAIL -1
#define PSA_KEY_ID_NULL 0
#define PSA_SUCCESS 0
int httpd_stop(int){
 assert(leases()==0 && client_owner.load()==nullptr && probe_worker==nullptr);
 if(redirect_stop_fails)return ESP_FAIL;
 server_alive=false;++listener_stops;return ESP_OK;
}
int httpd_ssl_stop(int){++https_stop_calls;return https_stop_fails?ESP_FAIL:ESP_OK;}
int psa_destroy_key(int){++key_destroys;return PSA_SUCCESS;}
void nvs_close(int){++nvs_closes;}void vSemaphoreDelete(void*){}
void wipe(void*p,size_t n){memset(p,0,n);}
struct Services {void*context;bool(*prepare_listener_stop)(void*);};
struct Web {bool started=true,nvs_open=true,restart_pending=false,certificate_verified=false;uint64_t retry_due_ms=0,restart_due_ms=0;SemaphoreHandle_t auth_mutex=nullptr;Services services{};int redirect=1,https=2,nvs=0,certificate_fd=0,auth=17,identity=42,tls_key_id=7,tls_key_config=7;unsigned char certificate_fingerprint[32]{};}web;
bool katmed_reset_begin(int){
 assert(leases()==0 && !web.redirect && !web.https && web.tls_key_id==PSA_KEY_ID_NULL);
 assert(!web.nvs_open && web.auth==0 && web.identity==0);
 ++journal_saves;return true;
}
bool start_listeners(){web.started=true;web.redirect=1;server_alive=true;return true;}
bool katmed_web_start(Services*){return start_listeners();}
void katmed_web_auth_tick(int*,uint64_t){}
static bool management_enabled=true;
void web_socket_diag_sample(uint64_t,bool){}
'''
        callers='\n'.join(method(main_source,x) for x in ('static bool web_prepare_listener_stop(', 'static bool reset_runtime_step('))
        web_bodies='\n'.join(method(web,x) for x in ('static bool stop_listeners(', 'bool katmed_web_stop(', 'void katmed_web_poll('))
        # reset references the existing public stop before its implementation.
        caller_stubs+='\nbool katmed_web_stop();\n'
        main=r'''
int main(int argc,char**){
web.services.prepare_listener_stop=web_prepare_listener_stop;
finish_response=[](){assert(server_alive && slots==1);httpd_req_t r{Acme::_acme->ValidationFile};Acme::acme_http_get_handler(&r);++response_finished;};
assert(katmed_acme_resource_probe_launch());tick=13000001;katmed_acme_resource_probe_observe();
for(int n=0;n<5000 && slots.load()==0;++n)std::this_thread::sleep_for(std::chrono::milliseconds(1));
assert(slots==1 && leases()==1 && owned()==0);
Runtime rt;
if(argc>1)assert(reset_runtime_step(&rt,1));
else{web.restart_pending=true;katmed_web_poll(1);assert(web.restart_pending);}
for(int n=0;n<5000 && queue_failures.load()==0;++n)std::this_thread::sleep_for(std::chrono::milliseconds(1));
assert(queue_failures>0 && server_alive && leases()==1 && !suspended && !deleted);
for(int i=0;i<4;++i){if(argc>1)assert(reset_runtime_step(&rt,2+i));else katmed_web_poll(2+i);}
assert(server_alive && leases()==1 && !katmed_acme_quiescent());
assert(journal_saves==0 && reset_resumes==0 && mesh_stops==0 && store_closes==0 && names_closes==0);
assert(rt.reset_requested && !rt.reset_active && rt.store_ready);
fail_queue=false;
for(int n=0;n<5000 && !suspended.load();++n)std::this_thread::sleep_for(std::chrono::milliseconds(1));
assert(suspended && leases()==0 && response_finished==1 && owned()==0);
if(argc>1)assert(reset_runtime_step(&rt,20));else katmed_web_poll(20);
assert(deleted==1 && listener_stops==0 && https_stop_calls==0 && leases()==0 && katmed_acme_quiescent());
assert(web.redirect && web.https && web.tls_key_id==7 && key_destroys==0);
assert(web.nvs_open && web.auth==17 && web.identity==42 && nvs_closes==0);
assert(journal_saves==0 && reset_resumes==0 && rt.reset_requested && !rt.reset_active);
/* Listener restart stops after a failed stop; only Reset retries automatically.
 * Issue another explicit restart request to test recovery in the other context. */
if(argc==1){assert(!web.restart_pending);web.restart_pending=true;}
redirect_stop_fails=false;
if(argc>1)assert(reset_runtime_step(&rt,6020));else katmed_web_poll(6020);
assert(listener_stops==1 && !web.redirect && web.https && https_stop_calls==1);
assert(web.tls_key_id==7 && key_destroys==0 && web.nvs_open && web.identity==42);
assert(journal_saves==0 && reset_resumes==0 && mesh_stops==0 && store_closes==0 && names_closes==0);
assert(rt.reset_requested && !rt.reset_active && rt.store_ready);
if(argc==1){assert(!web.restart_pending);web.restart_pending=true;}
https_stop_fails=false;
if(argc>1)assert(reset_runtime_step(&rt,12020));else katmed_web_poll(12020);
assert(deleted==1 && listener_stops==1 && https_stop_calls==2 && key_destroys==1);
if(argc>1){
 assert(!rt.reset_requested && rt.reset_active && !rt.store_ready);
 assert(journal_saves==1 && reset_resumes==1 && mesh_stops==1 && store_closes==1 && names_closes==1);
 assert(!web.https && !web.nvs_open && web.auth==0 && web.identity==0 && nvs_closes==1);
}else{
 assert(!web.restart_pending && web.started);
 assert(journal_saves==0 && reset_resumes==0 && mesh_stops==0 && store_closes==0 && names_closes==0);
}
std::thread second([]{current=(void*)3;assert(katmed_source_writer_enter());assert(owned()==1);katmed_source_writer_leave();assert(owned()==0);});second.join();
assert(leases()==0 && owned()==0);
puts("actual_owner_retry_two_contexts_pending_response_reset_restart_lease_and_server=passed");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp);(tmp/'lease.c').write_text(c)
            subprocess.run(['cc','-std=c11','-c',str(tmp/'lease.c'),'-o',str(tmp/'lease.o')],check=True,capture_output=True)
            f=tmp/'test.cpp';f.write_text(pre+common+upstream_methods+adapter+observer+prepare+caller_stubs+callers+web_bodies+main)
            result=subprocess.run(['c++','-std=c++17','-pthread',str(f),str(tmp/'lease.o'),'-o',str(tmp/'test')],capture_output=True)
            self.assertEqual(0,result.returncode,result.stderr.decode())
            for args in ([],['reset']):
                out=subprocess.run([str(tmp/'test'),*args],capture_output=True,timeout=15)
                self.assertEqual(0,out.returncode,out.stderr.decode())
                self.assertIn(b'actual_owner_retry_two_contexts',out.stdout)
                self.assertIn(b'phase=released status=error',out.stdout)

    def test_actual_operation_and_cpu_producer_complete_retries_and_aggregation(self):
        ac=(ROOT/'firmware/components/katmed_acme/katmed_acme.cpp').read_text()
        observer=ac[ac.index('static unsigned probe_cycle;'):ac.index('\n#else\nbool katmed_acme_resource_probe_launch')]
        stub=r'''
#define KATMED_ACME_RESOURCE_SERVER "http://127.0.0.1:17171/directory"
struct KatmedAcmeConfig {const char *id,*email,*server,*prefix;};
long time(void*){return 1800000000;}
struct Client {int getAccountKey(){return 1;}int getCertificateKey(){return 1;}
void GenerateAccountKey(){}void GenerateCertificateKey(){}
bool CreateNewAccount(){return false;}void CreateNewOrder(){}
bool HaveValidCertificate(long){return false;}void RenewCertificate(){}} fake,*client=&fake;
bool katmed_acme_start(const KatmedAcmeConfig*){return true;}
void katmed_acme_poll(uint64_t){}bool katmed_acme_certificate_changed(){return false;}
'''
        main=r'''
int main(){
for(unsigned c=1;c<=3;++c){
probe_cpu_begin("before",c);vTaskDelay(12000);probe_cpu_end();probe_cpu_begin("active",c);
probe_cycle=c;probe_sequence=0;probe_phase=0;probe_monitor_valid=true;
memset(probe_windows,0,sizeof(probe_windows));probe_running=false;probe_sample("running");
probe_running=true;monitor=true;probe_launched=true;probe_core=0;probe_worker=(void*)1;
katmed_acme_probe_mark(c==1?1:5);
if(c==1){free_bytes=52000;vTaskDelay(1000);katmed_acme_resource_probe_observe();free_bytes=64000;}
katmed_acme_probe_mark(2);free_bytes=56000;vTaskDelay(1000);katmed_acme_resource_probe_observe();free_bytes=64000;
katmed_acme_probe_mark(3);
vTaskDelay(1000);katmed_acme_resource_probe_observe();katmed_acme_probe_mark(2);katmed_acme_probe_mark(3);
assert(probe_windows[0].attempt==1);
katmed_acme_probe_mark(4);katmed_acme_probe_mark(2);vTaskDelay(1000);
// A new order aborts issuance, but retains the renewal parent until the final switch.
katmed_acme_probe_mark(12);katmed_acme_probe_mark(2);katmed_acme_probe_mark(3);katmed_acme_probe_mark(4);
free_bytes=48000;vTaskDelay(1000);katmed_acme_resource_probe_observe();
katmed_acme_probe_mark(2);free_bytes=64000;katmed_acme_probe_mark(8);
katmed_acme_probe_mark(2);assert(probe_phase==8);
free_bytes=40000;vTaskDelay(1000);katmed_acme_resource_probe_observe();katmed_acme_probe_mark(9);
free_bytes=64000;katmed_acme_probe_mark(13);katmed_acme_probe_mark(6);
for(const ProbeWindow&w:probe_windows)assert(!w.active);
probe_next(7);probe_sample("running");heap_caps_monitor_local_minimum_free_size_stop();probe_running=false;
probe_worker=nullptr;probe_cpu_end();probe_cpu_begin("after",c);probe_phase=9;probe_sample("ok");
vTaskDelay(12000);probe_cpu_end();}
}
'''
        pre=observer_fixture().replace('RESOURCE_CLIENT 0','RESOURCE_CLIENT 1')
        out=lifecycle.Lifecycle.compile_run(self,pre+stub+observer+main)
        result=ram.evaluate_resource(out,'client')
        self.assertEqual('source_capture_valid',result['qualification'])
        self.assertFalse(result['hardware_pass'])
        self.assertEqual(9,len(result['cpu_windows']))
        for w in result['operation_windows']:
            if w['op'] in ('issuance','renewal') and w['status']=='ok':
                self.assertLessEqual(w['internal_min'],39000)
                self.assertLessEqual(w['internal_block_observed'],38000)
        self.assertTrue(any(w['op']=='key' and w['internal_block_observed']<=50000 for w in result['operation_windows']))
        self.assertTrue(any(w['op']=='connection' and w['internal_block_observed']<=54000 for w in result['operation_windows']))
        self.assertTrue(any(w['status']=='error' for w in result['operation_windows']))
        self.assertTrue(any(w['max_gap_us']>=1000000 for w in result['operation_windows']))
        raw=out.splitlines(keepends=True)
        short=[]
        for line in raw:
            if b'op=renewal' in line and b'event=end' in line:
                line=re.sub(rb'end_seq=[0-9]+',b'end_seq=3',line)
            short.append(line)
        with self.assertRaises(ram.RamBenchError):ram.evaluate_resource(b''.join(short),'client')

    def test_actual_monitor_failure_cannot_be_hidden_by_later_success(self):
        ac=(ROOT/'firmware/components/katmed_acme/katmed_acme.cpp').read_text()
        observer=ac[ac.index('static unsigned probe_cycle;'):ac.index('\n#else\nbool katmed_acme_resource_probe_launch')]
        pre=observer_fixture().replace('int heap_caps_monitor_local_minimum_free_size_stop(){assert(monitor);monitor=false;return 0;}',
            'static int stops;int heap_caps_monitor_local_minimum_free_size_stop(){assert(monitor);monitor=false;return ++stops==1?1:0;}')
        main=r'''
int main(){assert(katmed_acme_resource_probe_launch());tick+=13000000;katmed_acme_resource_probe_observe();
try{probe_task(nullptr);}catch(int){}suspended=true;katmed_acme_resource_probe_observe();
assert(deleted==1 && !probe_success && !probe_measurement_valid);}
'''
        out=lifecycle.Lifecycle.compile_run(self,pre+observer+main)
        self.assertIn(b'phase=released status=error',out)
        self.assertIn(b'monitor_valid=0',out)

    def test_actual_observer_retains_worker_with_atomic_slot_or_running_monitor(self):
        ac=(ROOT/'firmware/components/katmed_acme/katmed_acme.cpp').read_text()
        observer=ac[ac.index('static unsigned probe_cycle;'):ac.index('\n#else\nbool katmed_acme_resource_probe_launch')]
        pre=observer_fixture().replace('bool katmed_acme_atomic_idle(){return true;}', 'static bool slots_idle=true;bool katmed_acme_atomic_idle(){return slots_idle;}')
        main=r'''
int main(){assert(katmed_acme_resource_probe_launch());tick+=13000000;katmed_acme_resource_probe_observe();
try{probe_task(nullptr);}catch(int){}suspended=true;slots_idle=false;
katmed_acme_resource_probe_observe();assert(deleted==0 && probe_worker);
slots_idle=true;probe_running=true;katmed_acme_resource_probe_observe();assert(deleted==0);
probe_running=false;katmed_acme_resource_probe_observe();assert(deleted==1 && !probe_worker);}
'''
        out=lifecycle.Lifecycle.compile_run(self,pre+observer+main)
        self.assertEqual(1,out.count(b'phase=released status=ok'))

    def test_actual_observer_allocation_failure_stops_launching(self):
        ac=(ROOT/'firmware/components/katmed_acme/katmed_acme.cpp').read_text()
        observer=ac[ac.index('static unsigned probe_cycle;'):ac.index('\n#else\nbool katmed_acme_resource_probe_launch')]
        pre=observer_fixture().replace('*h=(void*)1;free_bytes-=12288;suspended=false;return pdPASS;', '++deleted;return 0;')
        main=r'''
int main(){assert(katmed_acme_resource_probe_launch());tick+=13000000;
for(int i=0;i<5;++i){katmed_acme_resource_probe_observe();tick+=13000000;}
assert(deleted==1 && !probe_worker && acme_abort.load() && probe_cycle==1);}
'''
        out=lifecycle.Lifecycle.compile_run(self,pre+observer+main)
        self.assertEqual(1,out.count(b'phase=released status=error'))
        with self.assertRaises(ram.RamBenchError):ram.evaluate_resource(out,'baseline')

    def test_actual_cpu_producer_invalid_and_overflow_windows(self):
        ac=(ROOT/'firmware/components/katmed_acme/katmed_acme.cpp').read_text()
        fragment=ac[ac.index('struct ProbeCpuWindow'):ac.index('static const char *probe_phase_name')]
        pre=r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#define configNUMBER_OF_CORES 2
static bool tickless=false;
#define CONFIG_FREERTOS_USE_TICKLESS_IDLE tickless
#define ESP_LOGI(tag,fmt,...) std::printf("I (1) " tag ": " fmt "\n",__VA_ARGS__)
using BaseType_t=int;
static uint64_t tick=10,idle[2]={5,5};
uint64_t esp_timer_get_time(){return tick;}
uint64_t ulTaskGetIdleRunTimeCounterForCore(int c){return idle[c];}
const char*probe_variant(){return "baseline";}
'''
        main=r'''
int main(){probe_cpu_begin("before",1);tick+=12000000;idle[0]+=6000000;idle[1]+=6000000;probe_cpu_end();
probe_cpu_begin("active",1);tick+=10;--idle[0];idle[1]+=11;probe_cpu_end();
probe_cpu_begin("after",1);probe_cpu_end();
probe_cpu_begin("after",2);tick+=12000000;tickless=true;probe_cpu_end();}
'''
        out=lifecycle.Lifecycle.compile_run(self,pre+fragment+main)
        _,records=ram._auxiliary_records(out)
        self.assertEqual([1,1,0,0,0,0,0,0],[r['valid'] for r in records])
        for record in records:
            if record['window']=='active' and record['core']==0:self.assertEqual(0,record['idle_delta'])

    def test_resource_cli_writes_bounded_window_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp);(tmp/'capture').write_bytes(resource_capture())
            result=subprocess.run([sys.executable,str(ROOT/'tools/katmed_acme_ram.py'),
                '--mode','resource','--variant','client','--input',str(tmp/'capture'),
                '--output',str(tmp/'summary')],capture_output=True)
            self.assertEqual(0,result.returncode,result.stderr.decode())
            summary=json.loads((tmp/'summary').read_text())
            self.assertEqual('source_capture_valid',summary['qualification'])
            self.assertLessEqual((tmp/'summary').stat().st_size,ram.MAX_RESOURCE_SUMMARY_BYTES)
            self.assertEqual(9,len(summary['cpu_windows']))
            self.assertFalse(summary['hardware_pass'])

    def test_missing_short_wrong_windows_and_cpu_wrap_fail_closed(self):
        raw=resource_capture()
        self.assertEqual('source_capture_valid',ram.evaluate_resource(raw,'client')['qualification'])
        lines=raw.splitlines(keepends=True)
        changes=[
            b''.join(x for x in lines if not (b'op=challenge' in x and b'event=end' in x)),
            raw.replace(b'window=active',b'window=after',1),
            raw.replace(b'counter_bits=64',b'counter_bits=32',1),
            raw.replace(b'tickless=0',b'tickless=1',1),
            raw.replace(b'idle_end=',b'idle_end=18446744073709551616',1),
            raw.replace(b'valid=1',b'valid=0',1),
            raw.replace(b'observations=5',b'observations=4294967296',1),
        ]
        for bad in changes:
            with self.subTest(case=bad[:80]):
                with self.assertRaises(ram.RamBenchError):ram.evaluate_resource(bad,'client')
        for field,value in ((b'start_seq',b'1'),(b'end_seq',b'2'),(b'op',b'download')):
            bad=[];changed=False
            for line in lines:
                if not changed and b'op=issuance' in line and b'event=end' in line:
                    line=re.sub(field+rb'=[a-z0-9]+',field+b'='+value,line);changed=True
                bad.append(line)
            with self.assertRaises(ram.RamBenchError):ram.evaluate_resource(b''.join(bad),'client')
        missing=b''.join(x for x in lines if b'katmed_acme_cpu:' not in x)
        self.assertEqual('incomplete',ram.evaluate_resource(missing,'client')['qualification'])
        missing=b''.join(x for x in lines if b'katmed_acme_window:' not in x)
        self.assertEqual('incomplete',ram.evaluate_resource(missing,'client')['qualification'])

if __name__=='__main__':unittest.main()
