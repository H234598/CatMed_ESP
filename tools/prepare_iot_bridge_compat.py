#!/usr/bin/env python3
"""Apply the narrow ESP-IDF 6.1 and credential-log bridge to IoT Bridge 1.0.1.

Mesh-Lite 1.0.2 hard-requires this bridge version. Exact upstream bytes are
checked before patching; changed sources fail closed.
"""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess


SOURCE_HASH = "eed2df8383f3aee4a38011cc96f4124aeff4fa0c07abf26e24d8a451c0306a77"
PATCHED_HASH = "d52c296ce82fac29c8a00df8a13b2d744495cefb0ea913f68d5f9a8482a55ddd"
CMAKE_HASH = "11b60a67081ccce988b99e8c9f0cb9664843ec03a968a60da9e87a69bf32a62f"
CMAKE_MARKER = "KATMED_IOT_BRIDGE_IDF6_PATCH_GATE_1"
IDF6_COMMIT = "fff9895c82d744c7237be8847347bdd1b07c6643"
IDF6_LWIP_COMMIT = "c6f2f878e7b0f86033214b85547d579be43351e3"
IDF6_PATH = Path(os.environ.get("KATMED_TOOLS_DIR") or Path.home() / ".local/share/katmed-tools") / "esp-idf-v6.1-acme-20261019"
LWIP_NAPT_BEFORE = "35662ff7531d0a93b0609495a9452c40ba375cd26cc9427ec233a57d16716f33"
LWIP_NAPT_AFTER = "ac8022b28c7135eacf96cb0addbbba99e331d4e2355402369863b186226c8829"
REPLACEMENTS = (
    (b'ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg));',
     b'ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));'),
    (b'ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_cfg));',
     b'ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));'),
    (b'ESP_LOGI(TAG, "[%s] sta ssid: %s password: %s", __func__, conf->sta.ssid, conf->sta.password);',
     b'ESP_LOGI(TAG, "[%s] sta credentials loaded", __func__);'),
    (b'ESP_LOGI(TAG, "[%s] softap ssid: %s password: %s", __func__, conf->ap.ssid, conf->ap.password);',
     b'ESP_LOGI(TAG, "[%s] softap credentials loaded", __func__);'),
    (b'ESP_LOGI(TAG, "[%s] sta ssid: %s password: %s MAC "MACSTR"", __func__, ssid, password, MAC2STR(wifi_cfg.sta.bssid));',
     b'ESP_LOGI(TAG, "[%s] sta credentials loaded", __func__);'),
    (b'ESP_LOGI(TAG, "[%s] sta ssid: %s password: %s", __func__, ssid, password);',
     b'ESP_LOGI(TAG, "[%s] sta credentials loaded", __func__);'),
    (b'ESP_LOGI(TAG, "[%s] softap ssid: %s password: %s", __func__, (char*)wifi_cfg.ap.ssid, (char*)wifi_cfg.ap.password);',
     b'ESP_LOGI(TAG, "[%s] softap credentials loaded", __func__);'),
    (b'ESP_LOGI(TAG, "Found ssid %s", (const char*)router_config.ssid);',
     b'ESP_LOGI(TAG, "Found saved station configuration");'),
    (b'        ESP_LOGI(TAG, "Found password %s", (const char*)router_config.password);\n', b''),
)


def prepare(component: Path, check_only: bool = False) -> str:
    manifest = (component / "idf_component.yml").read_text()
    if "version: 1.0.1" not in manifest:
        raise ValueError("IoT Bridge version differs from required Mesh-Lite 1.0.2 dependency")
    source = component / "src/bridge_wifi.c"
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest == PATCHED_HASH:
        return "verified IoT Bridge 1.0.1 IDF 6.1 / credential-log patch already active"
    if digest != SOURCE_HASH:
        raise ValueError(f"Unexpected IoT Bridge 1.0.1 source SHA-256: {digest}")
    for old, new in REPLACEMENTS:
        if data.count(old) != 1:
            raise ValueError(f"IoT Bridge source patch anchor differs: {old.decode(errors='replace')}")
        data = data.replace(old, new, 1)
    if hashlib.sha256(data).hexdigest() != PATCHED_HASH:
        raise ValueError("IoT Bridge 1.0.1 patch result differs from expected SHA-256")
    if check_only:
        return "verified IoT Bridge 1.0.1 source; IDF 6.1 / credential-log patch required"
    source.write_bytes(data)
    if hashlib.sha256(source.read_bytes()).hexdigest() != PATCHED_HASH:
        raise ValueError("IoT Bridge source patch verification failed")
    _gate_legacy_lwip_patches(component / "CMakeLists.txt", check_only)
    return "verified IoT Bridge 1.0.1 IDF 6.1 / credential-log patch applied"


def _gate_legacy_lwip_patches(cmake: Path, check_only: bool) -> None:
    original = cmake.read_bytes()
    if CMAKE_MARKER.encode() in original:
        return
    if hashlib.sha256(original).hexdigest() != CMAKE_HASH:
        raise ValueError("Unexpected IoT Bridge CMakeLists.txt bytes")
    text = original.decode("utf-8")
    start = text.index("# Apply ESP-IDF Patch\n")
    end = text.index("execute_process(COMMAND\n                git rev-parse --short HEAD", start)
    block = text[start:end]
    if block.count("git apply ") != 4:
        raise ValueError("Unexpected IoT Bridge lwIP patch block")
    replacement = (
        "# " + CMAKE_MARKER + "\n"
        "# The pinned bridge patches target IDF 5.x source. IDF 6.1 already\n"
        "# contains the DHCP server netif binding and NAPT forwarding guard;\n"
        "# its remaining NAPT hooks are installed by the bounded local build\n"
        "# compatibility patch. Do not repeatedly apply 5.x hunks to IDF 6.1.\n"
        "if(IDF_VERSION_MAJOR LESS 6)\n" + block + "endif()\n\n"
    )
    if not check_only:
        cmake.write_text(text[:start] + replacement + text[end:], encoding="utf-8")


def _prepare_idf6_napt(idf: Path, check_only: bool) -> None:
    idf = idf.resolve()
    if idf != IDF6_PATH.resolve():
        raise ValueError("IDF 6.1 compatibility is bound to the isolated Katmed tool install")
    idf_commit = subprocess.check_output(["git", "-C", str(idf), "rev-parse", "HEAD"], text=True).strip()
    lwip = idf / "components/lwip/lwip"
    lwip_commit = subprocess.check_output(["git", "-C", str(lwip), "rev-parse", "HEAD"], text=True).strip()
    if idf_commit != IDF6_COMMIT or lwip_commit != IDF6_LWIP_COMMIT:
        raise ValueError("IDF/lwIP source revision differs from the reviewed 6.1 compatibility patch")
    source = lwip / "src/core/ipv4/ip4_napt.c"
    if not source.resolve().is_relative_to(idf):
        raise ValueError("lwIP NAPT source escapes the isolated SDK")
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest == LWIP_NAPT_AFTER:
        return
    if digest != LWIP_NAPT_BEFORE:
        raise ValueError("Unexpected IDF 6.1 lwIP NAPT source bytes")
    original = source.read_bytes()
    text = original.decode("utf-8")
    anchors = (
        "#if ESP_LWIP && LWIP_IPV4 && IP_NAPT\n\n#include <assert.h>",
        '  LWIP_DEBUGF(NAPT_DEBUG, ("ip_napt_add() failed to insert\\n"));\n  return 0;',
        "#endif /* LWIP_STATS */\n\n#endif /* ESP_LWIP && LWIP_IPV4 && IP_NAPT */",
    )
    if any(text.count(anchor) != 1 for anchor in anchors):
        raise ValueError("IDF 6.1 lwIP NAPT patch anchors differ")
    text = text.replace(anchors[0], "#if ESP_LWIP && LWIP_IPV4 && IP_NAPT\n\n#ifdef ESP_IDF_LWIP_HOOK_FILENAME\n#include ESP_IDF_LWIP_HOOK_FILENAME\n#endif\n\n#include <assert.h>", 1)
    text = text.replace(anchors[1], '  LWIP_DEBUGF(NAPT_DEBUG, ("ip_napt_add() failed to insert\\n"));\n\n#ifdef IP_NAPT_ADD_FAILED_HOOK\n    IP_NAPT_ADD_FAILED_HOOK();\n#endif\n\n  return 0;', 1)
    clear = '''#define IP_GET_V4(ipaddr)          (ip_2_ip4(ipaddr))

void ip_napt_table_clear(void)
{
    struct netif *each_netif;
    u16_t i = 0, next = 0;
    struct ip_napt_entry *t;

    NETIF_FOREACH(each_netif) {
        if (netif_is_up(each_netif)
            && !each_netif->napt
            && !ip_addr_isany(&each_netif->ip_addr)
            && IP_IS_V4(&each_netif->ip_addr)
            && !ip4_addr_isloopback(IP_GET_V4(&each_netif->ip_addr))) {
            /* napt used on another interface, no need for cleanup */
            return;
        }
    }

    uint32_t count = 0;
    for (i = napt_list; i != NO_IDX; i = next) {
        t = NT(i);
        next = t->next;
        count++;
    }
    LWIP_DEBUGF(NAPT_DEBUG, ("before napt count=%"PRIu32"\\r\\n", count));

    for (i = napt_list; i != NO_IDX; i = next) {
        t = NT(i);
        next = t->next;
        ip_napt_free(t);
    }

    for (i = napt_list; i != NO_IDX; i = next) {
        t = NT(i);
        next = t->next;
        count++;
    }
    LWIP_DEBUGF(NAPT_DEBUG, ("after napt count=%"PRIu32"\\r\\n", count));
}

'''
    text = text.replace(anchors[2], "#endif /* LWIP_STATS */\n\n" + clear + "#endif /* ESP_LWIP && LWIP_IPV4 && IP_NAPT */", 1)
    patched = text.encode("utf-8")
    if hashlib.sha256(patched).hexdigest() != LWIP_NAPT_AFTER:
        raise ValueError("IDF 6.1 lwIP NAPT patch result differs from its reviewed hash")
    if not check_only:
        source.write_bytes(patched)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, required=True)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--idf", type=Path, default=Path(os.environ.get("IDF_PATH", ".")))
    args = parser.parse_args()
    print(prepare(args.component, args.check_only))
    _gate_legacy_lwip_patches(args.component / "CMakeLists.txt", args.check_only)
    _prepare_idf6_napt(args.idf, args.check_only)
