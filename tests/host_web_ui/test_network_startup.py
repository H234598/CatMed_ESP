"""Run production Web startup against the IDF socket-init prerequisite.

Regression: fresh unprovisioned boot reaches Web without Mesh. Removing the
platform initialization, or accepting its failure, must prevent a ready Web
listener. No Wi-Fi profiles, real sockets, private keys or devices are used.
"""
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
static int https_stops,storage_closes,mutex_deletes;
static int listener_attempts,uninitialized_listener_attempts,nvs_opens;
static httpd_config_t https_httpd_config, redirect_httpd_config;
static uint64_t monotonic_ms(void){return 0;}
static void web_socket_diag_sample(uint64_t now_ms,bool force){(void)now_ms;(void)force;}
static int esp_netif_init(void) {
 if(init_fails)return ESP_FAIL;
 tcpip_ready=true;return ESP_OK;
}
static bool resources_allow(bool post){(void)post;return true;}
static int nvs_open(const char *name,int mode,int *out){(void)name;(void)mode;*out=1;++nvs_opens;return ESP_OK;}
static bool load_auth(void *state){(void)state;return true;}
static bool katmed_web_identity_load_or_create(int nvs,void *out){(void)nvs;(void)out;return true;}
static void *xSemaphoreCreateMutex(void){return (void*)1;}
static int httpd_ssl_start(void **handle,const httpd_ssl_config_t *cfg){
 https_httpd_config=cfg->httpd;++listener_attempts;
 if(!tcpip_ready){++uninitialized_listener_attempts;return ESP_FAIL;}
 *handle=(void*)2;return ESP_OK;
}
static int httpd_start(void **handle,const httpd_config_t *cfg){
 redirect_httpd_config=*cfg;++listener_attempts;
 if(!tcpip_ready){++uninitialized_listener_attempts;return ESP_FAIL;}
 *handle=(void*)3;return ESP_OK;
}
static bool register_routes(void *server){return server!=NULL;}
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
 }else if(!strcmp(argv[1],"c6_socket_capacity")){
  CHECK(argc==4);
  CHECK(katmed_web_start(NULL));
  CHECK(web.started && listener_attempts==2);
  CHECK(https_httpd_config.max_open_sockets>=1);
  CHECK(redirect_httpd_config.max_open_sockets>=1);
  CHECK(redirect_httpd_config.server_port==80);
  CHECK(https_httpd_config.ctrl_port!=redirect_httpd_config.ctrl_port);
  /* Four active slots were measured before both listeners on normal C6. */
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
        cls.exe = Path(cls.directory.name) / "startup"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", str(c), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_case(self, name, *arguments):
        result = subprocess.run([str(self.exe), name, *arguments], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_fresh_unprovisioned_without_wifi_starts_both_listeners(self):
        self.run_case("fresh_without_wifi")

    def test_already_initialized_mesh_keeps_idempotent_listener_start(self):
        self.run_case("already_initialized_mesh")

    def test_intentional_disabled_blocks_start_and_poll_retry(self):
        self.run_case("intentional_disabled")

    def test_failed_platform_initialization_opens_no_listener_or_store(self):
        self.run_case("initialization_failure")

    def test_c6_profile_can_accept_one_client_on_each_web_listener(self):
        config_path = os.environ.get("KATMED_C6_SDKCONFIG")
        if not config_path:
            self.skipTest("requires a freshly generated normal C6 Development001 sdkconfig")
        config = Path(config_path).read_text()
        self.assertIn('CONFIG_IDF_TARGET="esp32c6"', config)
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
        self.run_case("c6_socket_capacity", capacity.group(1), internal.group(1))

    def test_https_stop_failure_keeps_key_and_callers_until_success(self):
        self.run_case("https_stop_failure_retains_state")


if __name__ == "__main__":
    unittest.main()
