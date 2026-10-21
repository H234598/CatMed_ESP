"""Execute the C6 script handlers with their generated payloads and HTTP stubs."""

import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.build_web_gzip import ASSETS, build


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/components/katmed_web/katmed_web.c"
UI = ROOT / "firmware/components/katmed_web/ui"


PREFIX = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#define INET_ADDRSTRLEN 16
#define HTTPD_RESP_USE_STRLEN -1
#define ESP_OK 0
typedef int esp_err_t;
typedef struct {
    const char *host, *accept;
    bool read_error, send_error;
    char status[32], type[64], coding[32], vary[32], cache[32], csp[256];
    char error[96];
    size_t bytes;
    const char *body;
} httpd_req_t;
static bool request_host(httpd_req_t *r, char host[INET_ADDRSTRLEN]) {
    if (strcmp(r->host, "192.0.2.10")) return false;
    strcpy(host, r->host);
    return true;
}
static size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *name) {
    return strcmp(name, "Accept-Encoding") || !r->accept ? 0 : strlen(r->accept);
}
static int httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name,
                                       char *out, size_t capacity) {
    if (strcmp(name, "Accept-Encoding") || !r->accept || r->read_error ||
        strlen(r->accept) >= capacity) return 1;
    strcpy(out, r->accept);
    return ESP_OK;
}
static int httpd_resp_set_status(httpd_req_t *r, const char *v) {
    snprintf(r->status, sizeof(r->status), "%s", v); return ESP_OK;
}
static int httpd_resp_set_type(httpd_req_t *r, const char *v) {
    snprintf(r->type, sizeof(r->type), "%s", v); return ESP_OK;
}
static int httpd_resp_set_hdr(httpd_req_t *r, const char *key, const char *v) {
    char *dst = NULL; size_t capacity = 0;
    if (!strcmp(key, "Content-Encoding")) {dst = r->coding; capacity = sizeof(r->coding);}
    if (!strcmp(key, "Vary")) {dst = r->vary; capacity = sizeof(r->vary);}
    if (!strcmp(key, "Cache-Control")) {dst = r->cache; capacity = sizeof(r->cache);}
    if (!strcmp(key, "Content-Security-Policy")) {dst = r->csp; capacity = sizeof(r->csp);}
    if (dst) snprintf(dst, capacity, "%s", v);
    return ESP_OK;
}
static int httpd_resp_send(httpd_req_t *r, const char *body, ssize_t bytes) {
    if (r->send_error) return 19;
    r->bytes = bytes == HTTPD_RESP_USE_STRLEN ? strlen(body) : (size_t)bytes;
    r->body = body;
    if (r->status[0] == 0) strcpy(r->status, "200 OK");
    return ESP_OK;
}
extern const unsigned char _binary_app_js_start[], _binary_app_js_end[];
extern const unsigned char _binary_i18n_js_start[], _binary_i18n_js_end[];
'''

SUFFIX = r'''
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "failed at %d: %s\n", __LINE__, #x); return 1;}} while (0)
static int check(bool language, const char *accept, const char *host,
                 bool read_error, bool send_error, bool allowed) {
    httpd_req_t req = {.accept=accept, .host=host,
                       .read_error=read_error, .send_error=send_error};
    const unsigned char *payload = language ? _binary_i18n_js_start : _binary_app_js_start;
    size_t length = language ? (size_t)(_binary_i18n_js_end - payload) :
                               (size_t)(_binary_app_js_end - payload);
    int result = language ? language_script_get(&req) : script_get(&req);
    if (send_error && allowed && !strcmp(host, "192.0.2.10")) {
        CHECK(result == 19 && req.body == NULL); return 0;
    }
    CHECK(result == ESP_OK);
    if (strcmp(host, "192.0.2.10")) {
        CHECK(!strcmp(req.status, "403 Forbidden"));
        CHECK(req.body && strstr(req.body, "host"));
        CHECK(req.coding[0] == 0); return 0;
    }
    CHECK(!strcmp(req.vary, "Accept-Encoding"));
    CHECK(!strcmp(req.cache, "no-store"));
    if (!allowed) {
        CHECK(!strcmp(req.status, "406 Not Acceptable"));
        CHECK(!strcmp(req.type, "application/json; charset=utf-8"));
        CHECK(req.body && strstr(req.body, "gzip_required"));
        CHECK(req.coding[0] == 0); return 0;
    }
    CHECK(!strcmp(req.status, "200 OK"));
    CHECK(!strcmp(req.coding, "gzip"));
    CHECK(!strcmp(req.type, "application/javascript; charset=utf-8"));
    CHECK(strstr(req.csp, "script-src 'self'") != NULL);
    CHECK(req.bytes == length && req.body == (const char *)payload);
    CHECK(payload[0] == 0x1f && payload[1] == 0x8b);
    return 0;
}
int main(void) {
    const char *positive[] = {NULL, "gzip", "GZIP;q=1", "br, gzip;q=0.25",
                              "*;q=1", "gzip;q=1, *;q=0"};
    const char *negative[] = {"identity", "br", "gzip;q=0", "gzip;q=0, br;q=1",
                              "gzip;q=0.000", "gzip;q=0, *;q=1", "*;q=0",
                              "gzip;q=broken", "gzip;q=2", "gzip, gzip;q=0",
                              "gzip,"};
    char oversized[258];
    memset(oversized, 'g', sizeof(oversized)-1);
    oversized[sizeof(oversized)-1] = '\0';
    for (int language = 0; language < 2; language++) {
        for (size_t i = 0; i < sizeof(positive)/sizeof(*positive); i++) {
            fprintf(stderr, "positive language=%d accept=%s\n", language,
                    positive[i] ? positive[i] : "<absent>");
            CHECK(check(language, positive[i], "192.0.2.10", false, false, true) == 0);
        }
        for (size_t i = 0; i < sizeof(negative)/sizeof(*negative); i++)
            CHECK(check(language, negative[i], "192.0.2.10", false, false, false) == 0);
        CHECK(check(language, "gzip", "bad.invalid", false, false, false) == 0);
        CHECK(check(language, "gzip", "192.0.2.10", true, false, false) == 0);
        CHECK(check(language, oversized, "192.0.2.10", false, false, false) == 0);
        CHECK(check(language, "gzip", "192.0.2.10", false, true, true) == 0);
    }
    puts("C6 security script handlers: both assets, coding, status, host, headers and send failures PASS");
    return 0;
}
'''


class SecurityScriptHandlers(unittest.TestCase):
    def test_real_c_handler_selection(self):
        source = SOURCE.read_text()
        reply = source[source.index("static esp_err_t reply("):source.index("static esp_err_t json_reply(")]
        parser = source.index("static bool gzip_quality(")
        assets = source[source.rfind("#ifdef KATMED_WEB_SECURITY_JS_GZIP", 0, parser):
                        source.index("static esp_err_t page_get(")]
        scripts = source[source.index("static esp_err_t script_get("):source.index("static const char *const language_codes")]
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            build(UI, temporary)
            assembly = [".section .rodata"]
            for name in ASSETS:
                stem = name.replace(".", "_")
                assembly.extend((f".globl _binary_{stem}_start", f"_binary_{stem}_start:",
                                 f'.incbin "{temporary / (name + ".gz")}"',
                                 f".globl _binary_{stem}_end", f"_binary_{stem}_end:"))
            asm = temporary / "assets.S"
            asm.write_text("\n".join(assembly) + "\n")
            harness = temporary / "handlers.c"
            harness.write_text(PREFIX + reply + assets + scripts + SUFFIX)
            program = temporary / "handlers"
            compiled = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                       "-DKATMED_WEB_SECURITY_JS_GZIP=1", str(harness), str(asm),
                                       "-o", str(program)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(program)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
