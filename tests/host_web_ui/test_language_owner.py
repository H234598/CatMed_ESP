"""Real language HTTP handlers preserve owner completion and auth semantics."""
import tempfile
import unittest
from pathlib import Path
from test_network_startup import function, execute_tls_callback

ROOT = Path(__file__).resolve().parents[2]
PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "katmed_config_bridge.h"
#define ESP_OK 0
#define KATMED_LANGUAGE_COUNT 12
#define KATMED_LANG_EN 0
#define KATMED_WEB_TOKEN_HEX 64
typedef int esp_err_t;
typedef struct {int unused;} httpd_req_t;
typedef struct {int unused;} cJSON;
static cJSON object;
static const char *code="de",*status_text,*error,*reply_language;
static bool authorized=true,valid_keys=true;
static unsigned auth_calls,get_calls,set_calls;
static KatmedConfigStatus outcome=KATMED_CONFIG_OK;
static struct {struct {void *context;KatmedConfigStatus (*language_get)(void*,uint8_t*);KatmedConfigStatus (*language_set)(void*,uint8_t,uint8_t*);} services;} web;
static KatmedConfigStatus get(void *c,uint8_t *out){(void)c;++get_calls;if(outcome==KATMED_CONFIG_OK)*out=1;return outcome;}
static KatmedConfigStatus set(void *c,uint8_t language,uint8_t *out){(void)c;++set_calls;if(outcome==KATMED_CONFIG_OK)*out=language;return outcome;}
static int error_reply(httpd_req_t *r,const char *s,const char *e){(void)r;status_text=s;error=e;return 0;}
static int json_reply(httpd_req_t *r,const char *s,cJSON *j){(void)r;(void)j;status_text=s;return 0;}
static cJSON *cJSON_CreateObject(void){return &object;}
static void cJSON_AddStringToObject(cJSON *j,const char *k,const char *v){(void)j;assert(!strcmp(k,"language"));reply_language=v;}
static bool require_session(httpd_req_t *r,bool mutate,char *token){(void)r;(void)token;assert(mutate);++auth_calls;return authorized;}
static cJSON *read_json(httpd_req_t *r){(void)r;return &object;}
static const char *json_string(cJSON *j,const char *k,size_t n){(void)j;(void)k;(void)n;return code;}
static bool keys_only(cJSON *j,const char *const *k,size_t n){(void)j;(void)k;assert(n==1);return valid_keys;}
static void wipe_json(cJSON *j){(void)j;}
static void cJSON_Delete(cJSON *j){(void)j;}
'''
MAIN = r'''
int main(int argc,char **argv){
 assert(argc==2);web.services.language_get=get;web.services.language_set=set;httpd_req_t req={0};
 if(!strcmp(argv[1],"get")){language_get(&req);assert(get_calls==1 && !auth_calls && !strcmp(reply_language,"de") && !strcmp(status_text,"200 OK"));}
 else if(!strcmp(argv[1],"save")){language_put(&req);assert(auth_calls==1 && set_calls==1 && get_calls==0 && !strcmp(reply_language,"de") && !strcmp(status_text,"200 OK"));}
 else if(!strcmp(argv[1],"denied")){authorized=false;language_put(&req);assert(auth_calls==1 && set_calls==0 && get_calls==0 && !status_text);}
 else if(!strcmp(argv[1],"invalid")){code="unknown";language_put(&req);assert(set_calls==0 && !strcmp(error,"invalid"));}
 else if(!strcmp(argv[1],"keys")){valid_keys=false;language_put(&req);assert(set_calls==0 && !strcmp(error,"invalid"));}
 else if(!strcmp(argv[1],"unknown")){outcome=KATMED_CONFIG_COMMIT_UNKNOWN;language_put(&req);assert(set_calls==1 && get_calls==0 && !strcmp(error,"commit_unknown") && !strcmp(status_text,"503 Service Unavailable"));}
 else if(!strcmp(argv[1],"unavailable")){outcome=KATMED_CONFIG_UNAVAILABLE;language_get(&req);assert(!strcmp(error,"unavailable") && !reply_language);}
 else if(!strcmp(argv[1],"locales")){for(unsigned i=0;i<12;i++){code=language_codes[i];language_put(&req);assert(!strcmp(reply_language,code));}assert(set_calls==12 && get_calls==0);}
 else return 2;
 return 0;
}
'''


class LanguageOwnerHTTP(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        text = (ROOT / "firmware/components/katmed_web/katmed_web.c").read_text()
        start = text.index("static const char *const language_codes")
        end = text.index("};", start) + 2
        code = PREFIX + text[start:end] + "\n" + function(text, "static esp_err_t bridge_error(httpd_req_t *req, KatmedConfigStatus status)\n{")
        code += "\n" + "\n".join(function(text, s) for s in ["static esp_err_t language_reply(", "static esp_err_t language_get(", "static esp_err_t language_put("]) + MAIN
        source = Path(cls.tmp.name) / "language.c"
        source.write_text(code)
        cls.binary = Path(cls.tmp.name) / "check"
        execute_tls_callback(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "firmware/components/katmed_config_bridge/include"), "-I" + str(ROOT / "firmware/components/katmed_core/include"), str(source), "-o", str(cls.binary)])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_real_route_status_and_language_contract(self):
        for case in ["get", "save", "denied", "invalid", "keys", "unknown", "unavailable", "locales"]:
            with self.subTest(case=case):
                execute_tls_callback([str(self.binary), case])

UI_PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define T(x) (x)
#define LV_STATE_DISABLED 1
#include "katmed_ui.h"
typedef struct {uintptr_t data;} lv_event_t;
static struct {struct {void *context;UiSubmitResult (*submit)(void*,const UiRequest*);bool (*read_model)(void*,AppState*,LocalSettings*,ClockSample*);} services;AppState state;LocalSettings local;ClockSample clock;bool brightness_pending,confirmed_round,morse_alarm_paused,morse_alarm_confirmed,shared_pending,preview_valid;AlarmRoundId confirmed_id;int shared_result_status;void *brightness_slider,*style_title;int pending_style;} ui;
static UiRequest captured;
static unsigned calls,reads;static UiSubmitResult result=UI_SUBMIT_QUEUED;static const char *feedback;
static void *lv_event_get_user_data(lv_event_t *e){return (void*)e->data;}
static const char *result_text(UiSubmitResult r){return r==UI_SUBMIT_QUEUED?"queued":r==UI_SUBMIT_COMMITTED?"committed":"unavailable";}
static void show_feedback(const char *s){feedback=s;}
static void lv_obj_remove_state(void *x,int state){(void)x;(void)state;}
static void sync_brightness_slider(void){}
static unsigned style_closes;static const char *style_message;
static UiSubmitResult request_action(const UiRequest *r){assert(r->kind==UI_REQUEST_SET_CLOCK_STYLE && r->value.clock_style==2);return result;}
static void style_cancel(lv_event_t *e){(void)e;++style_closes;}
static void lv_label_set_text(void *p,const char *s){(void)p;style_message=s;}
static UiSubmitResult service_submit(void *ctx,const UiRequest *r){(void)ctx;captured=*r;++calls;return result;}
static bool read_model(void *ctx,AppState *s,LocalSettings *l,ClockSample *c){(void)ctx;(void)s;(void)l;(void)c;++reads;return true;}
'''
UI_MAIN = r'''
int main(void){
 ui.services.submit=service_submit;ui.services.read_model=read_model;lv_event_t event={1};
 language_select(&event);assert(calls==1 && reads==0 && !strcmp(feedback,"queued"));
 UiRequest language={.kind=UI_REQUEST_SET_LANGUAGE,.value.language=1};
 ui_deferred_result(&language,UI_SUBMIT_COMMITTED);assert(reads==1 && !strcmp(feedback,"committed"));
 ui_deferred_result(&language,UI_SUBMIT_UNAVAILABLE);assert(reads==1 && !strcmp(feedback,"unavailable"));
 result=UI_SUBMIT_UNAVAILABLE;language_select(&event);assert(calls==2 && reads==1 && !strcmp(feedback,"unavailable"));
 ui.pending_style=2;result=UI_SUBMIT_QUEUED;style_save(NULL);assert(style_closes==1);
 result=UI_SUBMIT_UNAVAILABLE;ui.style_title=(void*)1;style_save(NULL);assert(style_closes==1 && !strcmp(style_message,"unavailable"));
 ui.state.round.ordinal=7;ui.state.revision.number=9;ui.state.revision.coordinator=3;
 UiRequest confirm={.kind=UI_REQUEST_CONFIRM};result=UI_SUBMIT_QUEUED;
 assert(submit(&confirm)==UI_SUBMIT_QUEUED && captured.guard_model && captured.expected_round.ordinal==7 && captured.expected_revision.number==9);
 assert(!ui.confirmed_round);ui.morse_alarm_paused=true;
 ui_deferred_result(&captured,UI_SUBMIT_PENDING);assert(ui.confirmed_round && ui.confirmed_id.ordinal==7 && ui.morse_alarm_confirmed);
 ui.morse_alarm_confirmed=false;ui.state.round.ordinal=8;ui_deferred_result(&captured,UI_SUBMIT_PENDING);assert(!ui.morse_alarm_confirmed && ui.confirmed_id.ordinal==7);
 UiRequest rotate={.kind=UI_REQUEST_SET_ROTATION_MODE};assert(submit(&rotate)==UI_SUBMIT_QUEUED && captured.guard_model);
 ui_deferred_result(&captured,UI_SUBMIT_PENDING_SHARED);assert(ui.shared_pending && ui.shared_result_status==0);
 ui.preview_valid=true;ui_deferred_result(&captured,UI_SUBMIT_STALE);assert(!ui.preview_valid);
 UiRequest dismissal={.kind=UI_REQUEST_DISMISS_BOOT,.guard_model=true,.expected_round.ordinal=6};
 ui_deferred_result(&dismissal,UI_SUBMIT_DISMISSED);assert(ui.confirmed_id.ordinal==6);

 return 0;
}
'''


class PhysicalLanguageFeedback(unittest.TestCase):
    def test_real_callback_queued_busy_and_deferred_completion(self):
        text = (ROOT / "firmware/components/katmed_ui/ui.c").read_text()
        code = UI_PREFIX + function(text, "static bool same_round(") + "\n" + function(text, "static UiSubmitResult submit(const UiRequest *request)\n{") + "\n" + function(text, "static void language_select(lv_event_t *event)\n{") + "\n" + function(text, "void ui_deferred_result(") + "\n" + function(text, "static void style_save(lv_event_t *event)\n{") + UI_MAIN
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "physical.c"
            binary = Path(tmp) / "physical"
            source.write_text(code)
            execute_tls_callback(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "firmware/components/katmed_ui/include"), "-I" + str(ROOT / "firmware/components/katmed_core/include"), "-I" + str(ROOT / "firmware/components/katmed_mesh/include"), str(source), "-o", str(binary)])
            execute_tls_callback([str(binary)])
