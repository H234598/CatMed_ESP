"""Exercise real WiFi route and Main status callbacks; numeric failure logs only."""
import os
import tempfile
import unittest
from pathlib import Path
from test_network_startup import function, execute_tls_callback

ROOT = Path(__file__).resolve().parents[2]
ROUTE_SOURCE = Path(os.environ.get("KATMED_WIFI_DIAG_ROUTE_SOURCE", ROOT / "firmware/components/katmed_web/katmed_web_wifi.c"))
MAIN_SOURCE = Path(os.environ.get("KATMED_WIFI_DIAG_MAIN_SOURCE", ROOT / "firmware/main/main.c"))
LOG = r'''
#include <stdarg.h>
static unsigned records, logged_stage, logged_reason, logged_valid, logged_ready;
static int logged_bridge;
void katmed_wifi_test_log(const char *tag,const char *format,...){
 assert(!strcmp(tag,"katmed_wifi"));va_list args;va_start(args,format);++records;
 if(!strcmp(format,"get_failed stage=%u"))logged_stage=va_arg(args,unsigned);
 else {assert(!strcmp(format,"status_stage=%u bridge=%d bridge_diag=%u ready_valid=%u ready=%u"));
 logged_stage=va_arg(args,unsigned);logged_bridge=va_arg(args,int);
 logged_reason=va_arg(args,unsigned);
 logged_valid=va_arg(args,unsigned);logged_ready=va_arg(args,unsigned);}
 va_end(args);
}
'''
MAIN_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "katmed_config_bridge.h"
#include "katmed_web_wifi.h"
#include "esp_log.h"
static unsigned calls;
static KatmedConfigStatus bridge_status;
static KatmedConfigBridgeDiagnostic bridge_diagnostic;
static KatmedConfigBridgeTrace bridge_trace;
static bool actual_ready;
KatmedConfigStatus katmed_config_exchange_diagnostic(const KatmedConfigRequest *request,
 KatmedConfigResponse *response,uint32_t wait,KatmedConfigBridgeDiagnostic *diagnostic,
 KatmedConfigBridgeTrace *trace){
 assert(request->operation==KATMED_CONFIG_WIFI_STATUS && wait==1500u);++calls;
 response->status=bridge_status;response->wifi.ready=actual_ready;
 response->wifi.configured=true;response->wifi.recovery=false;response->wifi.limit=4;
 *diagnostic=bridge_diagnostic;
 *trace=bridge_trace;
 return bridge_status;
}
'''
MAIN_CASE = r'''
int main(int argc,char **argv){
 assert(argc==6);unsigned stage=(unsigned)atoi(argv[1]);bridge_status=(KatmedConfigStatus)atoi(argv[2]);
 bridge_diagnostic=(KatmedConfigBridgeDiagnostic)atoi(argv[3]);
 actual_ready=atoi(argv[4])!=0;bool diagnostics=atoi(argv[5])!=0;
 bridge_trace=(KatmedConfigBridgeTrace){.valid=KATMED_CONFIG_TRACE_GATE_START|
   KATMED_CONFIG_TRACE_GATE_RETURN|KATMED_CONFIG_TRACE_WAIT_RETURN,
   .tick_period_ms=10,.gate_start_tick=100,.gate_return_tick=101,
   .gate_acquired=1,.wait_return_tick=250,.wait_elapsed_ms=1490,.wait_signalled=0};
 bool value=false;size_t limit=0;bool ok;KatmedWebWifiStatusDiagnostic observed={0};
 if(stage==1)ok=web_wifi_configured((void*)1,&value,&observed);
 else if(stage==2)ok=web_wifi_recovery_available((void*)1,&value,&observed);
 else {assert(stage==3);ok=web_wifi_profile_limit((void*)1,&limit,&observed);}
 bool expected=bridge_status==KATMED_CONFIG_OK && (stage==2 || actual_ready);
 assert(calls==1 && ok==expected);
 assert(observed.available && observed.bridge==(int)bridge_status &&
        observed.bridge_diag==(unsigned)bridge_diagnostic &&
        observed.ready_valid==(bridge_status==KATMED_CONFIG_OK) &&
        observed.ready==(bridge_status==KATMED_CONFIG_OK && actual_ready));
 assert(observed.trace.valid==bridge_trace.valid &&
        observed.trace.gate_start_tick==100 &&
        observed.trace.wait_elapsed_ms==1490 && !observed.trace.wait_signalled);
 if(ok){if(stage==1)assert(value);if(stage==2)assert(!value);if(stage==3)assert(limit==4);}
 if(diagnostics){
  assert(records==(expected?0u:1u));
  if(!expected){assert(logged_stage==stage && logged_bridge==(int)bridge_status &&
                        logged_reason==(unsigned)bridge_diagnostic);
   assert(logged_valid==(bridge_status==KATMED_CONFIG_OK));
   assert(logged_ready==(bridge_status==KATMED_CONFIG_OK && actual_ready));}
 }
 unsigned before=calls;assert(!web_wifi_configured((void*)1,NULL,&observed));
 assert(!web_wifi_recovery_available((void*)1,NULL,&observed));assert(!web_wifi_profile_limit((void*)1,NULL,&observed));
 assert(calls==before);
 return 0;
}
'''
ROUTE_CASE = r'''
static bool allowed_diagnostic_key(const char *key){
 static const char *allowed[]={"error","message","status_stage","bridge","bridge_diag",
  "ready_valid","ready","trace_tick_ms","admission_start_ms","gate_return_elapsed_ms",
  "gate_acquired","enqueue_elapsed_ms","owner_poll_elapsed_ms",
  "owner_complete_elapsed_ms","wait_return_elapsed_ms","wait_elapsed_ms",
  "wait_signalled","decision_elapsed_ms","slot_at_decision","owner_phase_enqueue",
  "owner_phase_decision","owner_phase_seen","render_call_overlap_ms",
  "render_inner_period_at_freeze","render_inner_seen","render_inner_pre_enqueue",
  "render_inner_open_enqueue",
  "render_inner_open_freeze","render_inner_multi","render_lock_wait_overlap_ms",
  "render_ui_overlap_ms","render_rebuild_overlap_ms",
  "render_image_present_overlap_ms","render_image_select_overlap_ms",
  "render_variant_hash_overlap_ms","render_jpeg_decode_overlap_ms",
  "render_store_wait_overlap_ms","render_validation_wait_overlap_ms",
  "render_lock_success","render_image_select_status","render_image_present_status"};
 for(size_t i=0;i<sizeof(allowed)/sizeof(allowed[0]);++i)
  if(!strcmp(key,allowed[i]))return true;
 return false;
}
static void assert_diagnostic_keys(const cJSON *parsed){
 for(const cJSON *item=parsed->child;item;item=item->next){
  assert(item->string && allowed_diagnostic_key(item->string));
  for(const cJSON *older=parsed->child;older!=item;older=older->next)
   assert(older->string && strcmp(older->string,item->string));
  if(strcmp(item->string,"error") && strcmp(item->string,"message"))
   assert(cJSON_IsNumber(item));
 }
}
static bool fail_alloc;
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t count,size_t size){
 if(fail_alloc){fail_alloc=false;return NULL;}return __real_calloc(count,size);
}
int main(int argc,char **argv){
 assert(argc==3 || argc==4);fail_stage=(unsigned)atoi(argv[1]);bool diagnostics=atoi(argv[2])!=0;
 inner_mode=argc==4?(unsigned)atoi(argv[3]):0u;
 FakeWifi wifi={.view={{.ssid="synthetic-private-profile"}},
                .passwords={"synthetic-secret-never-return"}};
 KatmedWebWifiServices ws={.context=&wifi,.authorize=auth,
 .configured=configured,.recovery_available=recovery,.profile_limit=limit,
 .list_profiles=list_profiles,.save_profiles_atomic=save_profiles,
 .delete_profile_atomic=delete_profile,.schedule_reconnect=reconnect};
 assert(katmed_web_wifi_register((httpd_handle_t)1,&ws));httpd_req_t req;
 req_init(&req,NULL);req.authenticated=false;assert(route("/api/wifi",HTTP_GET)(&req)==ESP_OK);
 assert(!strcmp(req.response_status,"401 Unauthorized") && !records && !callback_count);
 assert(!strstr(req.response,"status_stage") && !strstr(req.response,"bridge_diag") &&
        !strstr(req.response,"admission_start_ms") &&
        !strstr(req.response,"render_inner_period_at_freeze"));
 req_init(&req,NULL);fail_alloc=fail_stage==4;
 assert(route("/api/wifi",HTTP_GET)(&req)==ESP_OK);
 cJSON *parsed=cJSON_ParseWithLength(req.response,req.response_size);
 assert(parsed && cJSON_IsObject(parsed));if(fail_stage)assert_diagnostic_keys(parsed);cJSON_Delete(parsed);
 if(!fail_stage){assert(!strcmp(req.response_status,"200 OK"));assert(wifi.list_calls==1);assert(!records);
 assert(!strstr(req.response,"status_stage") && !strstr(req.response,"bridge_diag") &&
        !strstr(req.response,"admission_start_ms") &&
        !strstr(req.response,"render_inner_period_at_freeze"));}
 else {assert(!strcmp(req.response_status,"503 Service Unavailable"));
 assert(strstr(req.response,"wifi_unavailable"));assert(strstr(req.response,"Der WLAN-Status ist derzeit nicht verf"));
 char stage_field[32];snprintf(stage_field,sizeof(stage_field),"\"status_stage\":%u",fail_stage);
 assert(strstr(req.response,stage_field));
 if(fail_stage<=3){char bridge_field[32];snprintf(bridge_field,sizeof(bridge_field),"\"bridge\":%u",10u+fail_stage);
 assert(strstr(req.response,bridge_field));assert(strstr(req.response,"\"bridge_diag\":7"));
 assert(strstr(req.response,"\"ready_valid\":0"));assert(strstr(req.response,"\"ready\":0"));
 if(fail_stage==1){if(inner_mode!=6 && inner_mode!=8){assert(strstr(req.response,"\"admission_start_ms\":0"));
 assert(strstr(req.response,"\"gate_return_elapsed_ms\":1"));
 assert(strstr(req.response,"\"wait_elapsed_ms\":19"));
 assert(strstr(req.response,"\"wait_signalled\":0"));
 assert(strstr(req.response,"\"wait_return_elapsed_ms\":20"));
 assert(strstr(req.response,"\"decision_elapsed_ms\":21"));
 assert(strstr(req.response,"\"slot_at_decision\":1"));
 assert(strstr(req.response,"\"render_call_overlap_ms\":17"));}
 if(inner_mode==1){
  assert(strstr(req.response,"\"render_inner_period_at_freeze\":2"));
  assert(strstr(req.response,"\"render_inner_seen\":511"));
  assert(strstr(req.response,"\"render_inner_pre_enqueue\":0"));
  assert(strstr(req.response,"\"render_inner_open_enqueue\":0"));
  assert(strstr(req.response,"\"render_inner_open_freeze\":0"));
  assert(strstr(req.response,"\"render_inner_multi\":0"));
  assert(strstr(req.response,"\"render_lock_wait_overlap_ms\":4294967295"));
  assert(strstr(req.response,"\"render_validation_wait_overlap_ms\":4294967295"));
  assert(strstr(req.response,"\"render_lock_success\":1"));
  assert(strstr(req.response,"\"render_image_select_status\":10"));
  assert(strstr(req.response,"\"render_image_present_status\":10"));
  assert(req.response_size<1536);
 } else if(inner_mode==2){
  assert(strstr(req.response,"\"render_inner_period_at_freeze\":1"));
  assert(strstr(req.response,"\"render_inner_seen\":1"));
  assert(strstr(req.response,"\"render_inner_pre_enqueue\":0"));
  assert(strstr(req.response,"\"render_lock_wait_overlap_ms\":0"));
  assert(!strstr(req.response,"render_ui_overlap_ms"));
  assert(!strstr(req.response,"render_lock_success"));
 } else if(inner_mode==3){
  assert(strstr(req.response,"\"render_inner_period_at_freeze\":3"));
  assert(!strstr(req.response,"render_inner_seen"));
  assert(!strstr(req.response,"render_lock_wait_overlap_ms"));
 } else if(inner_mode==7){
  assert(strstr(req.response,"\"render_inner_period_at_freeze\":2"));
  assert(strstr(req.response,"\"render_inner_seen\":0"));
  assert(strstr(req.response,"\"render_inner_pre_enqueue\":25"));
  assert(strstr(req.response,"\"render_lock_success\":1"));
  assert(strstr(req.response,"\"render_image_select_status\":7"));
  assert(strstr(req.response,"\"render_image_present_status\":0"));
  assert(!strstr(req.response,"render_lock_wait_overlap_ms"));
 } else assert(!strstr(req.response,"render_inner_period_at_freeze"));}
 if(fail_stage==2){assert(strstr(req.response,"\"gate_return_elapsed_ms\":5"));
 assert(!strstr(req.response,"\"wait_elapsed_ms\""));
 assert(!strstr(req.response,"render_inner_period_at_freeze"));}
 if(fail_stage==3){assert(!strstr(req.response,"admission_start_ms"));
 assert(!strstr(req.response,"render_inner_period_at_freeze"));}
 assert(!strstr(req.response,"gate_start_tick") &&
        !strstr(req.response,"enqueue_tick") &&
        !strstr(req.response,"wait_return_tick"));}
 else {assert(!strstr(req.response,"\"bridge\""));assert(!strstr(req.response,"bridge_diag"));
 assert(!strstr(req.response,"admission_start_ms"));
 assert(!strstr(req.response,"render_inner_period_at_freeze"));}
 assert(!strstr(req.response,"synthetic-private-profile") &&
        !strstr(req.response,"synthetic-secret-never-return"));
 assert(!wifi.list_calls);if(diagnostics)assert(records==1 && logged_stage==fail_stage);}
 assert(callback_count==(fail_stage>=1 && fail_stage<=3?fail_stage:3));
 if(fail_stage){unsigned first=fail_stage,next=first==1?2u:1u;fail_stage=next;callback_count=0;
 req_init(&req,NULL);assert(route("/api/wifi",HTTP_GET)(&req)==ESP_OK);
 parsed=cJSON_ParseWithLength(req.response,req.response_size);assert(parsed && cJSON_IsObject(parsed));cJSON_Delete(parsed);
 char next_field[32];snprintf(next_field,sizeof(next_field),"\"bridge\":%u",10u+next);
 assert(strstr(req.response,next_field));assert(callback_count==next);
 if(next==2)assert(strstr(req.response,"\"gate_return_elapsed_ms\":5") &&
                  !strstr(req.response,"\"wait_elapsed_ms\"") &&
                  !strstr(req.response,"render_inner_period_at_freeze") &&
                  !strstr(req.response,"render_lock_wait_overlap_ms"));
 if(next==1 && inner_mode!=6 && inner_mode!=8)
  assert(strstr(req.response,"\"gate_return_elapsed_ms\":1"));
 if(first<=3){char old_field[32];snprintf(old_field,sizeof(old_field),"\"bridge\":%u",10u+first);
 assert(!strstr(req.response,old_field));}}
 fail_stage=0;wifi.count=5;req_init(&req,NULL);
 assert(route("/api/wifi",HTTP_GET)(&req)==ESP_OK);
 assert(!strcmp(req.response_status,"503 Service Unavailable"));
 assert(strstr(req.response,"Die gespeicherten WLAN-Profile"));
 assert(!strstr(req.response,"status_stage") && !strstr(req.response,"bridge_diag") &&
        !strstr(req.response,"admission_start_ms") &&
        !strstr(req.response,"render_inner_period_at_freeze"));
 parsed=cJSON_ParseWithLength(req.response,req.response_size);assert(parsed && cJSON_IsObject(parsed));cJSON_Delete(parsed);
 wifi.count=0;fail_stage=1;extreme_diag=true;req_init(&req,NULL);
 assert(route("/api/wifi",HTTP_GET)(&req)==ESP_OK);
 assert(strstr(req.response,"\"bridge\":-2147483648"));
 assert(strstr(req.response,"\"bridge_diag\":4294967295"));
 assert(req.response_size<sizeof(req.response) && req.response[req.response_size-1]=='}');
 parsed=cJSON_ParseWithLength(req.response,req.response_size);assert(parsed && cJSON_IsObject(parsed));
 assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(parsed,"bridge")));
 assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(parsed,"bridge_diag")));
 if(inner_mode==1)assert(req.response_size>1024 && req.response_size<1536);
 cJSON_Delete(parsed);
 extreme_diag=false;
 for(unsigned early=2;early<=3;++early){early_fail_stage=early;fail_stage=early;callback_count=0;
 req_init(&req,NULL);assert(route("/api/wifi",HTTP_GET)(&req)==ESP_OK);
 assert(!strcmp(req.response_status,"503 Service Unavailable"));
 char early_field[32];snprintf(early_field,sizeof(early_field),"\"status_stage\":%u",early);
 assert(strstr(req.response,early_field) && !strstr(req.response,"\"bridge\""));
 assert(!strstr(req.response,"bridge_diag") &&
        !strstr(req.response,"admission_start_ms") && callback_count==early);
 assert(!strstr(req.response,"synthetic-private-profile") &&
        !strstr(req.response,"synthetic-secret-never-return"));
 parsed=cJSON_ParseWithLength(req.response,req.response_size);assert(parsed && cJSON_IsObject(parsed));cJSON_Delete(parsed);}
 return 0;
}
'''


class WifiStatusDiagnostics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        directory = Path(cls.tmp.name)
        includes = ["-I" + str(ROOT / "tests/host_web_upload_wifi/stubs"),
                    "-I" + str(ROOT / "firmware/components/katmed_web/include"),
                    "-I" + str(ROOT / "firmware/components/katmed_config_bridge/include"),
                    "-I" + str(ROOT / "firmware/components/katmed_core/include")]
        text = MAIN_SOURCE.read_text()
        signatures = ["static bool web_wifi_status(", "static bool web_wifi_configured(",
                      "static bool web_wifi_recovery_available(", "static bool web_wifi_profile_limit("]
        main = directory / "main.c"
        main.write_text(MAIN_PREFIX + LOG + "\n".join(function(text, sig) for sig in signatures) + MAIN_CASE)
        cls.main_binary = directory / "main"
        execute_tls_callback(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                              "-DKATMED_WIFI_DIAGNOSTIC_CAPTURE", "-I" + str(ROOT / "firmware/components/katmed_core/include"), *includes, str(main), "-o", str(cls.main_binary)])
        # Reuse the existing route harness's actual HTTP stubs/registration.
        fixture = (ROOT / "tests/host_web_upload_wifi/check_wifi_reset_routes.c").read_text().split("int main(void){", 1)[0]
        fixture = fixture.replace("static bool configured(void *ctx,bool*out,KatmedWebWifiStatusDiagnostic*diagnostic){(void)ctx;(void)diagnostic;*out=true;return true;}",
                                  "static unsigned fail_stage,callback_count,early_fail_stage,inner_mode;static bool extreme_diag;\n"
                                  "static void seed_inner(KatmedWebWifiStatusDiagnostic*d){\n"
                                  " if(!inner_mode)return;\n"
                                  " d->trace.valid|=KATMED_CONFIG_TRACE_RENDER_INNER;\n"
                                  " if(inner_mode==1){d->trace.inner_period_at_freeze=KATMED_CONFIG_RENDER_INNER_ENDED;\n"
                                  " d->trace.inner_seen_mask=(1u<<KATMED_RENDER_INNER_COUNT)-1u;\n"
                                  " d->trace.inner_status_valid=KATMED_CONFIG_RENDER_STATUS_LOCK|KATMED_CONFIG_RENDER_STATUS_SELECT|KATMED_CONFIG_RENDER_STATUS_PRESENT;\n"
                                  " d->trace.inner_lock_success=1;d->trace.inner_select_status=10;d->trace.inner_present_status=10;\n"
                                  " for(unsigned i=0;i<KATMED_RENDER_INNER_COUNT;++i)d->trace.inner_overlap_ms[i]=~0u;\n"
                                  " }else if(inner_mode==2){d->trace.inner_period_at_freeze=KATMED_CONFIG_RENDER_INNER_ACTIVE;\n"
                                  " d->trace.inner_seen_mask=1;d->trace.inner_open_freeze_mask=1;\n"
                                  " }else if(inner_mode==3)d->trace.inner_period_at_freeze=KATMED_CONFIG_RENDER_INNER_MULTI;\n"
                                  " else if(inner_mode==6){d->trace.inner_period_at_freeze=KATMED_CONFIG_RENDER_INNER_ENDED;\n"
                                  " d->trace.inner_seen_mask=1;d->trace.inner_status_valid=KATMED_CONFIG_RENDER_STATUS_LOCK;\n"
                                  " d->trace.inner_lock_success=1;}\n"
                                  " else if(inner_mode==7){d->trace.inner_period_at_freeze=KATMED_CONFIG_RENDER_INNER_ENDED;\n"
                                  " d->trace.inner_pre_enqueue_mask=(1u<<KATMED_RENDER_INNER_LVGL_LOCK)|(1u<<KATMED_RENDER_INNER_IMAGE_SELECT)|(1u<<KATMED_RENDER_INNER_IMAGE_PRESENT);\n"
                                  " d->trace.inner_status_valid=KATMED_CONFIG_RENDER_STATUS_LOCK|KATMED_CONFIG_RENDER_STATUS_SELECT|KATMED_CONFIG_RENDER_STATUS_PRESENT;\n"
                                  " d->trace.inner_lock_success=1;d->trace.inner_select_status=7;d->trace.inner_present_status=0;}\n"
                                  " else if(inner_mode==8){d->trace.inner_period_at_freeze=KATMED_CONFIG_RENDER_INNER_ENDED;\n"
                                  " d->trace.inner_seen_mask=1;d->trace.tick_period_ms=0;}\n"
                                  " else {d->trace.inner_period_at_freeze=KATMED_CONFIG_RENDER_INNER_ENDED;\n"
                                  " d->trace.inner_seen_mask=inner_mode==4?~0u:1u;\n"
                                  " d->trace.inner_status_valid=KATMED_CONFIG_RENDER_STATUS_LOCK;\n"
                                  " d->trace.inner_lock_success=2;}\n"
                                  "}\n"
                                  "static void seed_trace(KatmedWebWifiStatusDiagnostic*d,unsigned stage){if(stage==1){d->trace=(KatmedConfigBridgeTrace){.valid=KATMED_CONFIG_TRACE_GATE_START|KATMED_CONFIG_TRACE_GATE_RETURN|KATMED_CONFIG_TRACE_WAIT_RETURN|KATMED_CONFIG_TRACE_DECISION|KATMED_CONFIG_TRACE_PHASE_SEEN,.tick_period_ms=1,.gate_start_tick=100,.gate_return_tick=101,.gate_acquired=1,.wait_return_tick=120,.decision_tick=121,.wait_elapsed_ms=19,.wait_signalled=0,.slot_at_decision=1,.owner_phase_seen_mask=1u<<KATMED_CONFIG_OWNER_PHASE_RENDER_CALL,.render_overlap_ms=17};seed_inner(d);if(extreme_diag){d->trace.valid|=(1u<<10)-1u;d->trace.gate_return_tick=99;d->trace.enqueue_tick=99;d->trace.poll_enter_tick=99;d->trace.poll_complete_tick=99;d->trace.wait_return_tick=99;d->trace.decision_tick=99;d->trace.wait_elapsed_ms=~0u;d->trace.render_overlap_ms=~0u;d->trace.owner_phase_seen_mask=~0u;d->trace.gate_acquired=255;d->trace.wait_signalled=255;d->trace.slot_at_decision=255;d->trace.owner_phase_at_enqueue=255;d->trace.owner_phase_at_decision=255;}if(inner_mode==6)d->trace.valid&=~KATMED_CONFIG_TRACE_GATE_START;}else if(stage==2)d->trace=(KatmedConfigBridgeTrace){.valid=KATMED_CONFIG_TRACE_GATE_START|KATMED_CONFIG_TRACE_GATE_RETURN,.tick_period_ms=1,.gate_start_tick=200,.gate_return_tick=205,.gate_acquired=1};}\n"
                                  "static bool configured(void *ctx,bool*out,KatmedWebWifiStatusDiagnostic*diagnostic){(void)ctx;++callback_count;*out=true;if(diagnostic){*diagnostic=(KatmedWebWifiStatusDiagnostic){.bridge=extreme_diag?(-2147483647-1):11,.bridge_diag=extreme_diag?~0u:7u,.available=true};seed_trace(diagnostic,1);}return fail_stage!=1;}")
        fixture = fixture.replace("static bool recovery(void *ctx,bool*out,KatmedWebWifiStatusDiagnostic*diagnostic){(void)ctx;(void)diagnostic;*out=true;return true;}",
                                  "static bool recovery(void *ctx,bool*out,KatmedWebWifiStatusDiagnostic*diagnostic){(void)ctx;++callback_count;*out=true;if(early_fail_stage==2)return false;if(diagnostic){*diagnostic=(KatmedWebWifiStatusDiagnostic){.bridge=12,.bridge_diag=7,.available=true};seed_trace(diagnostic,2);}return fail_stage!=2;}")
        fixture = fixture.replace("static bool limit(void *ctx,size_t*out,KatmedWebWifiStatusDiagnostic*diagnostic){(void)ctx;(void)diagnostic;*out=4;return true;}",
                                  "static bool limit(void *ctx,size_t*out,KatmedWebWifiStatusDiagnostic*diagnostic){(void)ctx;++callback_count;*out=4;if(early_fail_stage==3)return false;if(diagnostic)*diagnostic=(KatmedWebWifiStatusDiagnostic){.bridge=13,.bridge_diag=7,.available=true};return fail_stage!=3;}")
        route = directory / "route.c"
        route.write_text(fixture + LOG + ROUTE_CASE)
        production_route = directory / "production_wifi.c"
        production_route.write_bytes(ROUTE_SOURCE.read_bytes())
        cls.route_binary = directory / "route"
        execute_tls_callback(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                              "-DKATMED_WEB_WIFI_TEST_WIPE_OBSERVER", "-DKATMED_WIFI_DIAGNOSTIC_CAPTURE",
                              *includes, str(route), str(ROOT / "tests/host_web_upload_wifi/stubs/cJSON.c"),
                              str(production_route), str(ROOT / "firmware/components/katmed_web/katmed_web_reset.c"),
                              "-Wl,--wrap=calloc", "-o", str(cls.route_binary)])

    def exercise(self, diagnostics):
        for stage in range(1, 4):
            for status, reason, ready in [(0, 0, 1), (0, 0, 0), (4, 0, 0),
                                           (4, 1, 0), (4, 2, 0), (4, 3, 0),
                                           (5, 4, 0)]:
                with self.subTest(callback=stage, status=status, reason=reason, ready=ready):
                    execute_tls_callback([str(self.main_binary), str(stage), str(status),
                                          str(reason), str(ready), str(diagnostics)])
        for stage in range(5):
            with self.subTest(route_failure_stage=stage):
                execute_tls_callback([str(self.route_binary), str(stage), str(diagnostics)])

    def test_existing_behavior_and_failure_cleanup(self):
        self.exercise(0)

    def test_numeric_failure_reason_contract(self):
        self.exercise(1)

    def test_request_bound_inner_group_and_privacy(self):
        # The real route emits only fixed numeric fields on authenticated stage-1 503.
        # Modes cover max-size payload, partial active period, MULTI, and malformed groups.
        for mode in range(1, 9):
            for stage in (0, 1, 2, 3, 4):
                with self.subTest(mode=mode, stage=stage):
                    execute_tls_callback([str(self.route_binary), str(stage), "1", str(mode)])
