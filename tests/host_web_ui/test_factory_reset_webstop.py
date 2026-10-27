"""Exercise the real Factory Reset main-loop caller with the real Web stop and journal.

Only the IDF/HTTP/NVS edges are synthetic. The reset confirmation, main-loop
reset step, Web listener stop, and journal state machine come from production.
"""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_network_startup import PREFIX, function


ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "firmware/main/main.c"
WEB = ROOT / "firmware/components/katmed_web/katmed_web.c"
JOURNAL = ROOT / "firmware/components/katmed_reset/katmed_reset_journal.c"
WEB_RESET_HEADER = ROOT / "firmware/components/katmed_web/include/katmed_web_reset.h"

EDGES = r'''
#define pdMS_TO_TICKS(n) (n)
typedef struct {
 bool reset_warning_ready,reset_requested,reset_active,store_ready;
 bool reset_prepared,reset_web_offer,reset_web_rejected;
 uint64_t reset_retry_due_ms;
 KatmedResetConfirmation reset_confirmation;
 int names_nvs,nvs;
} Runtime;
static void *reset_mutex=(void *)1;
static KatmedResetJournal journal;
static int journal_saves,erase_stages,restarts,mesh_stops,names_closes,store_closes;
static bool acme_ready=true,service_ready=true;
static bool journal_load(void *ctx,KatmedResetJournal *out){(void)ctx;*out=journal;return true;}
static bool journal_save(void *ctx,const KatmedResetJournal *next){
 (void)ctx;journal=*next;++journal_saves;return true;
}
static bool journal_run_stage(void *ctx,KatmedResetStage stage){
 (void)ctx;(void)stage;++erase_stages;return true;
}
static KatmedResetJournalBackend katmed_reset_idf_backend(void){
 return (KatmedResetJournalBackend){NULL,journal_load,journal_save,journal_run_stage};
}
static bool katmed_acme_prepare_listener_stop(void){return acme_ready;}
static bool web_service_prepare(void *ctx){(void)ctx;return service_ready;}
static void katmed_mesh_stop(void){++mesh_stops;}
static void names_idf_close(int *nvs){(void)nvs;++names_closes;}
static void store_idf_close_backend(int *nvs){(void)nvs;++store_closes;}
static void esp_restart(void){++restarts;}
static void esp_fill_random(void *out,size_t length){
 unsigned char *p=out;for(size_t i=0;i<length;i++)p[i]=(unsigned char)(i+1);
}
static bool reset_random_bytes(void *ctx,uint8_t *out,size_t length){
 (void)ctx;esp_fill_random(out,length);return true;
}
static void setup_web(void){
 web.services.prepare_listener_stop=web_service_prepare;
 web.https=(void *)2;web.redirect=(void *)3;web.started=true;
 web.tls_key_id=7;web.tls_key_config.source=ESP_KEY_SOURCE_PSA;
 web.tls_key_config.psa.key_id=7;
 web.nvs_open=true;web.auth_mutex=(void *)1;
 web.identity.key[0]=42;web.auth.record.cert_len=1;
}
static int exercise(const char *mode){
 Runtime rt={0};char token[33]={0};size_t token_len=0;
 CHECK(!katmed_runtime_reset_prepare(&rt,token,sizeof(token),&token_len));
 rt.reset_warning_ready=true; /* The existing physical-reset one-boot gate. */
 CHECK(katmed_runtime_reset_prepare(&rt,token,sizeof(token),&token_len));
 CHECK(token_len==32 && !rt.reset_requested);
 CHECK(katmed_runtime_reset_confirm(&rt,"wrong",5)==1 && !rt.reset_requested);
 CHECK(katmed_runtime_reset_confirm(&rt,token,token_len)==0);
 CHECK(katmed_runtime_reset_confirm(&rt,token,token_len)==1);
 CHECK(web_reset_status(&rt)==KATMED_WEB_RESET_STOP_PENDING);
 setup_web();
 if(!strcmp(mode,"https_failure"))https_stop_fails=true;
 if(!strcmp(mode,"acme_failure"))acme_ready=false;
 if(!strcmp(mode,"second_barrier_failure"))service_ready=false;
 CHECK(reset_runtime_step(&rt,1000));
 if(strcmp(mode,"success")){
  CHECK(journal_saves==0 && !journal.pending && erase_stages==0);
  CHECK(restarts==0 && mesh_stops==0 && names_closes==0 && store_closes==0);
  CHECK(web.https && web.tls_key_id==7 && key_destroys==0);
  CHECK(web.nvs_open && web.auth_mutex && web.identity.key[0]==42);
  CHECK(!rt.reset_active);
  CHECK(web_reset_status(&rt)==KATMED_WEB_RESET_STOP_PENDING);
  https_stop_fails=false;acme_ready=true;service_ready=true;
  CHECK(reset_runtime_step(&rt,2000));
 }
 CHECK(journal_saves>0 && !journal.pending && erase_stages>0);
 CHECK(web_reset_status(&rt)==KATMED_WEB_RESET_COMMITTED);
 CHECK(restarts==1 && mesh_stops==1 && names_closes==1 && store_closes==1);
 CHECK(!web.https && web.tls_key_id==PSA_KEY_ID_NULL && key_destroys==1);
 return 0;
}
int main(int argc,char **argv){
 CHECK(argc==2);return exercise(argv[1]);
}
'''


class FactoryResetWebStop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.temp.name) / "factory_reset_webstop"
        main = MAIN.read_text()
        web = WEB.read_text()
        reset_header = WEB_RESET_HEADER.read_text()
        enum_start = reset_header.index("typedef enum {")
        enum_end = reset_header.index("} KatmedWebResetStatus;", enum_start) + len("} KatmedWebResetStatus;")
        status_enum = reset_header[enum_start:enum_end]
        stop_sig = "bool katmed_web_stop(void)" if "bool katmed_web_stop(void)" in web else "void katmed_web_stop(void)"
        prefix = PREFIX.replace("void katmed_web_stop(void);", stop_sig + ";")
        prefix = prefix.replace(
            "static void *tls_session_callback,*katmed_web_plain_uri_match,*redirect_get;", ""
        )
        text = ('#include "katmed_reset_journal.h"\n' + prefix + status_enum + '\n' +
                EDGES.split('static void setup_web(void)')[0] +
                function(main, "static KatmedResetJournalBackend reset_backend(void)") + '\n' +
                function(main, "bool katmed_runtime_reset_prepare(") + '\n' +
                function(main, "int katmed_runtime_reset_confirm(") + '\n' +
                function(main, "static bool web_prepare_listener_stop(") + '\n' +
                function(main, "static KatmedWebResetStatus web_reset_status(") + '\n' +
                function(web, "static bool stop_listeners(void)") + '\n' +
                function(web, stop_sig) + '\n' +
                function(main, "static bool reset_runtime_step(") + '\n' +
                'static void setup_web(void)' + EDGES.split('static void setup_web(void)', 1)[1])
        source = Path(cls.temp.name) / "fixture.c"
        source.write_text(text)
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-I", str(ROOT / "firmware/components/katmed_reset/include"),
            str(source), str(JOURNAL), "-o", str(cls.exe)
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_failed_https_stop_does_not_commit_journal_or_erase(self):
        self.run_case("https_failure")

    def test_failed_acme_barrier_does_not_commit_journal_or_erase(self):
        self.run_case("acme_failure")

    def test_failed_web_service_barrier_does_not_commit_journal_or_erase(self):
        self.run_case("second_barrier_failure")

    def test_successful_stop_keeps_physical_confirmation_and_erases(self):
        self.run_case("success")

    def run_case(self, mode):
        result = subprocess.run([str(self.exe), mode], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
