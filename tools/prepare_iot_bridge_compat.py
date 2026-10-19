#!/usr/bin/env python3
"""Apply the narrow ESP-IDF 6.1 and credential-log bridge to IoT Bridge 1.0.1.

Mesh-Lite 1.0.2 hard-requires this bridge version. Exact upstream bytes are
checked before patching; changed sources fail closed.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


SOURCE_HASH = "eed2df8383f3aee4a38011cc96f4124aeff4fa0c07abf26e24d8a451c0306a77"
PATCHED_HASH = "d52c296ce82fac29c8a00df8a13b2d744495cefb0ea913f68d5f9a8482a55ddd"
CMAKE_HASH = "11b60a67081ccce988b99e8c9f0cb9664843ec03a968a60da9e87a69bf32a62f"
CMAKE_MARKER = "KATMED_IOT_BRIDGE_IDF6_PATCH_GATE_1"
IDF6_PATH = Path(os.environ.get("KATMED_TOOLS_DIR") or Path.home() / ".local/share/katmed-tools") / "esp-idf-v6.1-acme-20261019"
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


def _validate_idf_scope(idf: Path, ci_value: str | None = None) -> Path:
    idf = idf.resolve()
    ci_runner_idf = Path("/opt/esp/idf").resolve()
    if idf != IDF6_PATH.resolve() and not (
        (os.environ.get("CI") if ci_value is None else ci_value) == "true" and idf == ci_runner_idf
    ):
        raise ValueError("IDF 6.1 compatibility is limited to the isolated local SDK or ephemeral CI SDK")
    return idf


def _prepare_idf6_napt(idf: Path, check_only: bool) -> None:
    idf = _validate_idf_scope(idf)
    idf_commit = subprocess.check_output(["git", "-C", str(idf), "rev-parse", "HEAD"], text=True).strip()
    lwip = idf / "components/lwip/lwip"
    lwip_commit = subprocess.check_output(["git", "-C", str(lwip), "rev-parse", "HEAD"], text=True).strip()
    repo = Path(__file__).resolve().parents[1]
    manifest_path = repo / "firmware/compat/idf-6.1-lwip-napt-hooks.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    patch = manifest_path.parent / manifest["patch"]
    if manifest.get("schema") != 1 or hashlib.sha256(patch.read_bytes()).hexdigest() != manifest.get("patch_sha256"):
        raise ValueError("IDF 6.1 lwIP patch file differs from its checked-in hash binding")
    if idf_commit != manifest["idf_commit"] or lwip_commit != manifest["lwip_commit"]:
        raise ValueError("IDF/lwIP source revision differs from the reviewed 6.1 compatibility patch")
    source = lwip / manifest["source"]
    if not source.resolve().is_relative_to(idf):
        raise ValueError("lwIP NAPT source escapes the isolated SDK")
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest == manifest["source_sha256_after"]:
        return
    if digest != manifest["source_sha256_before"]:
        raise ValueError("Unexpected IDF 6.1 lwIP NAPT source bytes")
    if hashlib.sha256(patch.read_bytes()).hexdigest() != manifest["patch_sha256"]:
        raise ValueError("IDF 6.1 lwIP patch hash changed during validation")
    if not check_only:
        subprocess.run(["git", "-C", str(lwip), "apply", "--check", str(patch)], check=True)
        subprocess.run(["git", "-C", str(lwip), "apply", str(patch)], check=True)
        if hashlib.sha256(source.read_bytes()).hexdigest() != manifest["source_sha256_after"]:
            raise ValueError("IDF 6.1 lwIP patch result differs from the checked-in output hash")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, required=True)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--idf", type=Path, default=Path(os.environ.get("IDF_PATH", ".")))
    args = parser.parse_args()
    print(prepare(args.component, args.check_only))
    _gate_legacy_lwip_patches(args.component / "CMakeLists.txt", args.check_only)
    _prepare_idf6_napt(args.idf, args.check_only)
