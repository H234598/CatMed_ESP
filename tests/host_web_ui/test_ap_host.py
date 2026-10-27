"""Exercise the production Web Host gate with an IPv4 AP request on an IPv6 listener."""

import subprocess
import tempfile
import unittest
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "firmware/components/katmed_web/katmed_web.c"
POLICY = ROOT / "firmware/components/katmed_web/katmed_web_policy.c"


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PREFIX = r'''
#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "katmed_web_policy.h"
#define ESP_OK 0
#define HTTP_GET 0
typedef int esp_err_t;
typedef struct { void *handle; } httpd_req_t;
static struct { void *https, *redirect; } web = {(void *)1, (void *)2};
static const char *supplied_host;
static bool katmed_web_acme_identifier(char *out,size_t size) {(void)size;out[0]=0;return true;}
static int local_family;
static char response_status[40], response_location[64], response_error[32];
static int httpd_req_to_sockfd(httpd_req_t *req) { (void)req; return 17; }
static int host_getsockname(int fd, struct sockaddr *addr, socklen_t *length) {
    if (fd != 17) return -1;
    if (local_family == AF_INET6) {
        struct sockaddr_in6 value = {.sin6_family = AF_INET6};
        inet_pton(AF_INET6, "::ffff:192.168.4.1", &value.sin6_addr);
        size_t bytes = *length < sizeof(value) ? *length : sizeof(value);
        memcpy(addr, &value, bytes); *length = bytes;
    } else {
        struct sockaddr_in value = {.sin_family = AF_INET};
        inet_pton(AF_INET, "192.168.4.1", &value.sin_addr);
        size_t bytes = *length < sizeof(value) ? *length : sizeof(value);
        memcpy(addr, &value, bytes); *length = bytes;
    }
    return 0;
}
#define getsockname host_getsockname
static size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *key) {
    (void)req; return strcmp(key, "Host") == 0 && supplied_host ? strlen(supplied_host) : 0;
}
static int httpd_req_get_hdr_value_str(httpd_req_t *req, const char *key,
                                       char *out, size_t size) {
    (void)req;
    if (strcmp(key, "Host") || !supplied_host || strlen(supplied_host) + 1 > size)
        return -1;
    memcpy(out, supplied_host, strlen(supplied_host) + 1); return ESP_OK;
}
static esp_err_t error_reply(httpd_req_t *req, const char *status, const char *error) {
    (void)req; snprintf(response_status, sizeof(response_status), "%s", status);
    snprintf(response_error, sizeof(response_error), "%s", error); return ESP_OK;
}
static void httpd_resp_set_status(httpd_req_t *req, const char *status) {
    (void)req; snprintf(response_status, sizeof(response_status), "%s", status);
}
static void httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value) {
    (void)req;
    if (!strcmp(name, "Location")) snprintf(response_location, sizeof(response_location), "%s", value);
}
static esp_err_t httpd_resp_send(httpd_req_t *req, const char *data, int size) {
    (void)req; (void)data; (void)size; return ESP_OK;
}
'''

SUFFIX = r'''
static void run(int family, const char *host) {
    local_family = family; supplied_host = host;
    response_status[0] = response_location[0] = response_error[0] = 0;
    httpd_req_t req = {.handle = web.redirect};
    redirect_get(&req);
}
int main(void) {
    run(AF_INET, "192.168.4.1");
    if (strcmp(response_status, "308 Permanent Redirect") ||
        strcmp(response_location, "https://192.168.4.1/")) return 10;
    run(AF_INET6, "192.168.4.1");
    if (strcmp(response_status, "308 Permanent Redirect") ||
        strcmp(response_location, "https://192.168.4.1/")) {
        fprintf(stderr, "IPv4-mapped AP request rejected: %s %s\n",
                response_status, response_error);
        return 11;
    }
    run(AF_INET6, "evil.test");
    if (strcmp(response_status, "403 Forbidden") || strcmp(response_error, "host")) return 12;
    run(AF_INET6, "192.168.4.1:443");
    if (strcmp(response_status, "403 Forbidden") || strcmp(response_error, "host")) return 13;
    puts("AP IPv4 Host on dual-stack listener: PASS");
    return 0;
}
'''


class ApHost(unittest.TestCase):
    def test_ipv4_ap_host_on_idf_dual_stack_listener(self):
        text = WEB.read_text()
        code = PREFIX + function(text, "static bool request_host(") + "\n" + \
            function(text, "static esp_err_t redirect_get(") + SUFFIX
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "ap_host.c"
            exe = Path(tmp) / "ap_host"
            src.write_text(code)
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "firmware/components/katmed_web/include"),
                            str(src), str(POLICY), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_https_route_capacity_covers_registered_handlers(self):
        source = WEB.read_text()
        registration = source[source.index("static bool register_routes("):]
        main = registration[registration.index("} routes[] = {"):]
        main = main[:main.index("};")]
        count = len(re.findall(r'^\s*\{"/', main, re.MULTILINE))
        for name in ("katmed_web_wifi.c", "katmed_web_reset.c", "katmed_web_media.c"):
            module = (WEB.parent / name).read_text()
            routes = module[module.index("const httpd_uri_t routes[] = {"):]
            routes = routes[:routes.index("};")]
            count += len(re.findall(r'\.uri\s*=\s*"/', routes))
        capacity = int(re.search(r'tls\.httpd\.max_uri_handlers\s*=\s*(\d+)', source).group(1))
        self.assertLessEqual(count, capacity, "HTTPS listener would reject a registered route")


if __name__ == "__main__":
    unittest.main()
