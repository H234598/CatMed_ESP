"""Real setup GET -> Main owner -> reloaded Store, using the existing host backend."""
import os
import tempfile
import unittest
from pathlib import Path

from test_network_startup import execute_tls_callback, function

ROOT = Path(__file__).resolve().parents[2]

HTTP = r'''
#define KATMED_WEB_TOKEN_HEX 64
#define KATMED_WEB_IDENTIFIER_MAX 64
#define BRIDGE_TIMEOUT_MS 1500
typedef int esp_err_t;
struct host_httpd_req {int unused;};
typedef struct {char csrf[KATMED_WEB_TOKEN_HEX+1];} KatmedWebSession;
typedef struct {int unused;} cJSON;
static cJSON object;
static KatmedWebSession session;
static bool logged_in=true;
static const char *http_status;
static unsigned exchanges,source_fields,total_fields;
static char returned_source[64];
static bool request_host(httpd_req_t *r,char *out){(void)r;(void)out;return true;}
static KatmedWebSession *session_from_request(httpd_req_t *r,char *out){(void)r;(void)out;return logged_in?&session:NULL;}
static int error_reply(httpd_req_t *r,const char *status,const char *error){(void)r;(void)error;http_status=status;return ESP_OK;}
static bool same_origin(httpd_req_t *r){(void)r;return true;}
static size_t httpd_req_get_hdr_value_len(httpd_req_t *r,const char *k){(void)r;(void)k;return 0;}
static int httpd_req_get_hdr_value_str(httpd_req_t *r,const char *k,char *out,size_t n){(void)r;(void)k;(void)out;(void)n;return -1;}
static bool katmed_web_auth_equal(const void *a,const void *b,size_t n){return !memcmp(a,b,n);}
static void field(const char *key){
 assert(!strcmp(key,"configured") || !strcmp(key,"mode") || !strcmp(key,"clock_required") ||
        !strcmp(key,"setup_open") || !strcmp(key,"revision") || !strcmp(key,"time_source"));
 ++total_fields;
}
static cJSON *cJSON_CreateObject(void){return &object;}
static void cJSON_AddBoolToObject(cJSON *o,const char *k,bool v){(void)o;(void)v;field(k);}
static void cJSON_AddNumberToObject(cJSON *o,const char *k,int v){(void)o;(void)v;field(k);}
static void cJSON_AddStringToObject(cJSON *o,const char *k,const char *v){
 (void)o;field(k);if(!strcmp(k,"time_source")){
  ++source_fields;assert(strlen(v)<sizeof(returned_source));strcpy(returned_source,v);
 }
}
static int json_reply(httpd_req_t *r,const char *s,cJSON *o){(void)r;assert(o==&object);http_status=s;return ESP_OK;}
static KatmedConfigStatus setup_owner_exchange(const KatmedConfigRequest *r,KatmedConfigResponse *out,uint32_t timeout){
 assert(timeout==BRIDGE_TIMEOUT_MS && r->operation==KATMED_CONFIG_SETUP_GET);
 ++exchanges;config_execute(&runtime,r,out);return out->status;
}
#define katmed_config_exchange setup_owner_exchange
'''

MAIN = r'''
#undef katmed_config_exchange
static void get_source(const char *expected){
 httpd_req_t req={0};http_status=NULL;source_fields=total_fields=0;returned_source[0]=0;
 runtime_setup_get(&req);
 assert(http_status && !strcmp(http_status,"200 OK"));
 assert(source_fields==1 && total_fields==6 && !strcmp(returned_source,expected));
}
int main(void){
 committed_length=0;fail_commit=false;initialize_runtime();
 StoreProvision saved={.configured=true,.network_mode=STORE_NETWORK_ONLINE,
  .group={1},.secret={2},.ssid="fixture-network",.ssid_length=15,
  .password="fixture-password",.password_length=16};
 const char *hostname="ntp-readback-162.example.test";
 saved.time_source_length=(uint8_t)strlen(hostname);
 memcpy(saved.time_source,hostname,saved.time_source_length);
 assert(store_save_setup(&runtime.store,&saved)==STORE_OK);
 initialize_runtime(); /* Real decode from the existing committed byte backend. */
 assert(runtime.store.record.provision.configured);
 get_source(hostname);
 runtime.store.staged=runtime.store.record;
 memset(runtime.store.staged.provision.time_source,'x',63);
 runtime.store.staged.provision.time_source_length=63;
 get_source(hostname); /* Staging must not masquerade as committed state. */
 memset(saved.time_source,'a',63);saved.time_source_length=63;
 assert(store_save_setup(&runtime.store,&saved)==STORE_OK);
 initialize_runtime();char maximum[64];memset(maximum,'a',63);maximum[63]=0;
 get_source(maximum); /* Exact non-NUL-terminated 63-byte stored payload. */
 logged_in=false;unsigned before=exchanges;http_status=NULL;total_fields=0;
 httpd_req_t req={0};runtime_setup_get(&req);
 assert(http_status && !strcmp(http_status,"401 Unauthorized") && exchanges==before && !total_fields);
 logged_in=true;
 saved.time_source[0]='b';fail_commit=true;
 assert(store_save_setup(&runtime.store,&saved)==STORE_COMMIT_UNKNOWN);
 fail_commit=false;assert(runtime.store.uncertain);
 http_status=NULL;total_fields=0;runtime_setup_get(&req);
 assert(http_status && !strcmp(http_status,"503 Service Unavailable") && !total_fields);
 initialize_runtime();get_source(maximum); /* Failed write did not replace durable bytes. */
 saved=(StoreProvision){.configured=true,.network_mode=STORE_NETWORK_OFFLINE,.group={1},.secret={2}};
 assert(store_save_setup(&runtime.store,&saved)==STORE_OK);
 initialize_runtime();get_source("");
 committed_length=0;initialize_runtime();get_source("");
 puts("setup time_source: real authenticated GET/owner/reload/staging/63-byte/uncertain/secret exclusion PASS");
 return 0;
}
'''


class SetupTimeSource(unittest.TestCase):
    def test_committed_server_through_real_get_and_owner(self):
        web = (ROOT / "firmware/components/katmed_web/katmed_web.c").read_text()
        signatures = [
            "static KatmedWebSession *require_session(",
            "static esp_err_t bridge_error(httpd_req_t *req, KatmedConfigStatus status)\n{",
            "static esp_err_t runtime_setup_get(",
        ]
        with tempfile.TemporaryDirectory(prefix="katmed-setup-readback-") as temporary:
            out = Path(temporary)
            source = out / "setup.c"
            source.write_text(
                '#define main original_host_fixture_main\n#include "'
                + str(ROOT / "tests/host_store/check_main_submit.c")
                + '"\n#undef main\n'
                + HTTP + "\n".join(function(web, s) for s in signatures) + MAIN
            )
            includes = [ROOT / "tests/host_store/stubs", ROOT / "tests/host_main/stubs"]
            includes += sorted((ROOT / "firmware/components").glob("*/include"))
            includes += [ROOT / "firmware/main", Path(os.environ.get(
                "KATMED_HOST_LVGL_INCLUDE", ROOT / "firmware/managed_components/lvgl__lvgl"))]
            flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                     "-Wno-error=cpp", "-Wno-unused-variable", "-ffunction-sections", "-fdata-sections",
                     "-DCONFIG_IDF_TARGET_ESP32C6=1", "-DKATMED_FULL_STACK=1", "-DKATMED_WITH_CORE=1",
                     '-DLV_CONF_PATH="' + str(ROOT / "firmware/components/katmed_images/host/lv_conf.h") + '"',
                     "-DLV_KCONFIG_IGNORE", "-DLV_LVGL_H_INCLUDE_SIMPLE"]
            flags += ["-I" + str(p) for p in includes]
            c_sources = [source, ROOT / "firmware/components/katmed_ui/navigation.c",
                         ROOT / "firmware/components/katmed_ui/i18n_catalog.c",
                         ROOT / "firmware/components/katmed_reset/katmed_reset_journal.c",
                         ROOT / "firmware/components/katmed_time/katmed_time.c"]
            objects = []
            for number, c_source in enumerate(c_sources):
                obj = out / f"c{number}.o"
                self.run_child(["cc", *flags, "-c", str(c_source), "-o", str(obj)])
                objects.append(str(obj))
            bridge = out / "bridge.o"
            self.run_child(["cc", "-I" + str(ROOT / "tests/host_config_bridge/stubs"), *flags, "-pthread",
                            "-c", str(ROOT / "firmware/components/katmed_config_bridge/katmed_config_bridge.c"),
                            "-o", str(bridge)])
            objects.append(str(bridge))
            for component in ["katmed_core", "katmed_store"]:
                obj = out / f"{component}.o"
                self.run_child(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                "-ffunction-sections", "-fdata-sections", *["-I" + str(p) for p in includes],
                                "-c", str(ROOT / f"firmware/components/{component}/{component}.cpp"),
                                "-o", str(obj)])
                objects.append(str(obj))
            binary = out / "check"
            self.run_child(["c++", "-Wl,--gc-sections", *objects, "-pthread", "-lz", "-o", str(binary)])
            self.run_child([str(binary)])

    @staticmethod
    def run_child(argv):
        print(execute_tls_callback(argv), end="")
