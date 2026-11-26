"""Exercise the real Main certificate producer with synthetic ESP and Web state."""

from pathlib import Path
import subprocess  # nosec B404 - The system compiler and its private test binary.
import tempfile

import test_network_startup as startup


ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "firmware/main/main.c"
UI = ROOT / "firmware/components/katmed_ui/include/katmed_ui.h"
WEB = ROOT / "firmware/components/katmed_web/include"

FAKES = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { ESP_OK, ESP_FAIL };
typedef enum { WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
enum { STORE_NETWORK_ONLINE, STORE_NETWORK_OFFLINE };
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip; } esp_netif_ip_info_t;
typedef struct { bool up; uint32_t address; } esp_netif_t;
typedef struct { uint8_t bssid[6]; } wifi_ap_record_t;
typedef struct {
    bool setup_ap, mesh_started, store_ready, reboot_required;
    struct {
        bool uncertain;
        struct { struct { bool configured; int network_mode; } provision; } record;
    } store;
    struct { bool alarm_active; } state;
    struct { bool initialized, awake; } display_policy;
    struct { uint64_t device; } boot;
} Runtime;

#define IP(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) (unsigned)((ip)->addr & 255u), (unsigned)(((ip)->addr >> 8) & 255u), \
                   (unsigned)(((ip)->addr >> 16) & 255u), (unsigned)((ip)->addr >> 24)
#define DISPLAY_MB_LOCK(mailbox) ((void)0)
#define DISPLAY_MB_UNLOCK(mailbox) ((void)0)

static wifi_mode_t wifi_mode;
static esp_netif_t sta, replaced_sta, ap;
static esp_netif_t *active_sta;
static wifi_ap_record_t association;
static bool wifi_ready, sta_associated, ip_readable, roam_during_ip_read;
static unsigned ap_lookups, station_reads, request;
static uint64_t listener_generation, origin_revision;
static void (*change_between_samples)(void);

static int esp_wifi_get_mode(wifi_mode_t *out)
{
    if (++station_reads == 2 && change_between_samples)
        change_between_samples();
    *out = wifi_mode;
    return wifi_ready ? ESP_OK : ESP_FAIL;
}

static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{
    if (strcmp(key, "WIFI_STA_DEF") == 0)
        return active_sta;
    assert(strcmp(key, "WIFI_AP_DEF") == 0);
    ++ap_lookups;
    return &ap;
}

static bool esp_netif_is_netif_up(esp_netif_t *netif)
{
    return netif->up;
}

static int esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *out)
{
    out->ip.addr = netif->address;
    if (roam_during_ip_read)
        ++association.bssid[5];
    return ip_readable ? ESP_OK : ESP_FAIL;
}

static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *out)
{
    *out = association;
    return sta_associated ? ESP_OK : ESP_FAIL;
}

static unsigned ui_certificate_status_request(void) { return request; }
static uint32_t lv_tick_get(void) { return 70; }
"""

WEB_FAKES = r"""
static UiCertificateStatus ui_certificate_owner_snapshot;

static bool katmed_web_bootstrap_snapshot(KatmedWebBootstrapSnapshot *out)
{
    *out = (KatmedWebBootstrapSnapshot){.generation = listener_generation,
        .origin_revision = origin_revision, .sha256 = {0x12, 0x34},
        .dns_san = "bootstrap.example", .authority = "management.example"};
    return listener_generation != 0;
}

static bool katmed_web_bootstrap_snapshot_matches(uint64_t generation, uint64_t revision)
{
    return listener_generation != 0 && listener_generation == generation && origin_revision == revision;
}
"""

CASES = r"""
static Runtime online(void)
{
    wifi_mode = WIFI_MODE_APSTA;
    sta = (esp_netif_t){.up = true, .address = IP(192, 0, 2, 7)};
    ap = (esp_netif_t){.up = true, .address = IP(198, 51, 100, 1)};
    active_sta = &sta;
    association = (wifi_ap_record_t){.bssid = {2, 10, 20, 30, 40, 50}};
    wifi_ready = sta_associated = ip_readable = true;
    roam_during_ip_read = false;
    listener_generation = 17;
    origin_revision = 3;
    request = 4;
    station_reads = 0;
    change_between_samples = NULL;
    return (Runtime){.mesh_started = true, .store_ready = true,
        .store.record.provision = {.configured = true, .network_mode = STORE_NETWORK_ONLINE},
        .boot.device = 0x112233445566};
}

static UiCertificateStatus display_status(const Runtime *runtime)
{
    publish_certificate_status(runtime);
    UiCertificateStatus out;
    certificate_status(NULL, &out);
    assert(ap_lookups == 0);
    assert(out.request == request && out.sampled_tick == 70);
    return out;
}

static void expect_address(const Runtime *runtime, const char *address)
{
    UiCertificateStatus out = display_status(runtime);
    assert(out.interface == UI_CERTIFICATE_STA);
    assert(strcmp(out.ipv4, address) == 0);
    assert(out.generation == 17 && out.origin_revision == origin_revision && out.device == runtime->boot.device);
    assert(out.sha256[0] == 0x12 && out.sha256[1] == 0x34);
    assert(strcmp(out.dns_san, "bootstrap.example") == 0);
    assert(strcmp(out.origin, "https://management.example") == 0);
}

static void assert_unavailable(const UiCertificateStatus *out)
{
    assert(out->generation == 0 && out->origin_revision == 0 && out->device == 0);
    assert(out->interface == UI_CERTIFICATE_NO_INTERFACE);
    assert(!out->ipv4[0] && !out->dns_san[0] && !out->origin[0]);
    assert(memcmp(out->sha256, (uint8_t[32]){0}, sizeof(out->sha256)) == 0);
}

static void expect_unavailable(const Runtime *runtime)
{
    UiCertificateStatus out = display_status(runtime);
    assert_unavailable(&out);
}

static void change_address(void) { sta.address = IP(192, 0, 2, 18); }
static void disconnect(void) { sta_associated = false; }
static void roam(void) { ++association.bssid[5]; }
static void replace_netif(void) { replaced_sta = sta; active_sta = &replaced_sta; }
static void change_origin_revision(void) { origin_revision += 2; }

int main(void)
{
    Runtime runtime = online();
    assert(ap.address != 0 && ap.address != sta.address);
    expect_address(&runtime, "192.0.2.7");
    wifi_mode = WIFI_MODE_STA;
    expect_address(&runtime, "192.0.2.7");

    wifi_mode = WIFI_MODE_APSTA;
    disconnect(); /* The former STA lease is still cached in the fake. */
    expect_unavailable(&runtime);
    sta_associated = true;
    change_address();
    expect_address(&runtime, "192.0.2.18");

    runtime = online();
    wifi_mode = WIFI_MODE_AP;
    expect_unavailable(&runtime);
    wifi_mode = WIFI_MODE_NULL;
    ap.address = sta.address = 0;
    expect_unavailable(&runtime);
    runtime = online();
    runtime.setup_ap = true;
    expect_unavailable(&runtime);
    runtime.setup_ap = false;
    runtime.store.record.provision.network_mode = STORE_NETWORK_OFFLINE;
    expect_unavailable(&runtime);
    runtime = online();
    runtime.store.record.provision.configured = false;
    expect_unavailable(&runtime);
    runtime = online();
    runtime.mesh_started = false;
    expect_unavailable(&runtime);

    runtime = online();
    wifi_ready = false;
    expect_unavailable(&runtime);
    runtime = online();
    active_sta = NULL;
    expect_unavailable(&runtime);
    runtime = online();
    sta.up = false;
    expect_unavailable(&runtime);
    runtime = online();
    sta.address = 0;
    expect_unavailable(&runtime);
    runtime = online();
    ip_readable = false;
    expect_unavailable(&runtime);

    void (*const changes[])(void) = {change_address, disconnect, roam, replace_netif,
                                    change_origin_revision};
    for (unsigned i = 0; i < sizeof(changes) / sizeof(changes[0]); ++i) {
        runtime = online();
        change_between_samples = changes[i];
        expect_unavailable(&runtime);
    }
    runtime = online();
    roam_during_ip_read = true;
    expect_unavailable(&runtime);

    runtime = online();
    expect_address(&runtime, "192.0.2.7");
    /* Existing consumer-side guard still rejects a different listener generation. */
    ++listener_generation;
    UiCertificateStatus out;
    certificate_status(NULL, &out);
    assert_unavailable(&out);
    runtime = online();
    expect_address(&runtime, "192.0.2.7");
    /* The Origin text stays the same after an A-B-A writer transition. */
    change_origin_revision();
    certificate_status(NULL, &out);
    assert_unavailable(&out);
    puts("APSTA proof binds STA only; loss, address/association changes and ambiguity reject: PASS");
    return 0;
}
"""


def declaration(text, name, kind):
    ending = f"}} {name};"
    stop = text.index(ending) + len(ending)
    return text[text.rindex(f"typedef {kind} {{", 0, stop):stop]


def define(text, name):
    return next(line for line in text.splitlines() if line.startswith(f"#define {name} "))


def main():
    source = MAIN.read_text(encoding="utf-8")
    ui = UI.read_text(encoding="utf-8")
    web = (WEB / "katmed_web.h").read_text(encoding="utf-8")
    policy = (WEB / "katmed_web_policy.h").read_text(encoding="utf-8")
    producer = (
        declaration(source, "CertificateStation", "struct"),
        *(startup.function(source, signature) for signature in (
            "static bool certificate_station(", "static void publish_certificate_status(",
            "static void certificate_status(",
        )),
    )
    compiler = Path("/usr/bin/cc").resolve(strict=True)
    if not compiler.is_file() or compiler.stat().st_uid != 0:
        raise RuntimeError("Host compiler must be a root-owned system executable")
    with tempfile.TemporaryDirectory() as folder:
        program = Path(folder) / "certificate_management.c"
        binary = Path(folder) / "certificate_management"
        program.write_text("\n".join((
            FAKES, declaration(ui, "UiCertificateInterface", "enum"),
            define(ui, "KATMED_UI_CERTIFICATE_NAME_CAPACITY"),
            declaration(ui, "UiCertificateStatus", "struct"),
            define(policy, "KATMED_WEB_IDENTIFIER_MAX"),
            declaration(web, "KatmedWebBootstrapSnapshot", "struct"),
            WEB_FAKES, *producer, CASES,
        )), encoding="utf-8")
        command = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   str(program), "-o", str(binary)]
        subprocess.run(command, check=True, timeout=30)  # nosec B603 - Fixed argv, root-owned compiler.
        subprocess.run([str(binary)], check=True, timeout=10)  # nosec B603 - Private compiled fixture.


if __name__ == "__main__":
    main()
