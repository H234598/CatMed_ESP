"""Actual worker/client caller regression, adapted from the independent Peer3 harness.

Only network, certificate bytes and file-close failures are synthetic. The
worker, observer, AcmeProcess, ValidateOrder, RenewCertificate and DownloadCertificate
bodies are extracted from this checkout on every execution.
"""
from pathlib import Path
import re
import unittest
import test_acme_lifecycle as life
method=life.method
observer_fixture=life.observer_fixture
import test_acme_ownerfix3 as owner3
import katmed_acme_ram as ram
ROOT=Path(__file__).resolve().parents[2]

def client_capture(failures=0, delayed_delete=False):
    up=(ROOT/'firmware/managed_components/dannybackx__acmeclient/Acme.cpp').read_text()
    ac=(ROOT/'firmware/components/katmed_acme/katmed_acme.cpp').read_text()
    pre=observer_fixture().replace('RESOURCE_CLIENT 0','RESOURCE_CLIENT 1').replace('void katmed_acme_stop(){}','void katmed_acme_stop();')
    pre=pre.replace('#include <cstring>','#include <cstring>\n#include <cstdlib>\n#include <strings.h>\n#include <cerrno>\n#include <ctime>')
    # Variable tags in actual upstream methods and literal producer tags.
    pre=pre.replace('std::printf("I (1) " tag ": " fmt "\\n",__VA_ARGS__)', 'std::printf("I (1) %s: " fmt "\\n",tag,##__VA_ARGS__)')
    stub=r'''
    #define ESP_LOGD(...) ((void)0)
    #define ESP_LOGE(...) ((void)0)
    #define portTICK_PERIOD_MS 1
    #define KATMED_ACME_RESOURCE_SERVER "http://127.0.0.1:17171/directory"
    extern "C" void katmed_acme_probe_mark(unsigned);
    #define KATMED_ACME_PROBE(p) katmed_acme_probe_mark(p)
    static const char*tag="fixture";
    static const char *acme_status_invalid="invalid",*acme_status_pending="pending",*acme_status_ready="ready",*acme_status_processing="processing",*acme_status_downloaded="downloaded",*acme_status_valid="valid",*acme_http_01="http-01",*well_known="/.well-known/acme-challenge/";
    static int error_report_count;
    struct KatmedAcmeConfig {const char *id,*email,*server,*prefix;};
    FILE *katmed_acme_atomic_open(const char*){return tmpfile();}
    static int failures=FAILURES;
    int katmed_acme_atomic_close(FILE*f){fclose(f);return failures-- >0?-1:0;}
    int katmed_acme_atomic_abort(FILE*f){return fclose(f);}
    class Client {public:
     enum return_status {status_fail,status_pending,status_ok};
     struct Entry {const char*status="pending";const char*_type="http-01";const char*token="abc";bool active=false;};
     Entry entries[2];struct Challenge {const char*status="pending";Entry*challenges;};Challenge ch;
     Challenge*challenge=&ch;
     struct Order {char*status=nullptr;char*retry_after=nullptr;const char*certificate="fixture";};Order ord;Order*order=&ord;
     const char *email_address="fixture",*acme_url="probe.invalid",*retry_after=nullptr;
     int directory=1,account=1,http01_ix=0,webserver=1;bool ws_registered=false;
     void*certificate=nullptr;char *ValidationString=nullptr,*ValidationFile=nullptr;
     const char *cert_fn="certificate.pem",*filename_prefix="fixture",*acme_jose_json="",*acme_accept_pem_chain="";
     int pending=0;bool havekeys=false;
     Client(){entries[1].status=nullptr;ch.challenges=entries;}
     void ClearChallenge(){}bool checkConfig(){return true;}bool ReadAccountInfo(){return true;}void RequestNewAccount(const char*,bool){}
     void WriteAccountInfo(){}void ReadOrderInfo(){}void WriteOrderInfo(){}void OrderRemove(){}
     const char*returnstatus2string(return_status){return "fixture";}
     void RequestNewOrder(const char*){KATMED_ACME_PROBE(12);free(order->status);order->status=strdup("pending");pending=0;}
     void CreateNewOrder(){RequestNewOrder(acme_url);}
     void GetOrderStatus(){KATMED_ACME_PROBE(2);}
     void FinalizeOrder(){KATMED_ACME_PROBE(4);KATMED_ACME_PROBE(2);free(order->status);order->status=strdup("valid");}
     int DownloadAuthorizationResource(){KATMED_ACME_PROBE(2);return 0;}
     char*CreateValidationString(const char*){return strdup("abc.proof");}
     return_status ValidateAlertServer(){KATMED_ACME_PROBE(2);if(++pending==1)return status_pending;free(order->status);order->status=strdup("ready");return status_ok;}
     void EnableLocalWebServer(){ws_registered=true;}
     void DisableLocalWebServer(){ws_registered=false;}
     bool Cleanup(){DisableLocalWebServer();free(ValidationString);free(ValidationFile);ValidationString=ValidationFile=nullptr;return true;}
     char*MakeMessageKID(const char*,const char*){return strdup("offline");}
     char*PerformWebQuery(const char*,const char*,const char*,const char*){KATMED_ACME_PROBE(2);return strdup("artificial-certificate-file");}
     void ReadCertificate(){certificate=(void*)1;}
     int getAccountKey(){return havekeys?1:0;}int getCertificateKey(){return havekeys?1:0;}
     void GenerateAccountKey(){KATMED_ACME_PROBE(1);havekeys=true;}void GenerateCertificateKey(){KATMED_ACME_PROBE(1);}
     bool CreateNewAccount(){KATMED_ACME_PROBE(2);return true;}
     bool HaveValidCertificate(time_t){return certificate!=nullptr;}
     bool AcmeProcess(time_t);return_status ValidateOrder();bool DownloadCertificate();void RenewCertificate();
    };
    static Client fake,*client=&fake;static bool changed;
    bool katmed_acme_start(const KatmedAcmeConfig*){return true;}
    void katmed_acme_poll(uint64_t){changed=client->AcmeProcess(1800000000);}
    bool katmed_acme_certificate_changed(){bool c=changed;changed=false;return c;}
    void katmed_acme_stop(){client->Cleanup();}
    '''
    bodies='\n'.join(method(up,x).replace('Acme::','Client::') for x in ['bool Acme::AcmeProcess(', 'Acme::return_status Acme::ValidateOrder()', 'bool Acme::DownloadCertificate()', 'void Acme::RenewCertificate()'])
    stub=stub.replace('FAILURES',str(failures))
    if delayed_delete:
     pre=pre.replace('free_bytes+=12288;++deleted;', 'tick+=5000000;free_bytes+=12288;++deleted;')
    observer=ac[ac.index('static unsigned probe_cycle;'):ac.index('\n#else\nbool katmed_acme_resource_probe_launch')]
    main=r'''
    int main(){assert(katmed_acme_resource_probe_launch());
     for(unsigned c=1;c<=3;++c){
      tick+=13000000;katmed_acme_resource_probe_observe();assert(probe_worker);
      try{probe_task(nullptr);}catch(int){}
      suspended=true;katmed_acme_resource_probe_observe();assert(deleted==(int)c);
      tick+=13000000;katmed_acme_resource_probe_observe();
     }
     free(fake.order->status);
    }
    '''
    return life.Lifecycle().compile_run(pre+stub+bodies+observer+main)


def replace_field(line, key, value):
    return re.sub(key.encode()+rb'=[0-9]+', key.encode()+b'='+str(value).encode(), line)


def mutate(raw, transform):
    return b'\n'.join(transform(line) for line in raw.splitlines())+b'\n'


class Ownerfix4(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.control=client_capture()
        cls.retry=client_capture(1)
        cls.delayed=client_capture(delayed_delete=True)
        # Reuse the existing real hook/observer producer with changing heaps.
        # No manually composed positive capture can satisfy these regressions.
        from unittest.mock import patch
        original=life.Lifecycle.compile_run
        outputs=[]
        def capture(test, source, args=()):
            out=original(test,source,args)
            if b'katmed_acme_window:' in out: outputs.append(out)
            return out
        with patch.object(life.Lifecycle,'compile_run',capture):
            owner3.Ownerfix3().test_actual_operation_and_cpu_producer_complete_retries_and_aggregation()
        cls.lower=outputs[0]
        import os
        if os.environ.get('KATMED_ACME_TEST_ARTIFACTS'):
            dest=Path(os.environ['KATMED_ACME_TEST_ARTIFACTS'])
            dest.mkdir(parents=True,exist_ok=True)
            for name in ('control','retry','delayed','lower'):
                (dest/(name+'.log')).write_bytes(getattr(cls,name))

    def assert_invalid(self, raw):
        with self.assertRaises(ram.RamBenchError):
            ram.evaluate_resource(raw,'client')

    def test_actual_worker_client_control_and_same_phase_download_retry(self):
        for raw in (self.control,self.retry):
            self.assertEqual(3,raw.count(b'phase=released status=ok'))
            self.assertEqual('source_capture_valid',ram.evaluate_resource(raw,'client')['qualification'])
        self.assertIn(b'op=download attempt=1 event=end status=error',self.retry)
        self.assertIn(b'op=download attempt=2 event=end status=ok',self.retry)

    def test_actual_delayed_delete_is_covered_and_shortened_cpu_is_rejected(self):
        self.assertEqual('source_capture_valid',ram.evaluate_resource(self.delayed,'client')['qualification'])
        cleanups={}
        for line in self.delayed.splitlines():
            if b'phase=cleanup ' in line:
                kv=dict(x.split(b'=') for x in line.split(b': ')[1].split())
                cleanups[int(kv[b'cycle'])]=int(kv[b'uptime_ms'])*1000
        def shorten(line):
            if b'katmed_acme_cpu:' in line and b'window=active ' in line:
                kv=dict(x.split(b'=') for x in line.split(b': ')[1].split())
                end=cleanups[int(kv[b'cycle'])]+1000
                start=int(kv[b'start_us']);idle=int(kv[b'idle_start'])
                for k,v in {'end_us':end,'elapsed_us':end-start,'idle_delta':(end-start)//2,'idle_end':idle+(end-start)//2}.items():
                    line=replace_field(line,k,v)
            return line
        self.assert_invalid(mutate(self.delayed,shorten))

    def test_operations_outside_worker_or_renewal_are_rejected(self):
        for only in (None,b'op=challenge '):
            def shift(line):
                if b'katmed_acme_window:' in line and (only is None or only in line):
                    for k in ('start_us','end_us'):
                        value=int(re.search(k.encode()+rb'=([0-9]+)',line)[1])+10**12
                        line=replace_field(line,k,value)
                return line
            for raw in (self.control,self.retry,self.lower):
                self.assert_invalid(mutate(raw,shift))

    def test_download_end_cannot_move_before_actual_write_close_parse_samples(self):
        def short(line):
            if b'op=download ' in line and b'event=end' in line:
                seq=int(re.search(rb'start_seq=([0-9]+)',line)[1])
                start=int(re.search(rb'start_us=([0-9]+)',line)[1])
                for k,v in {'end_seq':seq,'end_us':start+1,'max_gap_us':1}.items():
                    line=replace_field(line,k,v)
            return line
        for raw in (self.control,self.retry,self.lower):
            self.assert_invalid(mutate(raw,short))
        # Sequence-only repair cannot hide a shortened timestamp either.
        def time_only(line):
            damaged=short(line)
            if damaged != line:
                damaged=replace_field(damaged,'end_seq',int(re.search(rb'end_seq=([0-9]+)',line)[1]))
            return damaged
        self.assert_invalid(mutate(self.retry,time_only))

    def test_aggregates_cannot_erase_lower_shared_observations(self):
        begins={}
        changed=0
        def inflate(line):
            nonlocal changed
            if b'katmed_acme_window:' in line:
                kv=dict(x.split(b'=') for x in line.split(b': ')[1].split())
                key=kv[b'cycle'],kv[b'op'],kv[b'attempt']
                if kv[b'event']==b'begin': begins[key]=kv
                else:
                    old=line
                    for metric in ('internal_min','dma_min','internal_block_observed','dma_block_observed'):
                        line=replace_field(line,metric,int(begins[key][metric.encode()]))
                    changed+=old!=line
            return line
        bad=mutate(self.lower,inflate)
        self.assertGreater(changed,0)
        self.assert_invalid(bad)

    def test_reordered_marks_and_missing_microsecond_binding_are_rejected(self):
        lines=self.control.splitlines()
        i=next(i for i,l in enumerate(lines) if b'op=download ' in l and b'event=end' in l)
        j=next(j for j,l in enumerate(lines) if b'op=download ' in l and b'event=begin' in l)
        moved=lines[:];moved.insert(j,moved.pop(i))
        self.assert_invalid(b'\n'.join(moved)+b'\n')
        self.assert_invalid(re.sub(rb' sample_us=[0-9]+',b'',self.control))
        self.assert_invalid(self.retry.replace(b'op=download attempt=2',b'op=download attempt=3'))
