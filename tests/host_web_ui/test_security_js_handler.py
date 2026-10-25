"""Execute the C6 script handlers with their generated payloads and HTTP stubs."""

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.build_web_gzip import ASSETS, build


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/components/katmed_web/katmed_web.c"
UI = ROOT / "firmware/components/katmed_web/ui"


# The ROM entry point cannot execute on x86. This narrow zlib adapter supplies
# its streaming contract; production parsing, bounds, CRC, headers and cleanup
# run unchanged. Target builds separately bind the SDK ROM ABI.
PREFIX = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <zlib.h>
#define INET_ADDRSTRLEN 16
#define HTTPD_RESP_USE_STRLEN -1
#define ESP_OK 0
#define ESP_FAIL -1
#define TINFL_LZ_DICT_SIZE 32768
#define TINFL_STATUS_DONE 0
#define TINFL_STATUS_HAS_MORE_OUTPUT 2
typedef int tinfl_status;
typedef struct {z_stream stream; bool live;} tinfl_decompressor;
static bool fail_inflate;
static int live_streams;
static void host_tinfl_init(tinfl_decompressor *r) {
    memset(r, 0, sizeof(*r));
    if (inflateInit2(&r->stream, -15) != Z_OK) abort();
    r->live = true; ++live_streams;
}
#define tinfl_init host_tinfl_init
static int tinfl_decompress(tinfl_decompressor *r, const unsigned char *in,
    size_t *insize, unsigned char *base, unsigned char *out,
    size_t *outsize, unsigned flags) {
    (void)base; (void)flags;
    r->stream.next_in = (void *)in; r->stream.avail_in = *insize;
    r->stream.next_out = out; r->stream.avail_out = *outsize;
    int rc = fail_inflate ? Z_DATA_ERROR : inflate(&r->stream, Z_NO_FLUSH);
    *insize -= r->stream.avail_in; *outsize -= r->stream.avail_out;
    if (rc == Z_STREAM_END || rc < 0) {
        inflateEnd(&r->stream); r->live = false; --live_streams;
    }
    return rc == Z_STREAM_END ? 0 : rc == Z_OK ? 2 : -1;
}
static uint32_t esp_rom_crc32_le(uint32_t crc, const unsigned char *p, size_t n) {
    return crc32(crc, p, n);
}
static bool fail_alloc;
static int allocations;
static size_t peak_allocation;
static void *script_malloc(size_t size) {
    if (fail_alloc) return NULL;
    void *p = calloc(1, size);
    if (p) {++allocations; peak_allocation = size;}
    return p;
}
static void script_free(void *p) {
    /* Only the host adapter allocates an internal zlib state. */
    tinfl_decompressor *r = p;
    if (r->live) {inflateEnd(&r->stream); --live_streams;}
    --allocations; free(p);
}
/* The corruption copy is test data, not an inflater workspace. */
static void *test_buffer_malloc(size_t size) { return malloc(size); }
static void test_buffer_free(void *p) { free(p); }
#define malloc script_malloc
#define free script_free
typedef int esp_err_t;
typedef struct {
    const char *host, *accept;
    bool read_error;
    int fail_send, sends, finished;
    char status[32], type[64], coding[32], vary[32], cache[32], csp[256];
    char nosniff[32], referrer[32];
    size_t bytes;
    unsigned char body[150000];
} httpd_req_t;
static bool request_host(httpd_req_t *r, char host[INET_ADDRSTRLEN]) {
    if (strcmp(r->host, "192.0.2.10")) return false;
    strcpy(host, r->host); return true;
}
static size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *name) {
    return strcmp(name, "Accept-Encoding") || !r->accept ? 0 : strlen(r->accept);
}
static int httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name,
                                       char *out, size_t capacity) {
    if (strcmp(name, "Accept-Encoding") || !r->accept || r->read_error ||
        strlen(r->accept) >= capacity) return 1;
    strcpy(out, r->accept); return ESP_OK;
}
static int httpd_resp_set_status(httpd_req_t *r, const char *v) {
    snprintf(r->status, sizeof(r->status), "%s", v); return ESP_OK;
}
static int httpd_resp_set_type(httpd_req_t *r, const char *v) {
    snprintf(r->type, sizeof(r->type), "%s", v); return ESP_OK;
}
static int httpd_resp_set_hdr(httpd_req_t *r, const char *key, const char *v) {
#define HDR(k, field) if (!strcmp(key, k)) snprintf(r->field, sizeof(r->field), "%s", v)
    HDR("Content-Encoding", coding); HDR("Vary", vary);
    HDR("Cache-Control", cache); HDR("Content-Security-Policy", csp);
    HDR("X-Content-Type-Options", nosniff); HDR("Referrer-Policy", referrer);
#undef HDR
    return ESP_OK;
}
static int httpd_resp_send_chunk(httpd_req_t *r, const char *body, ssize_t bytes) {
    if (++r->sends == r->fail_send) return 19;
    if (!bytes) {r->finished++; return ESP_OK;}
    if (r->bytes + (size_t)bytes > sizeof(r->body)) abort();
    memcpy(r->body + r->bytes, body, bytes); r->bytes += bytes;
    return ESP_OK;
}
static int httpd_resp_send(httpd_req_t *r, const char *body, ssize_t bytes) {
    return httpd_resp_send_chunk(r, body,
        bytes == HTTPD_RESP_USE_STRLEN ? (ssize_t)strlen(body) : bytes);
}
extern const unsigned char _binary_app_js_start[], _binary_app_js_end[];
extern const unsigned char _binary_i18n_js_start[], _binary_i18n_js_end[];
extern const unsigned char raw_app_start[], raw_app_end[], raw_i18n_start[], raw_i18n_end[];
'''

SUFFIX = r'''
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "failed at %d: %s\n", __LINE__, #x); return 1;}} while (0)
static int check(bool language, const char *accept, const char *host,
                 bool read_error, int failure, int expected) {
    httpd_req_t req = {.accept=accept, .host=host, .read_error=read_error,
                       .fail_send=failure};
    const unsigned char *payload = language ? _binary_i18n_js_start : _binary_app_js_start;
    size_t length = language ? (size_t)(_binary_i18n_js_end - payload) :
                               (size_t)(_binary_app_js_end - payload);
    const unsigned char *raw = language ? raw_i18n_start : raw_app_start;
    size_t raw_length = language ? (size_t)(raw_i18n_end - raw) : (size_t)(raw_app_end - raw);
    int result = language ? language_script_get(&req) : script_get(&req);
    CHECK(allocations == 0 && live_streams == 0);
    if (failure) {CHECK(result == 19 && req.finished == 0); return 0;}
    CHECK(result == ESP_OK);
    if (expected >= 400) {
        CHECK(atoi(req.status) == expected && req.coding[0] == 0);
        return 0;
    }
    CHECK(req.status[0] == 0 || !strcmp(req.status, "200 OK"));
    CHECK(!strcmp(req.vary, "Accept-Encoding"));
    CHECK(!strcmp(req.cache, "no-store"));
    CHECK(!strcmp(req.nosniff, "nosniff"));
    CHECK(!strcmp(req.referrer, "no-referrer"));
    CHECK(!strcmp(req.type, "application/javascript; charset=utf-8"));
    CHECK(strstr(req.csp, "script-src 'self'") != NULL);
    if (expected == 2) {
        CHECK(!strcmp(req.coding, "gzip"));
        CHECK(req.bytes == length && !memcmp(req.body, payload, length));
    } else {
        CHECK(req.coding[0] == 0 && req.finished == 1);
        CHECK(req.bytes == raw_length && !memcmp(req.body, raw, raw_length));
    }
    return 0;
}
int main(void) {
    struct {const char *value; int coding;} cases[] = {
        {NULL,1}, {"",1}, {"identity",1}, {"br",1}, {"gzip;q=0",1},
        {"gzip;q=0, br;q=1",1}, {"gzip;q=0, *;q=1",1},
        {"gzip",2}, {"GZIP;q=1",2}, {"br, gzip;q=0.25",1},
        {"gzip;q=.5",400}, {"gzip;q=0.25,identity;q=0.1",2},
        {"*;q=1",2}, {"gzip;q=1, *;q=0",2},
        {"*;q=0,identity;q=1",1}, {"identity;q=0,gzip;q=1",2},
        {"gzip;q=0,identity;q=0",406}, {"*;q=0",406},
        {"identity;q=0,br",406}, {"gzip;q=broken",400}, {"gzip;q=2",400},
        {"gzip, gzip;q=0",1}, {"gzip,",2}, {"gzip;q=0.000",1},
        {"identity,",1}, {",identity",1}, {"gzip;q=0, ,identity",1},
        {"gzip;q=0,",1}, {"identity, ",1}, {"gzip, ,identity;q=0",2},
        {" , \t, ",1}, {" \t ",1}, {",,gzip,,",2},
        {",gzip;q=0,*;q=1,",1}, {",*;q=0,identity;q=1,",1},
        {",gzip;q=0,identity;q=0,",406}, {",gzip,gzip;q=0,",1},
        {",gzip;q=broken,",400}, {",;q=1,",400},
        {"gzip;q=1.0000",400}, {"gzip;q=1.001",400},
        {" identity ; q=0.5 , gzip ; q=0.8 ",2}, {"gzip;q=0.",1}
    };
    char oversized[258]; memset(oversized, 'g', 257); oversized[257] = 0;
    char empty_list[257]; memset(empty_list, ',', 256); empty_list[256] = 0;
    for (int language = 0; language < 2; language++) {
        CHECK(check(language,empty_list,"192.0.2.10",false,0,1)==0);
        for (size_t i=0; i<sizeof(cases)/sizeof(*cases); i++) {
            fprintf(stderr,"case language=%d accept=%s\n", language, cases[i].value ? cases[i].value : "absent");
            CHECK(check(language,cases[i].value,"192.0.2.10",false,0,cases[i].coding)==0);
        }
        CHECK(check(language,"gzip","bad.invalid",false,0,403)==0);
        CHECK(check(language,"identity","bad.invalid",false,0,403)==0);
        CHECK(check(language,"gzip","192.0.2.10",true,0,400)==0);
        CHECK(check(language,oversized,"192.0.2.10",false,0,400)==0);
        CHECK(check(language,"gzip","192.0.2.10",false,1,2)==0);
        CHECK(check(language,"identity","192.0.2.10",false,1,1)==0);
        CHECK(check(language,"identity","192.0.2.10",false,2,1)==0);
        int chunks = language ? 4 : 2;
        CHECK(check(language,"identity","192.0.2.10",false,chunks+1,1)==0);
        fail_alloc=true;
        CHECK(check(language,"identity","192.0.2.10",false,0,503)==0);
        CHECK(check(language,"gzip","192.0.2.10",false,0,2)==0);
        fail_alloc=false; fail_inflate=true;
        CHECK(check(language,"identity","192.0.2.10",false,0,500)==0);
        fail_inflate=false;
        const unsigned char *data = language ? _binary_i18n_js_start : _binary_app_js_start;
        size_t n = language ? (size_t)(_binary_i18n_js_end-data) : (size_t)(_binary_app_js_end-data);
        CHECK(n>=8);
        unsigned char *bad=test_buffer_malloc(n); CHECK(bad); memcpy(bad,data,n);
        ScriptInflate *work=malloc(sizeof(*work)); CHECK(work);
        CHECK(script_inflate(work,bad,10,NULL)==ESP_FAIL);
        bad[0]=0; CHECK(script_inflate(work,bad,n,NULL)==ESP_FAIL); bad[0]=data[0];
        bad[n-8]^=1; CHECK(script_inflate(work,bad,n,NULL)==ESP_FAIL); bad[n-8]^=1;
        bad[n-4]^=1; CHECK(script_inflate(work,bad,n,NULL)==ESP_FAIL); bad[n-4]^=1;
        bad[n-1]=0xff; CHECK(script_inflate(work,bad,n,NULL)==ESP_FAIL);
        free(work); test_buffer_free(bad); CHECK(allocations==0 && live_streams==0);
    }
    printf("both production handlers: full gzip/raw bytes, negotiation, headers and failure cleanup PASS (host workspace %zu)\n",peak_allocation);
    return 0;
}
'''


class SecurityScriptHandlers(unittest.TestCase):
    def test_real_c_handler_selection(self):
        source = SOURCE.read_text()
        reply = source[source.index("static esp_err_t reply("):source.index("static esp_err_t json_reply(")]
        parser = source.index("static int gzip_quality(")
        assets = source[source.rfind("#ifdef KATMED_WEB_SECURITY_JS_GZIP", 0, parser):
                        source.index("static esp_err_t page_get(")]
        scripts = source[source.index("static esp_err_t script_get("):source.index("static const char *const language_codes")]
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            build(UI, temporary)
            elf_path = os.environ.get("KATMED_WEB_TEST_ELF")
            if elf_path:
                # Same extraction as the independent peer counterprogram: use
                # the linked section and symbol addresses, not generated fixtures.
                from elftools.elf.elffile import ELFFile
                with open(elf_path, "rb") as stream:
                    elf = ELFFile(stream)
                    symbols = elf.get_section_by_name(".symtab")
                    for name in ASSETS:
                        stem = name.replace(".", "_")
                        start, end = (symbols.get_symbol_by_name(
                            f"_binary_{stem}_{suffix}")[0] for suffix in ("start", "end"))
                        self.assertEqual(start["st_shndx"], end["st_shndx"])
                        section = elf.get_section(start["st_shndx"])
                        offset = start["st_value"] - section["sh_addr"]
                        data = section.data()[offset:offset + end["st_value"] - start["st_value"]]
                        self.assertEqual(data, (temporary / (name + ".gz")).read_bytes())
                        (temporary / (name + ".gz")).write_bytes(data)
            assembly = [".section .rodata"]
            for name in ASSETS:
                stem = name.replace(".", "_")
                assembly.extend((f".globl _binary_{stem}_start", f"_binary_{stem}_start:",
                                 f'.incbin "{temporary / (name + ".gz")}"',
                                 f".globl _binary_{stem}_end", f"_binary_{stem}_end:"))
            for name in ASSETS:
                stem = "raw_app" if name == "app.js" else "raw_i18n"
                assembly.extend((f".globl {stem}_start", f"{stem}_start:",
                                 f'.incbin "{UI / name}"',
                                 f".globl {stem}_end", f"{stem}_end:"))
            asm = temporary / "assets.S"
            asm.write_text("\n".join(assembly) + "\n")
            harness = temporary / "handlers.c"
            harness.write_text(PREFIX + reply + assets + scripts + SUFFIX)
            program = temporary / "handlers"
            compiled = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                       "-DKATMED_WEB_SECURITY_JS_GZIP=1", str(harness), str(asm),
                                       "-lz", "-o", str(program)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(program)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
