"""Run real Host/Origin, authenticated handlers and NVS update code with public edges."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_network_startup import function

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "firmware/components/katmed_web/katmed_web.c"

PREFIX = r'''
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "cJSON.h"
#include "katmed_web_policy.h"
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define ESP_FAIL -1
#define portMAX_DELAY 0
#define KATMED_WEB_TOKEN_HEX 64
typedef int esp_err_t;
typedef struct {char csrf[65];} KatmedWebSession;
typedef struct {int nvs; bool nvs_open,identifier_ready; void *auth_mutex,*redirect,*https;
 char acme_identifier[KATMED_WEB_IDENTIFIER_MAX];} WebContext;
static WebContext web;
typedef struct {void *handle; const char *host,*origin,*csrf,*body; bool authenticated;
 char status[40],response[600],location[300];} httpd_req_t;
static char durable[254],pending[254];
static bool exists,writer=true,fail_set,fail_commit;
static unsigned saves,commits;
static unsigned untouched_auth=17,untouched_cert=23,untouched_pref=41;
static int nvs_get_str(int h,const char *key,char *out,size_t *size){
 (void)h;assert(!strcmp(key,"acme_id"));if(!exists)return ESP_ERR_NVS_NOT_FOUND;
 if(*size<strlen(durable)+1)return ESP_FAIL;
 strcpy(out,durable);*size=strlen(durable)+1;return ESP_OK;
}
static int nvs_set_str(int h,const char *key,const char *value){
 (void)h;assert(!strcmp(key,"acme_id"));++saves;if(fail_set)return ESP_FAIL;
 strcpy(pending,value);return ESP_OK;
}
static int nvs_commit(int h){(void)h;++commits;if(fail_commit)return ESP_FAIL;
 strcpy(durable,pending);exists=true;return ESP_OK;}
static bool katmed_source_writer_enter(void){return writer;}
static void katmed_source_writer_leave(void){}
static void xSemaphoreTake(void *m,int delay){assert(m);(void)delay;}
static void xSemaphoreGive(void *m){assert(m);}
static int httpd_req_to_sockfd(httpd_req_t *r){(void)r;return 7;}
static int test_getsockname(int fd,struct sockaddr *out,socklen_t *len){
 (void)fd;struct sockaddr_in x={.sin_family=AF_INET};inet_pton(AF_INET,"192.0.2.42",&x.sin_addr);
 memcpy(out,&x,sizeof(x));*len=sizeof(x);return 0;
}
#define getsockname test_getsockname
static const char *header(httpd_req_t *r,const char *key){
 if(!strcmp(key,"Host"))return r->host;
 if(!strcmp(key,"Origin"))return r->origin;
 if(!strcmp(key,"X-Katmed-CSRF"))return r->csrf;
 return NULL;
}
static size_t httpd_req_get_hdr_value_len(httpd_req_t *r,const char *k){const char *v=header(r,k);return v?strlen(v):0;}
static int httpd_req_get_hdr_value_str(httpd_req_t *r,const char *k,char *out,size_t n){
 const char *v=header(r,k);if(!v || strlen(v)>=n)return ESP_FAIL;strcpy(out,v);return ESP_OK;
}
static KatmedWebSession session;
static KatmedWebSession *session_from_request(httpd_req_t *r,char *token){(void)token;return r->authenticated?&session:NULL;}
static bool katmed_web_auth_equal(const void *a,const void *b,size_t n){return !memcmp(a,b,n);}
static int error_reply(httpd_req_t *r,const char *s,const char *e){strcpy(r->status,s);strcpy(r->response,e);return 0;}
static int json_reply(httpd_req_t *r,const char *s,cJSON *obj){char *t=cJSON_PrintUnformatted(obj);
 strcpy(r->status,s);strcpy(r->response,t);free(t);cJSON_Delete(obj);return 0;}
static cJSON *read_json(httpd_req_t *r){return cJSON_Parse(r->body);}
static void wipe(void *p,size_t n){memset(p,0,n);}
static void httpd_resp_set_status(httpd_req_t *r,const char *s){strcpy(r->status,s);}
static void httpd_resp_set_hdr(httpd_req_t *r,const char *k,const char *v){if(!strcmp(k,"Location"))strcpy(r->location,v);}
static int httpd_resp_send(httpd_req_t *r,const char *p,int n){(void)r;(void)p;(void)n;return 0;}
'''

CASES = r'''
static httpd_req_t request(void){return (httpd_req_t){.handle=web.https,.host="192.0.2.42",
 .origin="https://192.0.2.42",.csrf=session.csrf,.authenticated=true,
 .body="{\"identifier\":\"device.example.invalid\",\"previous\":\"\"}"};}
int main(void){
 web.nvs=1;web.nvs_open=true;web.auth_mutex=(void *)1;web.https=(void *)2;web.redirect=(void *)3;
 memset(session.csrf,'a',64);session.csrf[64]=0;
 assert(load_identifier(&web));assert(!web.acme_identifier[0]);assert(!exists);
 char name[254];assert(katmed_web_acme_identifier(name,sizeof(name)) && !name[0]);
 httpd_req_t r=request();identifier_get(&r);assert(!strcmp(r.response,"{\"identifier\":\"\"}"));
 r=request();r.authenticated=false;identifier_put(&r);assert(!strcmp(r.status,"401 Unauthorized") && saves==0);
 r=request();r.authenticated=false;identifier_get(&r);assert(!strcmp(r.status,"401 Unauthorized") && saves==0);
 r=request();r.csrf="bad";identifier_put(&r);assert(!strcmp(r.response,"csrf") && saves==0);
 r=request();r.origin="https://foreign.example.invalid";identifier_put(&r);assert(!strcmp(r.response,"origin") && saves==0);
 r=request();r.host="device.example.invalid";identifier_put(&r);assert(!strcmp(r.response,"host") && saves==0);
 r=request();r.body="{\"identifier\":\"bad.invalid:443\",\"previous\":\"\"}";identifier_put(&r);assert(!strcmp(r.status,"422 Unprocessable Entity") && saves==0);
 r=request();r.body="{\"identifier\":\"device.example.invalid\"}";identifier_put(&r);assert(!strcmp(r.status,"422 Unprocessable Entity"));
 r=request();r.body="{\"identifier\":\"device.example.invalid\",\"previous\":\"\",\"other\":true}";identifier_put(&r);assert(!strcmp(r.status,"422 Unprocessable Entity"));
 r=request();r.body="{\"identifier\":\"device.example.invalid\",\"identifier\":\"evil.invalid\",\"previous\":\"\"}";identifier_put(&r);assert(!strcmp(r.status,"422 Unprocessable Entity"));
 r=request();identifier_put(&r);assert(!strcmp(r.status,"200 OK"));assert(saves==1 && commits==1);
 assert(!strcmp(durable,"device.example.invalid"));
 memset(web.acme_identifier,0,sizeof(web.acme_identifier));assert(load_identifier(&web));
 assert(katmed_web_acme_identifier(name,sizeof(name)) && !strcmp(name,durable));
 char host[254];assert(katmed_web_resolve_authority("192.0.2.42",name,"DEVICE.EXAMPLE.INVALID:443",443,host));
 assert(!strcmp(host,name));assert(katmed_web_same_origin(host,"https://DEVICE.EXAMPLE.INVALID:443"));
 assert(!katmed_web_same_origin(host,"https://192.0.2.42"));
 const char *bad[]={"foreign.example.invalid","device.example.invalid.attacker.invalid","device.example.invalid:80","device.example.invalid:0443","device.example.invalid@evil.invalid","device.example.invalid\r\nX: y"};
 for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i)assert(!katmed_web_resolve_authority("192.0.2.42",name,bad[i],443,host));
 const char *invalid[]={"*.example.invalid","-bad.example.invalid","bad-.example.invalid","bad..invalid","bad.invalid.","192.0.2.42","https://device.example.invalid","a_b.invalid","x/evil.invalid"};
 for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)assert(!katmed_web_identifier_valid(invalid[i]));
 assert(katmed_web_identifier_valid(""));assert(katmed_web_identifier_valid(name));
 char long_label[90];memset(long_label,'a',64);strcpy(long_label+64,".invalid");
 assert(!katmed_web_identifier_valid(long_label));
 assert(!katmed_web_same_origin(name,"https://device.example.invalid.attacker.invalid"));
 assert(!katmed_web_same_origin(name,"https://device.example.invalid:80"));
 assert(!katmed_web_same_origin(name,"https://device.example.invalid/"));
 r=request();r.host="device.example.invalid";r.origin="https://192.0.2.42";identifier_put(&r);assert(!strcmp(r.response,"origin"));
 r=request();r.handle=web.redirect;r.host="device.example.invalid:80";redirect_get(&r);
 assert(!strcmp(r.status,"308 Permanent Redirect") && !strcmp(r.location,"https://device.example.invalid/"));
 r=request();r.handle=web.redirect;redirect_get(&r);assert(!strcmp(r.location,"https://192.0.2.42/"));
 r=request();identifier_put(&r);assert(!strcmp(r.status,"409 Conflict"));
 r=request();r.host="device.example.invalid";r.origin="https://device.example.invalid";
 r.body="{\"identifier\":\"\",\"previous\":\"device.example.invalid\"}";identifier_put(&r);assert(!strcmp(r.status,"200 OK"));
 assert(load_identifier(&web) && !web.acme_identifier[0]);
 r=request();r.host="device.example.invalid";identifier_get(&r);assert(!strcmp(r.response,"host"));
 r=request();writer=false;identifier_put(&r);assert(!strcmp(r.status,"503 Service Unavailable"));writer=true;
 assert(!web.acme_identifier[0]);
 fail_commit=true;r=request();identifier_put(&r);assert(!strcmp(r.status,"503 Service Unavailable"));
 assert(!katmed_web_acme_identifier(name,sizeof(name)));fail_commit=false;assert(load_identifier(&web));
 assert(!web.acme_identifier[0]);
 assert(untouched_auth==17 && untouched_cert==23 && untouched_pref==41);
 puts("real identifier persistence/API/auth/origin/redirect: PASS");return 0;
}
'''


class IdentifierTest(unittest.TestCase):
    def test_production_acme_uses_configured_identifier(self):
        source = (ROOT / "firmware/components/katmed_acme/katmed_acme.cpp").read_text()
        prefix = r'''
#include <atomic>
#include <cstring>
#include <strings.h>
#include <cassert>
#include <cstdio>
#include "katmed_acme.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define KATMED_ACME_RESOURCE_CLIENT 0
typedef void *httpd_handle_t;
static const char *configured="";
static bool ready=true;
static std::atomic<void *> client_owner{nullptr};
static std::atomic<bool> acme_abort{false};
static bool certificate_changed;
static char challenge_host[254];
static void *xTaskGetCurrentTaskHandle(){return (void *)1;}
static bool katmed_web_acme_identifier(char *out,size_t cap){
 if(!ready || strlen(configured)>=cap)return false;strcpy(out,configured);return true;}
static bool katmed_web_acme_host_matches(const char *a,const char *b){return !strcmp(a,b);}
static bool katmed_source_writer_enter(){return true;}
static void katmed_source_writer_leave(){}
static httpd_handle_t katmed_web_acme_http_server(){return (void *)2;}
static size_t test_strlcpy(char *d,const char *s,size_t n){assert(strlen(s)<n);strcpy(d,s);return strlen(s);}
#define strlcpy test_strlcpy
struct Acme {
 const char *url=nullptr;
 void setUrl(const char *s){url=s;}
 void setEmail(const char *){} void setAcmeServer(const char *){}
 void setFilenamePrefix(const char *){} void setAccountFilename(const char *){}
 void setDirectoryFilename(const char *){} void setAccountKeyFilename(const char *){}
 void setOrderFilename(const char *){} void setCertKeyFilename(const char *){}
 void setCertificateFilename(const char *){} void setWebServer(httpd_handle_t){}
};
static Acme *client;
'''
        cases = r'''
int main(){
 KatmedAcmeConfig c={nullptr,"fixture@example.invalid","https://ca.example.invalid/directory","/katmed/acme"};
 assert(!katmed_acme_start(nullptr));assert(!katmed_acme_start(&c));
 configured="device.example.invalid";c.identifier="foreign.example.invalid";
 assert(!katmed_acme_start(&c));c.identifier=nullptr;
 assert(katmed_acme_start(&c));assert(client && !strcmp(client->url,configured));
 assert(client->url==challenge_host); /* library retains pointer after return */
 delete client;client=nullptr;client_owner.store(nullptr);
 ready=false;assert(!katmed_acme_start(&c));
 puts("real production ACME desired identifier and lifetime: PASS");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / "acme.cpp"
            c.write_text(prefix + function(source, "bool katmed_acme_start(") + cases)
            exe = Path(tmp) / "acme"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-Wno-misleading-indentation",
                            "-I" + str(ROOT / "firmware/components/katmed_acme/include"),
                            str(c), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_real_handlers_and_storage(self):
        source = WEB.read_text()
        signatures = ["static bool load_identifier(", "bool katmed_web_acme_identifier(",
                      "static bool save_identifier(", "static bool request_host(",
                      "static bool same_origin(", "static KatmedWebSession *require_session(",
                      "static void wipe_json(", "static bool keys_only(",
                      "static const char *json_string(", "static esp_err_t redirect_get(",
                      "static esp_err_t identifier_reply(", "static esp_err_t identifier_get(",
                      "static esp_err_t identifier_put("]
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / "identifier.c"
            c.write_text(PREFIX + "\n".join(function(source, sig) for sig in signatures) + CASES)
            json_dir = ROOT / "firmware/managed_components/espressif__cjson/cJSON"
            exe = Path(tmp) / "identifier"
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(json_dir),
                            "-I" + str(ROOT / "firmware/components/katmed_web/include"),
                            str(c), str(ROOT / "firmware/components/katmed_web/katmed_web_policy.c"),
                            str(json_dir / "cJSON.c"), "-lm", "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
