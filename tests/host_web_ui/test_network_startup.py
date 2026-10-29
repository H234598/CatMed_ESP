"""Run production Web startup against the IDF socket-init prerequisite.

Regression: fresh unprovisioned boot reaches Web without Mesh. Removing the
platform initialization, or accepting its failure, must prevent a ready Web
listener. No Wi-Fi profiles, real sockets, private keys or devices are used.
"""
import datetime
import json
import time
import subprocess
import tempfile
import unittest
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get("KATMED_WEB_SOURCE", ROOT / "firmware/components/katmed_web/katmed_web.c"))


def function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PREFIX = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define NVS_READWRITE 1
#define WEB_TAG "test"
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){.server_port=80,.ctrl_port=32768})
#define HTTPD_SSL_CONFIG_DEFAULT() ((httpd_ssl_config_t){.httpd=HTTPD_DEFAULT_CONFIG()})
#define CONFIG_ESP_TLS_SERVER_MIN_AUTH_MODE_OPTIONAL 1
#define HTTP_GET 0
typedef unsigned psa_key_id_t;
typedef int psa_status_t;
typedef struct { int unused; } psa_key_attributes_t;
typedef struct { int source; struct { unsigned key_id; } psa; } esp_key_config_t;
#define PSA_KEY_ID_NULL 0
#define PSA_SUCCESS 0
#define PSA_KEY_ATTRIBUTES_INIT ((psa_key_attributes_t){0})
#define PSA_KEY_TYPE_ECC_KEY_PAIR(x) (x)
#define PSA_ECC_FAMILY_SECP_R1 1
#define PSA_KEY_USAGE_SIGN_HASH 1
#define PSA_ALG_SHA_256 1
#define PSA_ALG_ECDSA(x) (x)
/* Lifecycle doubles only; actual policy and handshake use the installed
 * library in test_tls_server_signature_policy.py. */
#define PSA_ALG_DETERMINISTIC_ECDSA(x) (0x100 | (x))
#define MBEDTLS_PK_ALG_ECDSA(x) PSA_ALG_DETERMINISTIC_ECDSA(x)
#define ESP_KEY_SOURCE_PSA 2
static void psa_set_key_type(psa_key_attributes_t *a,int v){(void)a;(void)v;}
static void psa_set_key_bits(psa_key_attributes_t *a,int v){(void)a;(void)v;}
static void psa_set_key_usage_flags(psa_key_attributes_t *a,int v){(void)a;(void)v;}
static void psa_set_key_algorithm(psa_key_attributes_t *a,int v){(void)a;(void)v;}
static void psa_reset_key_attributes(psa_key_attributes_t *a){(void)a;}
static psa_status_t psa_crypto_init(void){return PSA_SUCCESS;}
static psa_status_t psa_import_key(const psa_key_attributes_t *a,const void *key,size_t n,psa_key_id_t *id){(void)a;(void)key;(void)n;*id=1;return PSA_SUCCESS;}
static int key_destroys;
static psa_status_t psa_destroy_key(psa_key_id_t id){(void)id;++key_destroys;return PSA_SUCCESS;}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"startup contract failed: %s\n",#x);return 1;}} while(0)
typedef struct {
 int max_open_sockets,max_uri_handlers,backlog_conn,recv_wait_timeout,send_wait_timeout;
 int server_port,ctrl_port,stack_size;bool lru_purge_enable;void *uri_match_fn;
} httpd_config_t;
typedef struct {
 httpd_config_t httpd;int tls_handshake_timeout_ms;const void *servercert,*prvtkey_pem,*cacert_pem;
 size_t servercert_len,prvtkey_len,cacert_len;void *user_cb;bool client_cert_authmode_optional;
 const esp_key_config_t *server_key;
} httpd_ssl_config_t;
typedef struct {const char *uri;int method;void *handler,*user_ctx;} httpd_uri_t;
typedef struct {void *context;bool (*prepare_listener_stop)(void *);} KatmedWebServices;
static struct {
 KatmedWebServices services;bool started,nvs_open,restart_pending;uint64_t retry_due_ms,restart_due_ms;int nvs,certificate_fd;
 struct {unsigned char cert[1024],key[256];size_t cert_len,key_len;} identity;
 psa_key_id_t tls_key_id;esp_key_config_t tls_key_config;
 bool certificate_verified;unsigned char certificate_fingerprint[32];
 struct {struct {size_t cert_len;const void *client_cert;} record;} auth;
 void *auth_mutex,*https,*redirect;
} web;
static bool tcpip_ready,init_fails,https_stop_fails;
static bool https_start_fails,redirect_start_fails,route_fails,post_admission_fails;
static int https_stops,storage_closes,mutex_deletes;
static int listener_attempts,uninitialized_listener_attempts,nvs_opens;
static httpd_config_t https_httpd_config, redirect_httpd_config;
static uint64_t monotonic_ms(void){return 0;}
static void web_socket_diag_sample(uint64_t now_ms,bool force){(void)now_ms;(void)force;}
static int esp_netif_init(void) {
 if(init_fails)return ESP_FAIL;
 tcpip_ready=true;return ESP_OK;
}
static bool resources_allow(bool post,const char *site){(void)site;return !post || !post_admission_fails;}
static int nvs_open(const char *name,int mode,int *out){(void)name;(void)mode;*out=1;++nvs_opens;return ESP_OK;}
static bool load_auth(void *state){(void)state;return true;}
static bool load_identifier(void *state){(void)state;return true;}
static bool katmed_web_identity_load_or_create(int nvs,void *out){(void)nvs;(void)out;return true;}
static void *xSemaphoreCreateMutex(void){return (void*)1;}
static int httpd_ssl_start(void **handle,const httpd_ssl_config_t *cfg){
 https_httpd_config=cfg->httpd;++listener_attempts;
 if(!tcpip_ready){++uninitialized_listener_attempts;return ESP_FAIL;}
 if(https_start_fails)return ESP_FAIL;
 *handle=(void*)2;return ESP_OK;
}
static int httpd_start(void **handle,const httpd_config_t *cfg){
 redirect_httpd_config=*cfg;++listener_attempts;
 if(!tcpip_ready){++uninitialized_listener_attempts;return ESP_FAIL;}
 if(redirect_start_fails)return ESP_FAIL;
 *handle=(void*)3;return ESP_OK;
}
static bool register_routes(void *server){return server!=NULL && !route_fails;}
static int httpd_register_uri_handler(void *server,const httpd_uri_t *uri){(void)uri;return server?ESP_OK:ESP_FAIL;}
static int httpd_stop(void *server){(void)server;return ESP_OK;}
static int httpd_ssl_stop(void *server){(void)server;++https_stops;return https_stop_fails?ESP_FAIL:ESP_OK;}
static void wipe(void *ptr,size_t len){memset(ptr,0,len);}
static void nvs_close(int nvs){(void)nvs;++storage_closes;}
static void vSemaphoreDelete(void *mutex){(void)mutex;++mutex_deletes;}
bool katmed_web_stop(void);
#define pdTRUE 1
#define portMAX_DELAY 1
static int xSemaphoreTake(void *lock,int wait){(void)lock;(void)wait;return pdTRUE;}
static void xSemaphoreGive(void *lock){(void)lock;}
static void katmed_web_auth_tick(void *auth,uint64_t now){(void)auth;(void)now;}
static void *tls_session_callback,*katmed_web_plain_uri_match,*redirect_get;
'''
SUFFIX = r'''
int main(int argc,char **argv){
 CHECK(argc>=2);
 if(!strcmp(argv[1],"fresh_without_wifi")){
  /* Fresh Store: no provisioned Mesh, no network/interface/IP or Wi-Fi profile. */
  tcpip_ready=false;
  CHECK(katmed_web_start(NULL));
  CHECK(web.started && web.https && web.redirect);
  CHECK(uninitialized_listener_attempts==0);
 }else if(!strcmp(argv[1],"already_initialized_mesh")){
  tcpip_ready=true;
  CHECK(katmed_web_start(NULL));
  CHECK(web.started && web.https && web.redirect);
  int existing=listener_attempts;
  CHECK(katmed_web_start(NULL));CHECK(listener_attempts==existing);
  CHECK(uninitialized_listener_attempts==0);
 }else if(!strcmp(argv[1],"initialization_failure")){
  init_fails=true;tcpip_ready=false;
  CHECK(!katmed_web_start(NULL));
  CHECK(!web.started && !web.https && !web.redirect && !web.nvs_open);
  CHECK(listener_attempts==0 && nvs_opens==0);
 }else if(!strcmp(argv[1],"intentional_disabled")){
  CHECK(katmed_web_start(NULL));int before=listener_attempts;
  katmed_web_set_enabled(false);CHECK(!web.started);
  katmed_web_poll(UINT64_C(900000));CHECK(listener_attempts==before);
  CHECK(!katmed_web_start(NULL));CHECK(listener_attempts==before);
  katmed_web_set_enabled(true);katmed_web_poll(UINT64_C(900001));
  CHECK(web.started && listener_attempts>before);
 }else if(!strcmp(argv[1],"target_listener_capacity")){
  CHECK(argc==3);CHECK(katmed_web_start(NULL));
  printf("ACTUAL_POLICY https=%d http=%d\n",https_httpd_config.max_open_sockets,redirect_httpd_config.max_open_sockets);
  CHECK(https_httpd_config.max_open_sockets==atoi(argv[2]));
  CHECK(redirect_httpd_config.max_open_sockets==1);
  CHECK(https_httpd_config.lru_purge_enable && https_httpd_config.backlog_conn==1);
  CHECK(https_httpd_config.recv_wait_timeout==2 && https_httpd_config.send_wait_timeout==2);
  CHECK(https_httpd_config.max_uri_handlers==35);
  CHECK(redirect_httpd_config.server_port==80 && redirect_httpd_config.ctrl_port!=https_httpd_config.ctrl_port);
 }else if(!strcmp(argv[1],"start_failure_cleanup")){
  CHECK(argc==3);web.identity.key_len=32;
  https_start_fails=!strcmp(argv[2],"https");redirect_start_fails=!strcmp(argv[2],"redirect");
  route_fails=!strcmp(argv[2],"routes");post_admission_fails=!strcmp(argv[2],"admission");
  CHECK(!katmed_web_start(NULL));
  CHECK(!web.https && !web.redirect && web.tls_key_id==PSA_KEY_ID_NULL);
  CHECK(key_destroys==1 && !web.nvs_open && !web.auth_mutex);
 }else if(!strcmp(argv[1],"fullstack_socket_capacity")){
  CHECK(argc==4);
  CHECK(katmed_web_start(NULL));
  CHECK(web.started && listener_attempts==2);
  CHECK(https_httpd_config.max_open_sockets>=1);
  CHECK(redirect_httpd_config.max_open_sockets>=1);
  CHECK(redirect_httpd_config.server_port==80);
  CHECK(https_httpd_config.ctrl_port!=redirect_httpd_config.ctrl_port);
  /* Four background slots are a Host-supported allowance, not an owner census.
   * Keep both complete SDK listener internals and the actual client policy. */
  int demand=4+2*atoi(argv[3])+https_httpd_config.max_open_sockets+
             redirect_httpd_config.max_open_sockets;
  CHECK(atoi(argv[2])>=demand);
 }else if(!strcmp(argv[1],"https_stop_failure_retains_state")){
  CHECK(katmed_web_start(NULL));
  web.tls_key_id=7;web.tls_key_config.source=ESP_KEY_SOURCE_PSA;
  web.tls_key_config.psa.key_id=7;
  web.identity.key[0]=42;web.auth.record.cert_len=1;
  web.restart_pending=true;web.restart_due_ms=0;
  https_stop_fails=true;
  int before=listener_attempts;
  katmed_web_poll(UINT64_C(1000));
  CHECK(https_stops==1 && web.https && web.tls_key_id==7);
  CHECK(key_destroys==0 && web.nvs_open && web.auth_mutex);
  CHECK(web.identity.key[0]==42 && web.auth.record.cert_len==1);
  CHECK(listener_attempts==before && storage_closes==0 && mutex_deletes==0);
  katmed_web_poll(UINT64_C(2000));
  CHECK(https_stops==1 && listener_attempts==before);
  CHECK(!katmed_web_stop());
  CHECK(https_stops==2 && web.https && web.tls_key_id==7);
  CHECK(web.nvs_open && web.auth_mutex && web.identity.key[0]==42);
  CHECK(key_destroys==0 && storage_closes==0 && mutex_deletes==0);
  https_stop_fails=false;
  CHECK(katmed_web_stop());
  CHECK(https_stops==3 && !web.https && !web.redirect);
  CHECK(web.tls_key_id==PSA_KEY_ID_NULL && key_destroys==1);
  CHECK(!web.nvs_open && !web.auth_mutex && storage_closes==1 && mutex_deletes==1);
  CHECK(web.identity.key[0]==0 && web.auth.record.cert_len==0);
  CHECK(!web.restart_pending);
  CHECK(katmed_web_stop());CHECK(key_destroys==1);
 }else return 2;
 puts("production Web startup prerequisite PASS");return 0;
}
'''


class NetworkStartup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        text = SOURCE.read_text()
        c = Path(cls.directory.name) / "startup.c"
        c.write_text(PREFIX + "\nstatic bool management_enabled=true;\n" + function(text, "static bool stop_listeners(void)" if "static bool stop_listeners(void)" in text else "static void stop_listeners(void)") + "\n" + function(text, "static bool start_listeners(void)") +
                     "\n" + function(text, "void katmed_web_set_enabled(") + "\n" + function(text, "bool katmed_web_start(") + "\n" + function(text, "void katmed_web_poll(") + "\n" + function(text, "bool katmed_web_stop(void)") + SUFFIX)
        cls.executables = {}
        for target in ("esp32c3", "esp32c6"):
            binary = Path(cls.directory.name) / target
            execute_tls_callback(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                  "-Wno-unused-function",
                                  "-DCONFIG_IDF_TARGET_ESP32C6=" + str(int(target == "esp32c6")),
                                  "-DCONFIG_IDF_TARGET_ESP32C3=" + str(int(target == "esp32c3")),
                                  str(c), "-o", str(binary)])
            cls.executables[target] = binary

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_case(self, name, *arguments, target="esp32c3"):
        execute_tls_callback([str(self.executables[target]), name, *arguments])

    def test_c6_preserves_first_connection_with_two_client_slots(self):
        self.run_case("target_listener_capacity", "2", target="esp32c6")

    def test_c3_listener_capacity_remains_one(self):
        self.run_case("target_listener_capacity", "1", target="esp32c3")

    def test_c6_thirteen_slot_fullstack_envelope(self):
        self.run_case("fullstack_socket_capacity", "13", "3", target="esp32c6")

    def test_c6_start_failures_release_key_and_both_listeners(self):
        for failure in ("https", "redirect", "routes", "admission"):
            with self.subTest(failure=failure):
                self.run_case("start_failure_cleanup", failure, target="esp32c6")

    def test_fresh_unprovisioned_without_wifi_starts_both_listeners(self):
        self.run_case("fresh_without_wifi")

    def test_already_initialized_mesh_keeps_idempotent_listener_start(self):
        self.run_case("already_initialized_mesh")

    def test_intentional_disabled_blocks_start_and_poll_retry(self):
        self.run_case("intentional_disabled")

    def test_failed_platform_initialization_opens_no_listener_or_store(self):
        self.run_case("initialization_failure")

    def check_resolved_fullstack_socket_capacity(self, config_path, target):
        config = Path(config_path).read_text()
        self.assertIn(f'CONFIG_IDF_TARGET="{target}"', config)
        capacity = re.search(r"^CONFIG_LWIP_MAX_SOCKETS=(\d+)$", config, re.M)
        self.assertIsNotNone(capacity)
        idf_path = Path(os.environ["IDF_PATH"])
        httpd_main = (idf_path / "components/esp_http_server/src/httpd_main.c").read_text()
        internal = re.search(
            r"HTTPD_MAX_SOCKETS\s*<\s*config->max_open_sockets\s*\+\s*(\d+)",
            httpd_main,
        )
        self.assertIsNotNone(internal)
        self.assertIn("cs_create_ctrl_sock", httpd_main)
        self.assertIn("msg_fd = socket", httpd_main)
        self.run_case("fullstack_socket_capacity", capacity.group(1), internal.group(1), target=target)

    def test_resolved_c3_profile_can_accept_one_client_on_each_web_listener(self):
        self.check_resolved_fullstack_socket_capacity(
            ROOT / "firmware/build-c3/sdkconfig", "esp32c3")

    def test_c6_profile_can_accept_one_client_on_each_web_listener(self):
        config_path = os.environ.get("KATMED_C6_SDKCONFIG")
        if not config_path:
            self.skipTest("requires a freshly generated normal C6 Development001 sdkconfig")
        self.check_resolved_fullstack_socket_capacity(config_path, "esp32c6")

    def test_https_stop_failure_keeps_key_and_callers_until_success(self):
        self.run_case("https_stop_failure_retains_state")


# Synthetic execution of the actual TLS callback, not a model of its policy.
TLS_BEHAVIOR_ONLY = os.environ.get("KATMED_TLS_DIAGNOSTICS_BEHAVIOR_ONLY") == "1"

TLS_DIAGNOSTIC_HOST = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 258
#define ESP_ERR_INVALID_STATE 259
#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_8BIT 2u
#define MALLOC_CAP_DMA 4u
#define WEB_TAG "synthetic"
typedef int esp_err_t;
typedef struct {int unused;} esp_tls_t;
typedef struct {int unused;} mbedtls_ssl_context;
typedef struct cert {struct cert *next;struct {unsigned char *p;size_t len;} raw;} mbedtls_x509_crt;
enum {HTTPD_SSL_USER_CB_SESS_CREATE,HTTPD_SSL_USER_CB_SESS_CLOSE,HTTPD_SSL_USER_CB_SESS_ERROR};
typedef struct {int user_cb_state;esp_tls_t *tls;} esp_https_server_user_cb_arg_t;
static struct {int certificate_fd;bool certificate_verified;unsigned char certificate_fingerprint[32];void *https;struct {struct {size_t cert_len;unsigned char client_fingerprint[32];} record;} auth;} web;
static size_t internal_free,dma_free,dma_largest;
static int fd_result,close_result,closes,last_close_fd,cert_checks,fd_calls;
static int samples[3];
void diagnostic(const char *tag,const char *format,...){
    (void)tag;va_list args;va_start(args,format);fputs("LOG ",stdout);vprintf(format,args);putchar('\n');va_end(args);
}
#define ESP_LOGI(...) diagnostic(__VA_ARGS__)
#define ESP_LOGW(...) diagnostic(__VA_ARGS__)
static int esp_tls_get_conn_sockfd(esp_tls_t *tls,int *fd){(void)tls;++fd_calls;if(fd_result==ESP_OK)*fd=17;return fd_result;}
static void wipe(void *p,size_t n){memset(p,0,n);}
static size_t heap_caps_get_free_size(uint32_t caps){
    if(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)){++samples[0];return internal_free;}
    if(caps==(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)){++samples[1];return dma_free;}
    abort();
}
static size_t heap_caps_get_largest_free_block(uint32_t caps){if(caps!=7u)abort();++samples[2];return dma_largest;}
static int httpd_sess_trigger_close(void *server,int fd){if(server!=(void*)1)abort();++closes;last_close_fd=fd;return close_result;}
static mbedtls_ssl_context *esp_tls_get_ssl_context(esp_tls_t *tls){(void)tls;++cert_checks;return NULL;}
static unsigned mbedtls_ssl_get_verify_result(mbedtls_ssl_context *s){(void)s;return 0;}
static const mbedtls_x509_crt *mbedtls_ssl_get_peer_cert(mbedtls_ssl_context *s){(void)s;return NULL;}
static bool certificate_time_valid(const mbedtls_x509_crt *c){(void)c;return true;}
static int sha256_bytes(const unsigned char *p,size_t n,unsigned char *o){(void)p;(void)n;(void)o;return 0;}
static bool katmed_web_auth_equal(const void *a,const void *b,size_t n){return memcmp(a,b,n)==0;}
'''
TLS_DIAGNOSTIC_MAIN = r'''
int main(int argc,char **argv){
    if(argc!=9)return 2;
    int event=atoi(argv[1]);internal_free=strtoul(argv[2],NULL,10);dma_free=strtoul(argv[3],NULL,10);dma_largest=strtoul(argv[4],NULL,10);
    web.https=atoi(argv[5])?(void*)1:NULL;close_result=atoi(argv[6]);fd_result=atoi(argv[7]);
    web.certificate_fd=17;web.certificate_verified=true;memset(web.certificate_fingerprint,0xa5,32);web.auth.record.cert_len=1;
    esp_tls_t tls={0};esp_https_server_user_cb_arg_t arg={.user_cb_state=event,.tls=atoi(argv[8])?&tls:NULL};
    tls_session_callback(event==9?NULL:&arg);
    unsigned remaining=0;for(size_t i=0;i<32;++i)remaining|=web.certificate_fingerprint[i];
    printf("STATE %d %d %d %d %d %u %d %d %d %d\n",closes,last_close_fd,cert_checks,web.certificate_fd,web.certificate_verified,remaining,fd_calls,samples[0],samples[1],samples[2]);
    return 0;
}
'''


def execute_tls_callback(argv):
    start = datetime.datetime.now(datetime.timezone.utc).isoformat()
    mono = time.monotonic_ns()
    # Callers supply only the fixed compiler argv or this test's fresh synthetic binary.
    child = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)  # nosec B603 - no shell or external input.
    out, err = child.communicate()
    print(json.dumps({"PID": child.pid, "startUTC": start,
                      "endUTC": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      "start_monotonic_ns": mono, "end_monotonic_ns": time.monotonic_ns(),
                      "Exit": child.returncode, "ChildEnd": True,
                      "streams_closed": child.stdout.closed and child.stderr.closed}))
    if child.returncode:
        raise AssertionError(err.decode() or out.decode())
    return out.decode()


class TlsSessionDiagnostics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = str(Path(cls.temp.name) / "callback")
        source = Path(cls.temp.name) / "callback.c"
        text = SOURCE.read_text()
        floors = re.findall(r"^#define LIVE_FREE_(?:INTERNAL|DMA)\b.*$", text, re.M)
        if len(floors) != 2:
            raise AssertionError("Both production TLS admission floors are required")
        source.write_text(TLS_DIAGNOSTIC_HOST + "\n".join(floors) + "\n" +
                          function(text, "static void tls_session_callback(") + TLS_DIAGNOSTIC_MAIN)
        execute_tls_callback(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", cls.binary])

    def case(self, event=0, internal=12288, dma=4096, largest=1024,
             server=1, close_return=0, fd_return=0, tls=1):
        out = execute_tls_callback([self.binary, *map(str, [event, internal, dma, largest,
                                             server, close_return, fd_return, tls])])
        state = next(line for line in out.splitlines() if line.startswith("STATE "))
        return list(map(int, state.split()[1:])), [line[4:] for line in out.splitlines() if line.startswith("LOG ")]

    def test_equality_and_above_floors_keep_certificate_path(self):
        for values in [(12288, 4096, 1024), (12289, 4097, 1025)]:
            with self.subTest(values=values):
                state, logs = self.case(internal=values[0], dma=values[1], largest=values[2])
                self.assertEqual(state[:7], [0, 0, 1, 17, 0, 0, 1])
                self.assertEqual(state[7:], [1, 1, 1])
                if not TLS_BEHAVIOR_ONLY:
                    self.assertEqual(logs, ["TLS hook event=CREATE fd=17 fd_result=0"])

    def test_each_rejection_and_decision_vs_later_samples(self):
        cases = [(12287, 4096, 1024, "INTERNAL_FREE", 0, 0),
                 (12288, 4095, 1024, "DMA_FREE", 1, 0),
                 (12288, 4096, 1023, "DMA_LARGEST", 1, 1)]
        for internal, dma, largest, reason, decision_dma, decision_largest in cases:
            with self.subTest(reason=reason):
                state, logs = self.case(internal=internal, dma=dma, largest=largest)
                self.assertEqual(state[:7], [1, 17, 0, 17, 0, 0, 1])
                if not TLS_BEHAVIOR_ONLY:
                    self.assertEqual(state[7:], [1, 1, 1], "Each operand sampled once, no predicate re-read")
                    self.assertEqual(logs, ["TLS hook event=CREATE fd=17 fd_result=0",
                        f"TLS resource reject fd=17 reason={reason} internal_free={internal}/12288 dma_free={dma}/4096 dma_largest={largest}/1024 decision_dma_free={decision_dma} decision_dma_largest={decision_largest} samples_sequential=1",
                        "TLS close request fd=17 result=0"])

    def test_close_result_is_not_observed_close_and_no_server_is_distinct(self):
        state, logs = self.case(internal=1, close_return=-1)
        self.assertEqual(state[:3], [1, 17, 0])
        if not TLS_BEHAVIOR_ONLY:
            self.assertEqual(logs[-1:], ["TLS close request fd=17 result=-1"])
            self.assertFalse(any("event=CLOSE" in line for line in logs))
        state, logs = self.case(internal=1, server=0)
        self.assertEqual(state[:3], [0, 0, 0])
        if not TLS_BEHAVIOR_ONLY:
            self.assertEqual(logs[-1:], ["TLS close request fd=17 state=NO_SERVER"])

    def test_close_preserves_certificate_cleanup_without_heap_or_new_close(self):
        state, logs = self.case(event=1, internal=0, dma=0, largest=0)
        self.assertEqual(state, [0, 0, 0, -1, 0, 0, 1, 0, 0, 0])
        if not TLS_BEHAVIOR_ONLY:
            self.assertEqual(logs, ["TLS hook event=CLOSE fd=17 fd_result=0"])

    def test_error_is_error_and_retains_existing_nonclose_branch(self):
        state, logs = self.case(event=2)
        self.assertEqual(state[:7], [0, 0, 1, 17, 0, 0, 1])
        if not TLS_BEHAVIOR_ONLY:
            self.assertEqual(logs, ["TLS hook event=ERROR fd=17 fd_result=0"])

    def test_invalid_inputs_do_not_touch_auth_heap_or_close(self):
        for event, fd_return, tls, fd_calls in [(9, 0, 1, 0), (2, 0, 0, 0), (0, -1, 1, 1)]:
            with self.subTest(event=event, tls=tls, fd_return=fd_return):
                state, logs = self.case(event=event, fd_return=fd_return, tls=tls)
                self.assertEqual(state, [0, 0, 0, 17, 1, 165, fd_calls, 0, 0, 0])
                if not TLS_BEHAVIOR_ONLY:
                    if event == 9:
                        expected = []
                    elif not tls:
                        expected = ["TLS hook event=ERROR fd=-1 fd_state=NO_TLS"]
                    else:
                        expected = ["TLS hook event=CREATE fd=-1 fd_result=-1"]
                    self.assertEqual(logs, expected)



# Real callback/request authorization path under the newly concurrent C6 policy.
# Only SDK transport, certificate primitives and session-context storage are doubles.
CERTIFICATE_SESSION_HOST = r"""
#define CONFIG_IDF_TARGET_ESP32C6 1
#include <assert.h>
typedef struct {int fd;} httpd_req_t;
static void *session_contexts[2];
static void (*context_free[2])(void *);
static int synthetic_fd=17;
static bool peer_present=true,allocation_fails;
static mbedtls_ssl_context synthetic_ssl;
static unsigned char synthetic_der=1;
static mbedtls_x509_crt synthetic_peer={.raw={&synthetic_der,1}};
static int allocations,releases;
static void *certificate_alloc(size_t count,size_t size){
    assert(count*size==32);++allocations;
    return allocation_fails?NULL:calloc(count,size);
}
static void certificate_release(void *p){
    const unsigned char *bytes=p;
    for(size_t i=0;i<32;++i)assert(bytes[i]==0);
    ++releases;free(p);
}
#define calloc certificate_alloc
#define free certificate_release
static int httpd_req_to_sockfd(httpd_req_t *req){return req->fd;}
static void *httpd_sess_get_ctx(void *server,int fd){
    assert(server==(void*)1 && fd>=17 && fd<=18);return session_contexts[fd-17];
}
static void httpd_sess_set_ctx(void *server,int fd,void *ctx,void (*release)(void *)){
    assert(server==(void*)1 && fd>=17 && fd<=18);int i=fd-17;
    if(session_contexts[i] && session_contexts[i]!=ctx)context_free[i](session_contexts[i]);
    session_contexts[i]=ctx;context_free[i]=release;
}
static void end_session(int fd){
    int i=fd-17;if(session_contexts[i])context_free[i](session_contexts[i]);
    session_contexts[i]=NULL;context_free[i]=NULL;
}
"""
CERTIFICATE_SESSION_MAIN = r"""
static void create_session(int fd,bool certificate){
    synthetic_fd=fd;peer_present=certificate;
    esp_tls_t tls={0};esp_https_server_user_cb_arg_t arg={HTTPD_SSL_USER_CB_SESS_CREATE,&tls};
    tls_session_callback(&arg);
}
int main(int argc,char **argv){
    assert(argc==2);web.https=(void*)1;web.auth.record.cert_len=1;
    memset(web.auth.record.client_fingerprint,0x2a,32);
    internal_free=32768;dma_free=16384;dma_largest=8192;
    httpd_req_t a={17},b={18};unsigned char out[32];
    if(!strcmp(argv[1],"allocation_failure")){
        allocation_fails=true;create_session(17,true);
        assert(!certificate_on_request(&a,out));assert(allocations==1 && releases==0);
    }else{
        create_session(17,true);assert(certificate_on_request(&a,out));
        create_session(18,false);
        assert(certificate_on_request(&a,out));assert(!certificate_on_request(&b,out));
        if(!strcmp(argv[1],"pin_replaced")){
            web.auth.record.client_fingerprint[0]^=1;
            assert(!certificate_on_request(&a,out));
        }else if(!strcmp(argv[1],"other_close")){
            end_session(18);synthetic_fd=18;esp_tls_t tls={0};
            esp_https_server_user_cb_arg_t arg={HTTPD_SSL_USER_CB_SESS_CLOSE,&tls};
            tls_session_callback(&arg);assert(certificate_on_request(&a,out));
        }
        end_session(17);assert(!certificate_on_request(&a,out));
        assert(releases==1);
    }
    end_session(17);end_session(18);
    printf("certificate session case=%s PASS\n",argv[1]);return 0;
}
"""


class C6CertificateSessionIsolation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = str(Path(cls.temp.name) / "certificate-session")
        text = SOURCE.read_text()
        host = TLS_DIAGNOSTIC_HOST.replace(
            "*fd=17;", "*fd=synthetic_fd;")
        # Declare before the reused SDK doubles, define storage in the suffix.
        host = host.replace("static int fd_result,", "static int synthetic_fd;\nstatic int fd_result,")
        host = host.replace("(void)tls;++cert_checks;return NULL;",
                            "(void)tls;++cert_checks;return &synthetic_ssl;")
        host = host.replace("(void)s;return NULL;", "(void)s;return peer_present?&synthetic_peer:NULL;")
        host = host.replace("(void)p;(void)n;(void)o;return 0;",
                            "(void)p;(void)n;memset(o,0x2a,32);return 0;")
        # Fixture declarations must precede functions referencing them.
        marker = "static int esp_tls_get_conn_sockfd("
        before, after = host.split(marker, 1)
        fixture = CERTIFICATE_SESSION_HOST.replace("static int synthetic_fd=17;", "")
        optional = ""
        if "static void free_certificate_session(" in text:
            start = text.index("typedef struct {\n    uint8_t fingerprint[32];")
            end = text.index("} CertificateSession;", start) + len("} CertificateSession;")
            optional = text[start:end] + "\n" + function(text, "static void free_certificate_session(")
        floors = re.findall(r"^#define LIVE_FREE_(?:INTERNAL|DMA)\b.*$", text, re.M)
        source = before + fixture + "\n" + marker + after + "\n" + optional + "\n" + "\n".join(floors)
        source += "\n#include <inttypes.h>\n" + ENDPOINT_SOCKET_HOST + endpoint_source(text)
        source += "\n" + function(text, "static void tls_session_callback(")
        source += "\n" + function(text, "static bool certificate_on_request(") + CERTIFICATE_SESSION_MAIN
        path = Path(cls.temp.name) / "certificate-session.c"
        path.write_text(source)
        execute_tls_callback(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(path), "-o", cls.binary])

    def test_verified_peer_survives_second_password_only_connection(self):
        execute_tls_callback([self.binary, "isolation"])

    def test_other_session_close_does_not_revoke_verified_peer(self):
        execute_tls_callback([self.binary, "other_close"])

    def test_replaced_pin_is_not_accepted_from_cached_session(self):
        execute_tls_callback([self.binary, "pin_replaced"])

    def test_allocation_failure_denies_certificate_login(self):
        execute_tls_callback([self.binary, "allocation_failure"])


# Private endpoint diagnostics: actual production callback/helper with synthetic sockets.
ENDPOINT_SOCKET_HOST = r"""
#include <arpa/inet.h>
#include <sys/socket.h>
static int endpoint_mode,endpoint_queries;
static uint64_t monotonic_ms(void){return 5000;}
static int endpoint_query(int fd,struct sockaddr *out,socklen_t *length,bool peer){
    ++endpoint_queries;
    if(endpoint_mode==10 && endpoint_queries==4)fd_result=-1;
    if(fd<0 || endpoint_mode==1)return -1;
    struct sockaddr_storage storage;memset(&storage,0xa5,sizeof(storage));
    if(endpoint_mode==2){storage.ss_family=AF_UNIX;memcpy(out,&storage,sizeof(storage));return 0;}
    if(endpoint_mode==3){*length=1;return 0;}
    if(endpoint_mode==11){*length=sizeof(storage)+1;return 0;}
    if(endpoint_mode==5 || endpoint_mode==6){
        struct sockaddr_in6 value={.sin6_family=AF_INET6,.sin6_port=htons(peer?45000:443)};
        inet_pton(AF_INET6,endpoint_mode==5?(peer?"::ffff:192.0.2.8":"::ffff:192.0.2.9"):(peer?"2001:db8::8":"2001:db8::9"),&value.sin6_addr);
        memcpy(&storage,&value,sizeof(value));*length=sizeof(value);
    }else if(endpoint_mode==7){
        struct sockaddr_in6 value={.sin6_family=AF_INET6,.sin6_scope_id=2,.sin6_port=htons(443)};
        inet_pton(AF_INET6,"fe80::1",&value.sin6_addr);memcpy(&storage,&value,sizeof(value));*length=sizeof(value);
    }else{
        struct sockaddr_in value={.sin_family=AF_INET,.sin_port=htons(peer?45000:443)};
        inet_pton(AF_INET,peer?"192.0.2.8":"192.0.2.9",&value.sin_addr);
        if(endpoint_mode==4 && endpoint_queries>2)value.sin_port=htons(45001);
        if(endpoint_mode==8)value.sin_port=0;
        memcpy(&storage,&value,sizeof(value));*length=sizeof(value);
    }
    memcpy(out,&storage,sizeof(storage));return 0;
}
static int endpoint_peer(int fd,struct sockaddr *out,socklen_t *len){return endpoint_query(fd,out,len,true);}
static int endpoint_local(int fd,struct sockaddr *out,socklen_t *len){return endpoint_query(fd,out,len,false);}
#define getpeername endpoint_peer
#define getsockname endpoint_local
"""


def endpoint_source(text):
    marker = "/* Private endpoint correlation:"
    if marker not in text:
        return ""
    start = text.index(marker)
    end = text.index("#endif /* C6 endpoint correlation */", start)
    return text[start:end] + "#endif\n"


ENDPOINT_MAIN = r"""
int main(int argc,char **argv){
    if(argc!=2)return 2;
    endpoint_mode=atoi(argv[1]);
    internal_free=32768;dma_free=16384;dma_largest=8192;
    web.auth.record.cert_len=0;fd_result=endpoint_mode==9?-1:0;
    esp_tls_t tls={0};esp_https_server_user_cb_arg_t arg={HTTPD_SSL_USER_CB_SESS_CREATE,&tls};
    tls_session_callback(&arg);
    if(closes || cert_checks)return 3;
    printf("QUERIES %d\n",endpoint_queries);return 0;
}
"""


class TlsEndpointCorrelation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = str(Path(cls.temp.name) / "endpoint")
        text = SOURCE.read_text()
        floors = re.findall(r"^#define LIVE_FREE_(?:INTERNAL|DMA)\b.*$", text, re.M)
        # A no-certificate path exercises the real CREATE producer, not auth doubles.
        host = TLS_DIAGNOSTIC_HOST + "\n#define CONFIG_IDF_TARGET_ESP32C6 1\n#include <inttypes.h>\n" + ENDPOINT_SOCKET_HOST
        if "} CertificateSession;" in text:
            start = text.index("typedef struct {\n    uint8_t fingerprint[32];")
            end = text.index("} CertificateSession;", start) + len("} CertificateSession;")
            host += text[start:end] + "\n" + function(text, "static void free_certificate_session(")
            host += "\nstatic void httpd_sess_set_ctx(void *s,int fd,void *v,void (*f)(void *)){(void)s;(void)fd;(void)v;(void)f;}\n"
        path = Path(cls.temp.name) / "endpoint.c"
        path.write_text(host + "\n" + "\n".join(floors) + "\n" + endpoint_source(text) + "\n" +
                        function(text, "static void tls_session_callback(") + ENDPOINT_MAIN)
        execute_tls_callback(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(path), "-o", cls.binary])

    def case(self, mode):
        return execute_tls_callback([self.binary, str(mode)])

    def test_actual_create_emits_canonical_ipv4_without_padding(self):
        out = self.case(0)
        self.assertIn("TLS endpoint schema=1 valid=1 fd=17 boot_ms=5000 local_family=4 local_addr=c0000209000000000000000000000000 local_port=443 peer_family=4 peer_addr=c0000208000000000000000000000000 peer_port=45000", out)
        self.assertIn("QUERIES 4", out)
        self.assertNotIn("a5a5", out)

    def test_mapped_ipv6_is_same_canonical_ipv4(self):
        self.assertIn("local_family=4 local_addr=c0000209000000000000000000000000", self.case(5))

    def test_native_ipv6_is_explicit_bytes_not_sockaddr_padding(self):
        self.assertIn("local_family=6 local_addr=20010db8000000000000000000000009", self.case(6))

    def test_query_family_partial_mutation_scope_zero_port_are_unknown(self):
        for mode in [1,2,3,4,7,8,10,11]:
            with self.subTest(mode=mode):
                out=self.case(mode)
                self.assertIn("TLS endpoint schema=1 valid=0",out)
                self.assertNotIn("local_addr=",out)
                self.assertNotIn("peer_addr=",out)

    def test_failed_tls_fd_lookup_does_not_query_or_emit_endpoints(self):
        out=self.case(9)
        self.assertIn("QUERIES 0",out)
        self.assertNotIn("TLS endpoint",out)

if __name__ == "__main__":
    unittest.main()
