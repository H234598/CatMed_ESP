"""Compile actual resolved lifecycle and observer bodies against narrow offline stubs."""
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import katmed_acme_ram as ram
import prepare_acme_component as patch

def method(text, signature):
    start = text.index(signature)
    pos = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[start:pos]

class Lifecycle(unittest.TestCase):
    def compile_run(self, source, args=()):
        with tempfile.TemporaryDirectory() as tmp:
            f = Path(tmp) / 'test.cpp'
            f.write_text(source)
            result = subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra',
                                     str(f), '-o', str(Path(tmp)/'test')], capture_output=True)
            self.assertEqual(0, result.returncode, result.stderr.decode())
            return subprocess.check_output([str(Path(tmp)/'test'), *args])

    def test_actual_pending_cleanup_destructor_failure_and_reregistration(self):
        import shutil
        with tempfile.TemporaryDirectory() as tmp:
            component = Path(tmp)/'client'
            shutil.copytree(ROOT/'firmware/managed_components/dannybackx__acmeclient', component)
            patch.apply(component)
            text = (component/'Acme.cpp').read_text()
        pre = r'''
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <functional>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_OK 0
#define HTTP_GET 0
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_404_NOT_FOUND 404
#define portMAX_DELAY 0
using esp_err_t=int;
struct httpd_req_t {const char *uri;};
struct httpd_uri_t {const char *uri;int method;int(*handler)(httpd_req_t*);};
struct StaticSemaphore_t {bool ready=false;};
using SemaphoreHandle_t=StaticSemaphore_t*;
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s){return s;}
void xSemaphoreGive(SemaphoreHandle_t s){s->ready=true;}
static void(*queued)(void*);static void*queued_arg;
static std::function<void()> finish_response;
void xSemaphoreTake(SemaphoreHandle_t s,int){
assert(queued);if(finish_response){finish_response();finish_response=nullptr;}
auto f=queued;queued=nullptr;f(queued_arg);assert(s->ready);
}
static int slots=0,code=0;static bool fail_queue=false,fail_unregister=false;
static std::string body;
static int httpd_queue_work(int,void(*f)(void*),void*a){if(fail_queue)return 1;queued=f;queued_arg=a;return 0;}
static int httpd_register_uri_handler(int,const httpd_uri_t*){if(slots)return 1;++slots;return 0;}
static int httpd_unregister_uri_handler(int,const char*,int){if(fail_unregister)return 1;--slots;return 0;}
static const char*esp_err_to_name(int){return "fixture";}
static bool katmed_acme_http_host_allowed(httpd_req_t*){return true;}
static void httpd_resp_send_err(httpd_req_t*,int c,const char*){code=c;}
static void httpd_resp_set_type(httpd_req_t*,const char*){}
static void httpd_resp_send(httpd_req_t*,const char*b,size_t n){body.assign(b,n);}
static void mbedtls_psa_crypto_free(){}
static void mbedtls_x509_crt_free(void*){}
static const char*acme_http_404="404";
#define KATMED_ACME_PROBE(p) ((void)0)
static const char*acme_http_01="http-01",*acme_status_invalid="invalid",*well_known="/.well-known/acme-challenge/";
class Acme {public:
static Acme *_acme;
enum return_status {status_fail,status_ok,status_pending};
struct Entry {const char*status;const char*_type;const char*token;};
Entry entries[2]={{"pending","http-01","pending"},{nullptr,nullptr,nullptr}};
struct Challenge {Entry*challenges;};Challenge ch{entries};Challenge*challenge=&ch;
struct Order {char*status=nullptr;};Order ord;Order*order=&ord;
int http01_ix=0;
int DownloadAuthorizationResource(){return 0;}
char*CreateValidationString(const char*){return strdup("synthetic.proof");}
return_status alert_status=status_pending;
return_status ValidateAlertServer(){return alert_status;}
return_status ValidateOrder();
int webserver=1;bool ws_registered=false;
char *ovf=nullptr,*ValidationFile=nullptr,*ValidationString=nullptr;
char *nonce=nullptr,*reply_buffer=nullptr,*account_location=nullptr,*retry_after=nullptr,*errmsg=nullptr;
int reply_buffer_len=0;void*certificate=nullptr;
void ClearAccount(){}void ClearOrder(){}void ClearChallenge(){}void ClearDirectory(){}void freeRenewalInfo(){}
~Acme();bool Cleanup();void EnableLocalWebServer();void DisableLocalWebServer();static int acme_http_get_handler(httpd_req_t*);
};
Acme *Acme::_acme=nullptr;
'''
        bodies = '\n'.join(method(text, sig) for sig in (
            'Acme::~Acme()', 'bool Acme::Cleanup()', 'esp_err_t Acme::acme_http_get_handler(',
            'void Acme::EnableLocalWebServer()', 'void Acme::DisableLocalWebServer()', 'Acme::return_status Acme::ValidateOrder()'))
        main = r'''
int main(){
for(int n=0;n<3;++n){
Acme*a=new Acme;Acme::_acme=a;
assert(a->ValidateOrder()==Acme::status_pending);assert(slots==1);
assert(a->ValidateOrder()==Acme::status_pending);assert(slots==1);
httpd_req_t r{a->ValidationFile};Acme::acme_http_get_handler(&r);assert(body=="synthetic.proof");
fail_queue=true;assert(!a->Cleanup());assert(slots==1 && a->ValidationFile);fail_queue=false;
fail_unregister=true;assert(!a->Cleanup());assert(slots==1 && a->ovf);fail_unregister=false;
finish_response=[&](){assert(slots==1);assert(a->ValidationString);Acme::acme_http_get_handler(&r);};
if(n==0){a->alert_status=Acme::status_ok;assert(a->ValidateOrder()==Acme::status_ok);
assert(slots==0 && !a->ValidationFile && !a->ValidationString && !a->ovf);assert(a->Cleanup());}
delete a;assert(slots==0 && Acme::_acme==nullptr);
body.clear();httpd_req_t missing{"/.well-known/acme-challenge/pending"};
Acme::acme_http_get_handler(&missing);assert(body.empty() && code==403);
}
puts("pending_cleanup_destructor_failure_retry_and_slot_reuse=passed");}
'''
        self.assertIn(b'passed', self.compile_run(pre+bodies+main))

    def test_actual_worker_release_observer_and_multicycle_loss(self):
        text = (ROOT/'firmware/components/katmed_acme/katmed_acme.cpp').read_text()
        text = text[text.index('static unsigned probe_cycle;'):text.index('\n#else\nbool katmed_acme_resource_probe_launch')]
        pre = r'''
#include <cstdio>
#include <cstdint>
#include <cassert>
#define KATMED_ACME_RESOURCE_CLIENT 0
#define ESP_LOGI(tag,fmt,...) std::printf("I (1) " tag ": " fmt "\n",__VA_ARGS__)
#define ESP_OK 0
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_DMA 4
#define pdMS_TO_TICKS(x) (x)
#define tskIDLE_PRIORITY 0
#define pdPASS 1
using BaseType_t=int;using TaskHandle_t=void*;
static int free_bytes=64000,deleted=0;static bool monitor=false,suspended=false;
static long tick=0;
long esp_timer_get_time(){return tick;}
int heap_caps_monitor_local_minimum_free_size_start(){assert(!monitor);monitor=true;return 0;}
int heap_caps_monitor_local_minimum_free_size_stop(){assert(monitor);monitor=false;return 0;}
int heap_caps_get_free_size(int){return free_bytes;}
int heap_caps_get_minimum_free_size(int){return free_bytes-1000;}
int heap_caps_get_largest_free_block(int){return free_bytes-2000;}
void katmed_acme_stop(){}bool katmed_acme_quiescent(){return true;}
void vTaskDelay(int){}
void vTaskSuspend(void*){throw 1;}
enum {eRunning,eSuspended};
int eTaskGetState(void*){return suspended?eSuspended:eRunning;}
int xPortGetCoreID(){return 0;}
void vTaskDelete(void*p){assert(p && suspended && !monitor);free_bytes+=12288;++deleted;}
int xTaskCreatePinnedToCore(void(*)(void*),const char*,int,void*,int,void**h,int){*h=(void*)1;free_bytes-=12288;suspended=false;return pdPASS;}
void*katmed_web_acme_http_server(){return (void*)1;}
'''
        main = r'''
int main(int argc,char**){
assert(katmed_acme_resource_probe_launch());
for(int c=1;c<=3;++c){
tick+=13000000;katmed_acme_resource_probe_observe();assert(probe_worker);
try{probe_task(nullptr);}catch(int){}
assert(!monitor);katmed_acme_resource_probe_observe();assert(deleted==c-1);
suspended=true;
if(argc>1)free_bytes-=100;
katmed_acme_resource_probe_observe();assert(deleted==c && !probe_worker);
}
}
'''
        capture = self.compile_run(pre+text+main)
        self.assertEqual('source_capture_valid',ram.evaluate_resource(capture,'baseline')['qualification'])
        self.assertEqual(3,capture.count(b'phase=released status=ok'))
        with self.assertRaises(ram.RamBenchError):
            ram.evaluate_resource(capture.replace(b'phase=released',b'phase=cleanup'),'baseline')
        client_stub = r'''
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
        failed=self.compile_run(pre.replace('RESOURCE_CLIENT 0','RESOURCE_CLIENT 1')+client_stub+text+main)
        self.assertEqual(3,failed.count(b'phase=released status=error'))
        with self.assertRaises(ram.RamBenchError): ram.evaluate_resource(failed,'client')
        window_main = r'''
int main(){probe_cycle=1;probe_running=true;probe_phase=4;monitor=true;probe_monitor_valid=true;
katmed_acme_probe_mark(8);katmed_acme_probe_mark(2);
assert(probe_phase==8 && monitor && probe_window);
katmed_acme_probe_mark(9);katmed_acme_probe_mark(10);assert(!probe_window);}
'''
        window_capture=self.compile_run(pre+text+window_main)
        self.assertNotIn(b'phase=connection',window_capture)
        self.assertGreaterEqual(window_capture.count(b'phase=certificate_download'),3)
        # Same producer, changing concurrent retained heap after every full cycle.
        drift = self.compile_run(pre+text+main, ('drift',))
        with self.assertRaises(ram.RamBenchError):
            ram.evaluate_resource(drift,'baseline')

    def test_actual_download_window_closes_on_success_and_failure(self):
        text=(ROOT/'firmware/managed_components/dannybackx__acmeclient/Acme.cpp').read_text()
        pre=r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <cerrno>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static bool window=false;static int mode=0,reads=0;static std::vector<unsigned> points;
void mark(unsigned p){points.push_back(p);if(p==8){assert(!window);window=true;}else if(p==10){assert(window);window=false;}else assert(window);}
#define KATMED_ACME_PROBE(p) mark(p)
FILE*katmed_acme_atomic_open(const char*){assert(window);return tmpfile();}
int katmed_acme_atomic_close(FILE*f){fclose(f);return mode==2?-1:0;}
int katmed_acme_atomic_abort(FILE*f){fclose(f);return -1;}
struct Order {const char*certificate="synthetic";};
class Acme {public:
Order item;Order*order=&item;bool ws_registered=false;
const char*cert_fn="certificate.pem",*filename_prefix="fixture",*acme_jose_json="",*acme_accept_pem_chain="";
char*MakeMessageKID(const char*,const char*){assert(window);return strdup("fixture");}
char*PerformWebQuery(const char*,const char*,const char*,const char*){mark(2);return mode==1?nullptr:strdup("synthetic-file");}
void DisableLocalWebServer(){}void ReadCertificate(){assert(window);++reads;}
bool DownloadCertificate();
};
'''
        main=r'''
int main(){Acme a;for(mode=0;mode<3;++mode){points.clear();reads=0;
bool ok=a.DownloadCertificate();assert(ok==(mode==0));assert(!window);
assert(points.front()==8 && points.back()==10);assert(reads==(mode==0));
if(mode==0)assert(points.size()>=6);}}
'''
        self.compile_run(pre+method(text,'bool Acme::DownloadCertificate()')+main)

    def test_actual_stop_retains_lease_on_failure(self):
        text=(ROOT/'firmware/components/katmed_acme/katmed_acme.cpp').read_text()
        pre=r'''
#include <cassert>
#include <cstring>
#define ESP_LOGE(...) ((void)0)
static int destroyed=0,leaves=0;
class Acme {public: bool ok=false;bool Cleanup(){return ok;}~Acme(){++destroyed;}};
static Acme*client;
static bool certificate_changed;
static char challenge_host[254];
bool katmed_acme_atomic_idle(){return true;}
void katmed_source_writer_leave(){++leaves;}
'''
        main=r'''
int main(){client=new Acme;katmed_acme_stop();assert(client && !destroyed && !leaves);
assert(!katmed_acme_quiescent());client->ok=true;katmed_acme_stop();
assert(!client && destroyed==1 && leaves==1 && katmed_acme_quiescent());
katmed_acme_stop();assert(destroyed==1 && leaves==1);}
'''
        self.compile_run(pre+method(text,'void katmed_acme_stop(')+method(text,'bool katmed_acme_quiescent(')+main)

    def test_whole_trace_rejects_late_issuance_and_accepts_nested_retries(self):
        from test_acme_ram import resource_record
        def trace(late):
            lines=[]
            for cycle in range(1,4):
                names=['idle','key' if cycle==1 else 'renewal','connection',
                       'challenge','connection','challenge','issuance','connection',
                       'issuance','connection','challenge','connection','issuance',
                       'certificate_download','certificate_download',
                       'certificate_switch']
                if late: names += ['issuance']
                names += ['cleanup','released']
                for seq,name in enumerate(names,1):
                    lines.append(resource_record(cycle,seq,name,status='ok' if name=='released' else 'running'))
            return ''.join(lines).encode()
        self.assertEqual('source_capture_valid',ram.evaluate_resource(trace(False),'client')['qualification'])
        with self.assertRaises(ram.RamBenchError): ram.evaluate_resource(trace(True),'client')

if __name__ == '__main__': unittest.main()
