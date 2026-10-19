#include "katmed_web_wifi.h"
#include "katmed_web_reset.h"
#include "esp_err.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROUTE_CAP 16
static httpd_uri_t routes[ROUTE_CAP];
static size_t route_count;
static unsigned wipe_calls;
static bool wipe_nonzero;
static size_t wipe_lengths[64];
static size_t wipe_length_count;
void katmed_web_wifi_test_wipe_observer(const void *memory,size_t length){const unsigned char*p=memory;wipe_calls++;if(wipe_length_count<64)wipe_lengths[wipe_length_count++]=length;for(size_t i=0;i<length;i++)if(p[i]!=0)wipe_nonzero=true;}
static bool saw_wipe_length(size_t length){for(size_t i=0;i<wipe_length_count;i++)if(wipe_lengths[i]==length)return true;return false;}
static void assert_wiped_body(size_t body_length){assert(wipe_calls>=2);assert(!wipe_nonzero);assert(saw_wipe_length(body_length));assert(saw_wipe_length(8*sizeof(KatmedWebWifiProfileUpdate)));}

typedef struct {
    KatmedWebWifiProfileView view[8];
    char passwords[8][65];
    size_t count;
    bool fail_save;
    unsigned save_calls;
    unsigned reconnect_calls;
    unsigned list_calls;
    unsigned delete_calls;
} FakeWifi;

typedef struct {
    unsigned prepare_calls;
    unsigned confirm_calls;
    KatmedWebResetResult next_result;
    bool reset_applied;
} FakeReset;

esp_err_t httpd_resp_set_status(httpd_req_t *req,const char *status){snprintf(req->response_status,sizeof(req->response_status),"%s",status);return ESP_OK;}
esp_err_t httpd_resp_set_type(httpd_req_t *req,const char *type){(void)req;(void)type;return ESP_OK;}
esp_err_t httpd_resp_set_hdr(httpd_req_t *req,const char *name,const char *value){if(strcmp(name,"Connection")==0&&strcmp(value,"close")==0)req->connection_close=true;return ESP_OK;}
esp_err_t httpd_resp_send(httpd_req_t *req,const char *body,ssize_t length){size_t n=length==HTTPD_RESP_USE_STRLEN?strlen(body):(size_t)length;if(n>=sizeof(req->response))n=sizeof(req->response)-1;memcpy(req->response,body,n);req->response[n]=0;req->response_size=n;req->response_complete=true;return ESP_OK;}
esp_err_t httpd_resp_send_chunk(httpd_req_t *req,const char *body,ssize_t length){return httpd_resp_send(req,body,length);}
int httpd_req_recv(httpd_req_t *req,char *buffer,size_t length){if(req->body_offset>=req->body_size)return -1;size_t remain=req->body_size-req->body_offset;if(req->fail_after&&req->body_offset>=req->fail_after)return -1;if(length>remain)length=remain;if(req->fail_after&&req->body_offset+length>req->fail_after)length=req->fail_after-req->body_offset;if(!length)return -1;memcpy(buffer,req->body+req->body_offset,length);req->body_offset+=length;return (int)length;}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req,const char *name,char *out,size_t size){if(strcmp(name,"Content-Type")||!req->content_type)return ESP_FAIL;snprintf(out,size,"%s",req->content_type);return ESP_OK;}
esp_err_t httpd_register_uri_handler(httpd_handle_t server,const httpd_uri_t *route){(void)server;assert(route_count<ROUTE_CAP);routes[route_count++]=*route;return ESP_OK;}
esp_err_t httpd_req_get_url_query_str(httpd_req_t *req,char *out,size_t size){if(!req->query)return ESP_FAIL;snprintf(out,size,"%s",req->query);return ESP_OK;}
esp_err_t httpd_query_key_value(const char *query,const char *key,char *out,size_t size){char pattern[48];snprintf(pattern,sizeof(pattern),"%s=",key);const char*p=strstr(query,pattern);if(!p)return ESP_FAIL;p+=strlen(pattern);size_t n=strcspn(p,"&");if(n>=size)return ESP_FAIL;memcpy(out,p,n);out[n]=0;return ESP_OK;}

static esp_err_t (*route(const char *uri,httpd_method_t method))(httpd_req_t *){for(size_t i=0;i<route_count;i++)if(routes[i].method==method&&!strcmp(routes[i].uri,uri))return routes[i].handler;return NULL;}
static void req_init(httpd_req_t *r,const char *body){memset(r,0,sizeof(*r));r->authenticated=true;r->csrf_ok=true;r->content_type="application/json";if(body){r->body=(const unsigned char*)body;r->body_size=strlen(body);r->content_len=(int)r->body_size;}}
static bool auth(httpd_req_t *req,bool mutate,void *ctx){(void)ctx;if(!req->authenticated){httpd_resp_set_status(req,"401 Unauthorized");return false;}if(mutate&&!req->csrf_ok){httpd_resp_set_status(req,"403 Forbidden");return false;}return true;}
static bool configured(void *ctx,bool*out){(void)ctx;*out=true;return true;}
static bool recovery(void *ctx,bool*out){(void)ctx;*out=true;return true;}
static bool limit(void *ctx,size_t*out){(void)ctx;*out=8;return true;}
static bool list_profiles(void *ctx,KatmedWebWifiProfileView*out,size_t cap,size_t*count){FakeWifi*f=ctx;f->list_calls++;if(f->count>cap)return false;memcpy(out,f->view,f->count*sizeof(*out));*count=f->count;return true;}
static bool save_profiles(void *ctx,const KatmedWebWifiProfileUpdate*updates,size_t count){FakeWifi*f=ctx;f->save_calls++;if(f->fail_save||count>8)return false;KatmedWebWifiProfileView next[8]={0};char passwords[8][65]={{0}};for(size_t i=0;i<count;i++){const KatmedWebWifiProfileUpdate*u=&updates[i];next[i].id=u->id?u->id:(uint32_t)(100+i);snprintf(next[i].ssid,sizeof(next[i].ssid),"%s",u->ssid);next[i].priority=u->priority;const char*old=NULL;for(size_t j=0;j<f->count;j++)if(f->view[j].id==u->id)old=f->passwords[j];if(u->credential_action==KATMED_WEB_WIFI_CREDENTIAL_KEEP){if(!old&&u->id)return false;if(old)snprintf(passwords[i],sizeof(passwords[i]),"%s",old);}else if(u->credential_action==KATMED_WEB_WIFI_CREDENTIAL_REPLACE)snprintf(passwords[i],sizeof(passwords[i]),"%s",u->password);next[i].secured=passwords[i][0]!=0;}
 memcpy(f->view,next,sizeof(next));memcpy(f->passwords,passwords,sizeof(passwords));f->count=count;return true;}
static bool delete_profile(void *ctx,uint32_t id){FakeWifi*f=ctx;f->delete_calls++;size_t i;for(i=0;i<f->count&&f->view[i].id!=id;i++);if(i==f->count)return false;for(size_t j=i+1;j<f->count;j++){f->view[j-1]=f->view[j];memcpy(f->passwords[j-1],f->passwords[j],65);}f->count--;return true;}
static bool reconnect(void *ctx){((FakeWifi*)ctx)->reconnect_calls++;return true;}
static bool prepare(void *ctx,char*out,size_t cap,size_t*length){FakeReset*f=ctx;f->prepare_calls++;const char token[]="synthetic-one-use-token";if(cap<sizeof(token))return false;memcpy(out,token,sizeof(token));*length=sizeof(token)-1;return true;}
static KatmedWebResetResult confirm_reset(void *ctx,const char*token,size_t length){FakeReset*f=ctx;f->confirm_calls++;if(length!=23||memcmp(token,"synthetic-one-use-token",23))return KATMED_WEB_RESET_STALE;if(f->next_result==KATMED_WEB_RESET_ACCEPTED)f->reset_applied=true;return f->next_result;}
static void fill_profiles(char *body,size_t cap,size_t n){size_t at=(size_t)snprintf(body,cap,"{\"profiles\":[");for(size_t i=0;i<n;i++){at+=(size_t)snprintf(body+at,cap-at,"%s{\"ssid\":\"network-%zu\",\"priority\":1,\"credential\":\"open\",\"password\":\"\"}",i?",":"",i);}snprintf(body+at,cap-at,"]}");}

int main(void){
 FakeWifi wifi={.view={{.id=7,.ssid="existing",.priority=1,.secured=true}},.passwords={"synthetic-secret-never-return"},.count=1};
 KatmedWebWifiServices ws={.context=&wifi,.authorize=auth,.configured=configured,.recovery_available=recovery,.profile_limit=limit,.list_profiles=list_profiles,.save_profiles_atomic=save_profiles,.delete_profile_atomic=delete_profile,.schedule_reconnect=reconnect};
 FakeReset reset={.next_result=KATMED_WEB_RESET_STALE};
 KatmedWebResetServices rs={.context=&reset,.authorize=auth,.prepare_confirmation=prepare,.confirm_and_reset=confirm_reset};
 assert(katmed_web_wifi_register((httpd_handle_t)1,&ws));assert(katmed_web_reset_register((httpd_handle_t)1,&rs));
 httpd_req_t req;req_init(&req,NULL);req.authenticated=false;assert(route("/api/wifi",HTTP_GET)(&req)==ESP_OK);assert(!strcmp(req.response_status,"401 Unauthorized"));assert(wifi.list_calls==0);
 req_init(&req,NULL);assert(route("/api/wifi",HTTP_GET)(&req)==ESP_OK);assert(strstr(req.response,"\"profile_limit\":8"));assert(strstr(req.response,"existing"));assert(!strstr(req.response,"synthetic-secret-never-return"));assert(!strstr(req.response,"password"));
 char body[4096];fill_profiles(body,sizeof(body),8);req_init(&req,body);assert(route("/api/wifi/profiles",HTTP_PUT)(&req)==ESP_OK);assert(!strcmp(req.response_status,"200 OK"));assert(wifi.count==8);for(size_t i=0;i<8;i++)assert(wifi.view[i].priority==1&&wifi.view[i].id==100+i); /* ties preserve supplied order */
 const unsigned before=wifi.save_calls;fill_profiles(body,sizeof(body),9);req_init(&req,body);assert(route("/api/wifi/profiles",HTTP_PUT)(&req)==ESP_OK);assert(!strcmp(req.response_status,"422 Unprocessable Content"));assert(wifi.save_calls==before&&wifi.count==8);
 const char secret_body[]="{\"profiles\":[{\"ssid\":\"changed\",\"priority\":2,\"credential\":\"replace\",\"password\":\"synthetic-route-secret\"}]}";
 wipe_calls=wipe_length_count=0;wipe_nonzero=false;req_init(&req,secret_body);assert(route("/api/wifi/profiles",HTTP_PUT)(&req)==ESP_OK);assert(!strcmp(req.response_status,"200 OK"));assert_wiped_body(strlen(secret_body));assert(!strstr(req.response,"synthetic-route-secret"));
 const char old_ssid[]="changed";const unsigned saves_before_failure=wifi.save_calls;
 wipe_calls=wipe_length_count=0;wipe_nonzero=false;wifi.fail_save=true;req_init(&req,secret_body);assert(route("/api/wifi/profiles",HTTP_PUT)(&req)==ESP_OK);assert(!strcmp(req.response_status,"503 Service Unavailable"));assert_wiped_body(strlen(secret_body));assert(wifi.save_calls==saves_before_failure+1);assert(!strcmp(wifi.view[0].ssid,old_ssid));assert(!strstr(req.response,"synthetic-route-secret"));wifi.fail_save=false;
 const char semantic_bad[]="{\"profiles\":[{\"ssid\":\"bad\",\"priority\":0,\"credential\":\"replace\",\"password\":\"synthetic-parse-secret\"}]}";
 wipe_calls=wipe_length_count=0;wipe_nonzero=false;req_init(&req,semantic_bad);assert(route("/api/wifi/profiles",HTTP_PUT)(&req)==ESP_OK);assert(!strcmp(req.response_status,"422 Unprocessable Content"));assert_wiped_body(strlen(semantic_bad));assert(!strstr(req.response,"synthetic-parse-secret"));
 const char syntax_bad[]="{\"profiles\":[{\"ssid\":\"bad\",\"priority\":1,\"credential\":\"replace\",\"password\":\"synthetic-syntax-secret\"}]}!";
 wipe_calls=wipe_length_count=0;wipe_nonzero=false;req_init(&req,syntax_bad);assert(route("/api/wifi/profiles",HTTP_PUT)(&req)==ESP_OK);assert(!strcmp(req.response_status,"422 Unprocessable Content"));assert_wiped_body(strlen(syntax_bad));assert(!strstr(req.response,"synthetic-syntax-secret"));
 req_init(&req,NULL);req.csrf_ok=false;assert(route("/api/wifi/reconnect",HTTP_POST)(&req)==ESP_OK);assert(!strcmp(req.response_status,"403 Forbidden"));assert(wifi.reconnect_calls==0);
 req_init(&req,NULL);assert(route("/api/wifi/reconnect",HTTP_POST)(&req)==ESP_OK);assert(strstr(req.response,"reevaluate_priority"));assert(wifi.reconnect_calls==1);

 req_init(&req,NULL);req.csrf_ok=false;assert(route("/api/factory-reset/prepare",HTTP_POST)(&req)==ESP_OK);assert(!strcmp(req.response_status,"403 Forbidden"));assert(reset.prepare_calls==0);
 req_init(&req,NULL);assert(route("/api/factory-reset/prepare",HTTP_POST)(&req)==ESP_OK);assert(strstr(req.response,"synthetic-one-use-token"));assert(reset.prepare_calls==1&&reset.confirm_calls==0&&!reset.reset_applied);
 req_init(&req,"{\"confirmation\":\"stale\"}");assert(route("/api/factory-reset/confirm",HTTP_POST)(&req)==ESP_OK);assert(!strcmp(req.response_status,"409 Conflict"));assert(!reset.reset_applied);
 req_init(&req,"{\"confirmation\":\"synthetic-one-use-token\"}");reset.next_result=KATMED_WEB_RESET_FAILED;assert(route("/api/factory-reset/confirm",HTTP_POST)(&req)==ESP_OK);assert(!strcmp(req.response_status,"503 Service Unavailable"));assert(!reset.reset_applied);
 req_init(&req,"{\"confirmation\":\"synthetic-one-use-token\"}");reset.next_result=KATMED_WEB_RESET_ACCEPTED;assert(route("/api/factory-reset/confirm",HTTP_POST)(&req)==ESP_OK);assert(!strcmp(req.response_status,"202 Accepted"));assert(reset.reset_applied);
 return 0;
}
