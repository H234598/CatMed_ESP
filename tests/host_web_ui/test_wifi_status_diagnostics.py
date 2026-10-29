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
static unsigned records, logged_stage, logged_valid, logged_ready;
static int logged_bridge;
void katmed_wifi_test_log(const char *tag,const char *format,...){
 assert(!strcmp(tag,"katmed_wifi"));va_list args;va_start(args,format);++records;
 if(!strcmp(format,"get_failed stage=%u"))logged_stage=va_arg(args,unsigned);
 else {assert(!strcmp(format,"status_stage=%u bridge=%d ready_valid=%u ready=%u"));
 logged_stage=va_arg(args,unsigned);logged_bridge=va_arg(args,int);
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
#include "esp_log.h"
static unsigned calls;
static KatmedConfigStatus bridge_status;
static bool actual_ready;
KatmedConfigStatus katmed_config_exchange(const KatmedConfigRequest *request,
 KatmedConfigResponse *response,uint32_t wait){
 assert(request->operation==KATMED_CONFIG_WIFI_STATUS && wait==1500u);++calls;
 response->status=bridge_status;response->wifi.ready=actual_ready;
 response->wifi.configured=true;response->wifi.recovery=false;response->wifi.limit=4;
 return bridge_status;
}
'''
MAIN_CASE = r'''
int main(int argc,char **argv){
 assert(argc==5);unsigned stage=(unsigned)atoi(argv[1]);bridge_status=(KatmedConfigStatus)atoi(argv[2]);
 actual_ready=atoi(argv[3])!=0;bool diagnostics=atoi(argv[4])!=0;
 bool value=false;size_t limit=0;bool ok;
 if(stage==1)ok=web_wifi_configured((void*)1,&value);
 else if(stage==2)ok=web_wifi_recovery_available((void*)1,&value);
 else {assert(stage==3);ok=web_wifi_profile_limit((void*)1,&limit);}
 bool expected=bridge_status==KATMED_CONFIG_OK && (stage==2 || actual_ready);
 assert(calls==1 && ok==expected);
 if(ok){if(stage==1)assert(value);if(stage==2)assert(!value);if(stage==3)assert(limit==4);}
 if(diagnostics){
  assert(records==(expected?0u:1u));
  if(!expected){assert(logged_stage==stage && logged_bridge==(int)bridge_status);
   assert(logged_valid==(bridge_status==KATMED_CONFIG_OK));
   assert(logged_ready==(bridge_status==KATMED_CONFIG_OK && actual_ready));}
 }
 unsigned before=calls;assert(!web_wifi_configured((void*)1,NULL));
 assert(!web_wifi_recovery_available((void*)1,NULL));assert(!web_wifi_profile_limit((void*)1,NULL));
 assert(calls==before);
 return 0;
}
'''
ROUTE_CASE = r'''
static bool fail_alloc;
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t count,size_t size){
 if(fail_alloc){fail_alloc=false;return NULL;}return __real_calloc(count,size);
}
int main(int argc,char **argv){
 assert(argc==3);fail_stage=(unsigned)atoi(argv[1]);bool diagnostics=atoi(argv[2])!=0;
 FakeWifi wifi={0};KatmedWebWifiServices ws={.context=&wifi,.authorize=auth,
 .configured=configured,.recovery_available=recovery,.profile_limit=limit,
 .list_profiles=list_profiles,.save_profiles_atomic=save_profiles,
 .delete_profile_atomic=delete_profile,.schedule_reconnect=reconnect};
 assert(katmed_web_wifi_register((httpd_handle_t)1,&ws));httpd_req_t req;
 req_init(&req,NULL);req.authenticated=false;assert(route("/api/wifi",HTTP_GET)(&req)==ESP_OK);
 assert(!strcmp(req.response_status,"401 Unauthorized") && !records && !callback_count);
 req_init(&req,NULL);fail_alloc=fail_stage==4;
 assert(route("/api/wifi",HTTP_GET)(&req)==ESP_OK);
 if(!fail_stage){assert(!strcmp(req.response_status,"200 OK"));assert(wifi.list_calls==1);assert(!records);}
 else {assert(!strcmp(req.response_status,"503 Service Unavailable"));
 assert(strstr(req.response,"wifi_unavailable"));assert(strstr(req.response,"Der WLAN-Status ist derzeit nicht verf"));
 assert(!wifi.list_calls);if(diagnostics)assert(records==1 && logged_stage==fail_stage);}
 assert(callback_count==(fail_stage>=1 && fail_stage<=3?fail_stage:3));
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
        fixture = fixture.replace("static bool configured(void *ctx,bool*out){(void)ctx;*out=true;return true;}",
                                  "static unsigned fail_stage,callback_count;\nstatic bool configured(void *ctx,bool*out){(void)ctx;++callback_count;*out=true;return fail_stage!=1;}")
        fixture = fixture.replace("static bool recovery(void *ctx,bool*out){(void)ctx;*out=true;return true;}",
                                  "static bool recovery(void *ctx,bool*out){(void)ctx;++callback_count;*out=true;return fail_stage!=2;}")
        fixture = fixture.replace("static bool limit(void *ctx,size_t*out){(void)ctx;*out=4;return true;}",
                                  "static bool limit(void *ctx,size_t*out){(void)ctx;++callback_count;*out=4;return fail_stage!=3;}")
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
            for status, ready in [(0, 1), (0, 0), (4, 0), (5, 0)]:
                with self.subTest(callback=stage, status=status, ready=ready):
                    execute_tls_callback([str(self.main_binary), str(stage), str(status), str(ready), str(diagnostics)])
        for stage in range(5):
            with self.subTest(route_failure_stage=stage):
                execute_tls_callback([str(self.route_binary), str(stage), str(diagnostics)])

    def test_existing_behavior_and_failure_cleanup(self):
        self.exercise(0)

    def test_numeric_failure_reason_contract(self):
        self.exercise(1)
