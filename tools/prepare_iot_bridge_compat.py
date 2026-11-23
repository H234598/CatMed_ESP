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
import shutil
import subprocess
import tempfile


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


# Canonical full-tree inventories derived from the registry-lock-verified originals
# and the reviewed, once-prepared local override end states.
OVERRIDE_TREE_HASHES = {
    "dannybackx__acmeclient": "769203aa47cb72e33223c4410572b33bf528e51065c8c50c9ce19139a913f5b3",
    "espressif__iot_bridge": "145e1a35b80364d48a4c5be68b09895c6b243408a80fa1f9cca055ff1a4f30df",
    "lvgl__lvgl": "b2d56e41ae0c57e343ccec497cf08d6a5a01dfb702abaee95179dd0062676a86",
}
ORIGINAL_LOCK_HASHES = {
    "esp32c3": "cffdac58ff00c3889ef5c32c4dcc55486ef07cfeb6dbb1c5102cbbb3f77d48ad",
    "esp32c6": "40cc087b48cb8418166c7caee12f7d829d95f2a57dc52ae25124f6c1e9e0754d",
}


def _inventory_hash(root: Path) -> str:
    if not root.is_dir() or root.is_symlink():
        raise ValueError(f"Invalid override directory: {root}")
    inventory = {}
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"Symlink in override tree: {path}")
        if path.is_file():
            inventory[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
        elif not path.is_dir():
            raise ValueError(f"Unsupported override entry: {path}")
    encoded = json.dumps(inventory, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _validate_override_lock(lock: Path, target: str, allowed: Path, originals: tuple[Path, ...]) -> None:
    if not lock.is_absolute() or not allowed.is_absolute() or allowed.resolve() != allowed:
        raise ValueError("Override lock directory must be a real absolute path")
    if lock.parent != allowed or lock.name != f"override-{target.removeprefix('esp32')}.lock":
        raise ValueError("Override lock must use the task-local target path")
    if lock.is_symlink():
        raise ValueError("Override lock must not be a symlink")
    if lock.exists():
        if not lock.is_file() or lock.stat().st_nlink != 1:
            raise ValueError("Override lock must be an unaliased regular file")
        if any(os.path.samefile(lock, original) for original in originals):
            raise ValueError("Override lock aliases an original registry lock")


def _git_executable() -> str:
    executable = shutil.which("git")
    if executable is None:
        raise RuntimeError("Git is required to validate the SDK checkout")
    return os.path.abspath(executable)


def _git_status(path: Path) -> set[str]:
    # The repository path is a separate argument to a fixed status command.
    output = subprocess.check_output(  # nosec B603
        [_git_executable(), "-C", str(path), "status", "--porcelain=v1", "-z", "--untracked-files=all", "--ignore-submodules=none"],
        env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"},
    )
    return set(output.decode("utf-8").rstrip("\0").split("\0")) if output else set()


def _validate_sdk_worktree(idf: Path) -> None:
    idf = _validate_idf_scope(idf)
    lwip = idf / "components/lwip/lwip"
    mbedtls = idf / "components/mbedtls/mbedtls"
    expected = {
        idf: {" M components/lwip/lwip", " M components/mbedtls/mbedtls"},
        lwip: {" M src/core/ipv4/ip4_napt.c"},
        mbedtls: {"?? framework"},
    }
    for directory, permitted in expected.items():
        if _git_status(directory) != permitted:
            raise ValueError(f"Unexpected tracked, staged or untracked SDK state: {directory}")
    framework = mbedtls / "framework"
    if not framework.is_symlink() or framework.resolve(strict=True) != (mbedtls / "tf-psa-crypto/framework").resolve(strict=True):
        raise ValueError("Unexpected mbedTLS framework link")
    # The checkout is already validated; only Git status is read.
    status = subprocess.check_output([_git_executable(), "-C", str(idf), "submodule", "status", "--recursive"], text=True,  # nosec B603
                                     env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"})
    if any(not line.startswith(" ") for line in status.splitlines()):
        raise ValueError("Unexpected uninitialized, conflicted or changed SDK submodule revision")


def verify_override_end_state(root: Path, idf: Path, target: str, lock: Path, *, before_prepare: bool = False) -> None:
    repo = Path(__file__).resolve().parents[1]
    if target not in ORIGINAL_LOCK_HASHES:
        raise ValueError("Unsupported target for override original binding")
    allowed = root.parent
    originals = tuple(repo / "firmware" / f"dependencies.{t}.lock" for t in sorted(ORIGINAL_LOCK_HASHES))
    _validate_override_lock(lock, target, allowed, originals)
    if root.is_symlink() or root != allowed / "katmed-overrides" or root.resolve() != root:
        raise ValueError("Override root must be the real build-local directory")
    original_lock = repo / "firmware" / f"dependencies.{target}.lock"
    if hashlib.sha256(original_lock.read_bytes()).hexdigest() != ORIGINAL_LOCK_HASHES[target]:
        raise ValueError("Original target registry lock differs from reviewed binding")
    if not before_prepare:
        for name, expected in OVERRIDE_TREE_HASHES.items():
            if _inventory_hash(root / name) != expected:
                raise ValueError(f"Override end tree differs: {name}")
    _prepare_idf6_napt(idf, check_only=True)
    _validate_sdk_worktree(idf)
    lwip = _validate_idf_scope(idf) / "components/lwip/lwip"
    manifest = json.loads((repo / "firmware/compat/idf-6.1-lwip-napt-hooks.json").read_text())
    if hashlib.sha256((lwip / manifest["source"]).read_bytes()).hexdigest() != manifest["source_sha256_after"]:
        raise ValueError("IDF 6.1 lwIP patched end state required")
    # The checked-in patch uses seven-digit object IDs; Git's automatic
    # abbreviation length varies with the SDK repository's object count.
    # The SDK path was validated above, and the diff switches are fixed.
    diff = subprocess.check_output([_git_executable(), "-C", str(lwip), "diff", "--binary", "--abbrev=7"])  # nosec B603
    if hashlib.sha256(diff).hexdigest() != manifest["patch_sha256"]:
        raise ValueError("Unexpected lwIP working-tree diff")
    mbedtls = _validate_idf_scope(idf) / "components/mbedtls/mbedtls"
    # The Git revision query uses a fixed ref in the validated SDK checkout.
    commit = subprocess.check_output([_git_executable(), "-C", str(mbedtls), "rev-parse", "HEAD"], text=True).strip()  # nosec B603
    if commit != manifest["mbedtls_commit"]:
        raise ValueError("mbedTLS revision differs from reviewed IDF binding")


def _fetch_component(fetcher_type, component, pending: Path, path: Path) -> None:
    # Manager 3.0 takes the parent directory, while 3.1 takes the component
    # directory. Bind the effective destination before any download.
    fetcher = fetcher_type(component, pending)
    if fetcher.component_path != path:
        fetcher = fetcher_type(component, path)
    if fetcher.component_path != path:
        raise ValueError("Component fetcher destination differs from the pending tree")
    fetcher.download()


def prepare_override_root(root: Path, idf: Path, target: str, lock: Path) -> None:
    from idf_component_tools.hash_tools.validate import validate_hash_eq_hashfile, validate_hashfile_eq_hashdir
    from idf_component_tools.lock.manager import LockManager
    from idf_component_tools.sources.fetcher import ComponentFetcher
    import prepare_acme_component
    import prepare_lvgl_compat

    verify_override_end_state(root, idf, target, lock, before_prepare=True)
    if root.exists():
        verify_override_end_state(root, idf, target, lock)
        return
    original = Path(__file__).resolve().parents[1] / "firmware" / f"dependencies.{target}.lock"
    components = LockManager(original).load().solved_components
    # A failed or interrupted fetch must never leave an incomplete published root.
    # The private parent also keeps leftovers from an uncatchable termination out
    # of the next attempt; only this invocation's directory is cleaned up.
    with tempfile.TemporaryDirectory(prefix=".katmed-overrides-", dir=root.parent) as tmp:
        pending = Path(tmp) / root.name
        pending.mkdir()
        for name in OVERRIDE_TREE_HASHES:
            path = pending / name
            component = components[name.replace("__", "/", 1)]
            _fetch_component(ComponentFetcher, component, pending, path)
            validate_hash_eq_hashfile(path, component.component_hash)
            validate_hashfile_eq_hashdir(path)
            if name == "dannybackx__acmeclient":
                prepare_acme_component.apply(path)
            elif name == "espressif__iot_bridge":
                prepare(path)
            else:
                prepare_lvgl_compat.prepare(path)
            if _inventory_hash(path) != OVERRIDE_TREE_HASHES[name]:
                raise ValueError(f"Prepared override differs from reviewed end tree: {name}")
        if root.exists() or root.is_symlink():
            raise ValueError("Override root appeared during preparation")
        pending.rename(root)
    verify_override_end_state(root, idf, target, lock)


def _verify_dependency_checkout(name, current, initial, root, firmware, graph, lock):
    from idf_component_tools.hash_tools.validate import validate_hash_eq_hashfile, validate_hashfile_eq_hashdir

    component = name.replace("/", "__")
    if component not in OVERRIDE_TREE_HASHES:
        if initial["component_hash"] != current.get("component_hash") or current["source"].get("type") != "service":
            raise ValueError(f"Registry lock binding differs: {name}")
        if initial["source"]["registry_url"].rstrip("/") != current["source"]["registry_url"].rstrip("/"):
            raise ValueError(f"Registry URL differs: {name}")
        path = (firmware / "managed_components" / component).resolve()
        validate_hash_eq_hashfile(path, initial["component_hash"])
        validate_hashfile_eq_hashdir(path)
        if name == "bblanchon/arduinojson":
            if str(path / "src") not in graph["arduinojson"]["include_dirs"]:
                raise ValueError("ArduinoJson forwarding path differs")
        elif Path(graph[component]["dir"]).resolve() != path:
            raise ValueError(f"Registry graph path differs: {name}")
        return

    path = (root / component).resolve()
    if current["source"].get("type") != "local" or (lock.parent / current["source"]["path"]).resolve() != path:
        raise ValueError(f"Override lock path differs: {name}")
    if Path(graph[component]["dir"]).resolve() != path:
        raise ValueError(f"Override graph path differs: {name}")


def _edge_bindings(entry):
    return sorted((edge["name"], str(edge["version"]),
                   tuple(rule["if"] for rule in edge.get("rules", [])))
                  for edge in entry.get("dependencies", []))


def _verify_resolved_components(old, new, root, firmware, graph, lock):
    for name, initial in old.items():
        current = new[name]
        if initial["version"] != current["version"]:
            raise ValueError(f"Version changed: {name}")
        if _edge_bindings(initial) != _edge_bindings(current):
            raise ValueError(f"Dependency constraints changed: {name}")
        if name == "idf":
            continue
        _verify_dependency_checkout(name, current, initial, root, firmware, graph, lock)


def _verified_config(build, allow_missing_config):
    config_path = Path(json.loads((build / "project_description.json").read_text())["config_file"])
    try:
        config = config_path.read_text()
    except FileNotFoundError:
        if not allow_missing_config:
            raise
        config = None
    if config is not None:
        for symbol in ("FREETYPE", "LIBJPEG_TURBO", "LIBPNG", "LZ4"):
            name = f"CONFIG_LV_USE_{symbol}"
            matches = [line for line in config.splitlines() if line.startswith(name + "=") or line == f"# {name} is not set"]
            if matches != [f"# {name} is not set"]:
                raise ValueError(f"Original inactive dependency condition changed: {symbol}")
    return config


def _verify_graph_edges(firmware, graph, old):
    import yaml
    from idf_component_tools.semver import SimpleSpec, Version

    active = {"target not in [esp32s2, esp32s3]": True,
              "target in [esp32s2, esp32s3]": False, "idf_version >=5.1.4": True}
    for symbol in ("FREETYPE", "LIBJPEG_TURBO", "LIBPNG"):
        active[f"idf_version >=5.5 && $CONFIG{{LV_USE_{symbol}}} == True"] = False
    active["idf_version >=5.5 && $CONFIG{LV_USE_LZ4} == True && $CONFIG{LV_USE_LZ4_INTERNAL} == False"] = False
    def check_edge(source, name, spec):
        if name not in old or Version(old[name]["version"]) not in SimpleSpec(str(spec)):
            raise ValueError(f"Original version edge differs: {source} -> {name}: {spec}")
    manifest = yaml.safe_load((firmware / "main/idf_component.yml").read_text())
    for name, entry in manifest["dependencies"].items():
        check_edge("main", name, entry if isinstance(entry, str) else entry["version"])
    for source, entry in old.items():
        _verify_dependency_edges(source, entry, active, graph, check_edge)


def _verify_dependency_edges(source, entry, active, graph, check_edge):
    for edge in entry.get("dependencies", []):
        conditions = [rule["if"] for rule in edge.get("rules", [])]
        if any(condition not in active for condition in conditions):
            raise ValueError(f"Unknown original dependency condition: {conditions}")
        if all(active[condition] for condition in conditions):
            check_edge(source, edge["name"], edge["version"])
            parent, child = source.replace("/", "__"), edge["name"].replace("/", "__")
            if edge["name"] != "idf" and parent in graph:
                if child not in graph[parent].get("reqs", []) + graph[parent].get("priv_reqs", []):
                    raise ValueError(f"Original active graph edge missing: {source} -> {edge['name']}")


def verify_override_graph(root: Path, target: str, lock: Path, build: Path, *, allow_missing_config: bool = False) -> None:
    import yaml

    repo = Path(__file__).resolve().parents[1]
    firmware = repo / "firmware"
    original = yaml.safe_load((firmware / f"dependencies.{target}.lock").read_text())
    resolved = yaml.safe_load(lock.read_text())
    if original["target"] != target or resolved["target"] != target:
        raise ValueError("Unexpected target in dependency lock")
    old, new = original["dependencies"], resolved["dependencies"]
    if old.keys() != new.keys():
        raise ValueError("Override lock dependency set differs")
    if set(resolved["direct_dependencies"]) != set(original["direct_dependencies"]) | {"espressif/iot_bridge"}:
        raise ValueError("Override lock direct dependency set differs")
    graph = json.loads((build / "project_description.json").read_text())["build_component_info"]
    main_edges = graph["main"].get("reqs", []) + graph["main"].get("priv_reqs", [])
    for name in resolved["direct_dependencies"]:
        if name == "idf":
            continue
        component = "arduinojson" if name == "bblanchon/arduinojson" else name.replace("/", "__")
        if component not in main_edges:
            raise ValueError(f"Direct graph edge missing: main -> {name}")
    _verify_resolved_components(old, new, root, firmware, graph, lock)
    if "espressif__iot_bridge" not in graph["espressif__mesh_lite"]["priv_reqs"] or "lvgl__lvgl" not in graph["espressif__esp_lvgl_port"]["reqs"]:
        raise ValueError("Original transitive override edges missing")
    _verified_config(build, allow_missing_config)
    _verify_graph_edges(firmware, graph, old)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path)
    parser.add_argument("--verify-override-root", type=Path)
    parser.add_argument("--verify-host-lvgl", type=Path)
    parser.add_argument("--prepare-override-root", type=Path)
    parser.add_argument("--target", choices=sorted(ORIGINAL_LOCK_HASHES))
    parser.add_argument("--override-lock", type=Path)
    parser.add_argument("--verify-graph", type=Path)
    parser.add_argument("--allow-missing-config", action="store_true")
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--idf", type=Path, default=Path(os.environ.get("IDF_PATH", ".")))
    args = parser.parse_args()
    if args.verify_host_lvgl:
        if any((args.component, args.verify_override_root, args.prepare_override_root,
                args.target, args.override_lock, args.verify_graph, args.allow_missing_config, args.check_only)):
            parser.error("host LVGL verification takes no other options")
        if _inventory_hash(args.verify_host_lvgl) != OVERRIDE_TREE_HASHES["lvgl__lvgl"]:
            raise ValueError("Host LVGL differs from the reviewed prepared tree")
        print("Verified prepared host LVGL tree")
        raise SystemExit(0)
    if args.allow_missing_config and (not args.verify_override_root or not args.verify_graph):
        parser.error("--allow-missing-config requires override and graph verification")
    if args.prepare_override_root:
        if not args.target or not args.override_lock or args.component or args.check_only or args.verify_override_root or args.verify_graph:
            parser.error("override preparation requires target and lock")
        prepare_override_root(args.prepare_override_root, args.idf, args.target, args.override_lock)
        print("Verified registry originals, prepared override trees and SDK")
        raise SystemExit(0)
    if args.verify_override_root:
        if not args.target or not args.override_lock or args.component or args.check_only:
            parser.error("override verification requires --target and excludes --component/--check-only")
        verify_override_end_state(args.verify_override_root, args.idf, args.target, args.override_lock)
        if args.verify_graph:
            verify_override_graph(args.verify_override_root, args.target, args.override_lock, args.verify_graph,
                                  allow_missing_config=args.allow_missing_config)
        print("Verified original lock, full override end trees and patched IDF 6.1 state")
        raise SystemExit(0)
    if not args.component or args.target or args.override_lock or args.verify_graph:
        parser.error("--component is required outside override verification")
    print(prepare(args.component, args.check_only))
    _gate_legacy_lwip_patches(args.component / "CMakeLists.txt", args.check_only)
    _prepare_idf6_napt(args.idf, args.check_only)
