"""Run production Web startup against the IDF socket-init prerequisite.

Regression: fresh unprovisioned boot reaches Web without Mesh. Removing the
platform initialization, or accepting its failure, must prevent a ready Web
listener. No Wi-Fi profiles, real sockets, private keys or devices are used.
"""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/components/katmed_web/katmed_web.c"


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
#define CHECK(x) do {if(!(x)){fprintf(stderr,"startup contract failed: %s\n",#x);return 1;}} while(0)
typedef struct {
 int max_open_sockets,max_uri_handlers,backlog_conn,recv_wait_timeout,send_wait_timeout;
 int server_port,ctrl_port,stack_size;bool lru_purge_enable;void *uri_match_fn;
} httpd_config_t;
typedef struct {
 httpd_config_t httpd;int tls_handshake_timeout_ms;const void *servercert,*prvtkey_pem,*cacert_pem;
 size_t servercert_len,prvtkey_len,cacert_len;void *user_cb;bool client_cert_authmode_optional;
} httpd_ssl_config_t;
typedef struct {const char *uri;int method;void *handler,*user_ctx;} httpd_uri_t;
typedef struct {void *context;bool (*prepare_listener_stop)(void *);} KatmedWebServices;
static struct {
 KatmedWebServices services;bool started,nvs_open,restart_pending;uint64_t retry_due_ms,restart_due_ms;int nvs,certificate_fd;
 struct {const void *cert,*key;size_t cert_len,key_len;} identity;
 struct {struct {size_t cert_len;const void *client_cert;} record;} auth;
 void *auth_mutex,*https,*redirect;
} web;
static bool tcpip_ready,init_fails;
static int listener_attempts,uninitialized_listener_attempts,nvs_opens;
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
 (void)cfg;++listener_attempts;
 if(!tcpip_ready){++uninitialized_listener_attempts;return ESP_FAIL;}
 *handle=(void*)2;return ESP_OK;
}
static int httpd_start(void **handle,const httpd_config_t *cfg){
 (void)cfg;++listener_attempts;
 if(!tcpip_ready){++uninitialized_listener_attempts;return ESP_FAIL;}
 *handle=(void*)3;return ESP_OK;
}
static bool register_routes(void *server){return server!=NULL;}
static int httpd_register_uri_handler(void *server,const httpd_uri_t *uri){(void)uri;return server?ESP_OK:ESP_FAIL;}
static void stop_listeners(void){web.https=NULL;web.redirect=NULL;web.started=false;}
static void katmed_web_stop(void){stop_listeners();web.nvs_open=false;}
#define pdTRUE 1
#define portMAX_DELAY 1
static int xSemaphoreTake(void *lock,int wait){(void)lock;(void)wait;return pdTRUE;}
static void xSemaphoreGive(void *lock){(void)lock;}
static void katmed_web_auth_tick(void *auth,uint64_t now){(void)auth;(void)now;}
static void *tls_session_callback,*katmed_web_plain_uri_match,*redirect_get;
'''
SUFFIX = r'''
int main(int argc,char **argv){
 CHECK(argc==2);
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
        c.write_text(PREFIX + "\nstatic bool management_enabled=true;\n" + function(text, "static bool start_listeners(void)") +
                     "\n" + function(text, "void katmed_web_set_enabled(") + "\n" + function(text, "bool katmed_web_start(") + "\n" + function(text, "void katmed_web_poll(") + SUFFIX)
        cls.exe = Path(cls.directory.name) / "startup"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", str(c), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_case(self, name):
        result = subprocess.run([str(self.exe), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_fresh_unprovisioned_without_wifi_starts_both_listeners(self):
        self.run_case("fresh_without_wifi")

    def test_already_initialized_mesh_keeps_idempotent_listener_start(self):
        self.run_case("already_initialized_mesh")

    def test_intentional_disabled_blocks_start_and_poll_retry(self):
        self.run_case("intentional_disabled")

    def test_failed_platform_initialization_opens_no_listener_or_store(self):
        self.run_case("initialization_failure")


if __name__ == "__main__":
    unittest.main()
