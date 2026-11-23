#!/usr/bin/env python3
"""Check the real Development001 compiler's diagnostic port binding."""
import argparse
import hashlib
import json
import shlex
import shutil
# The test runs fixed offline CMake and toolchain commands.
import subprocess  # nosec B404
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import yaml

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
import prepare_iot_bridge_compat as compat  # noqa: E402

TARGET = "esp32c3"
PORT = "espressif/esp_lvgl_port"
DEFAULTS = "sdkconfig.defaults;boards/c3.defaults;security.defaults;boards/c3.security.defaults;boards/c3.development001.defaults"


def check_callers(build: Path) -> None:
    description = json.loads((build / "project_description.json").read_text())
    graph = description["build_component_info"]
    commands = json.loads((build / "compile_commands.json").read_text())
    provider = Path(graph["espressif__esp_lvgl_port"]["dir"])
    header = provider / "include/esp_lvgl_port_c3_alarm_probe.h"
    if not header.is_file():
        raise ValueError("Resolved esp_lvgl_port lacks the diagnostic header")
    for component, filename in (("main", "main.c"), ("katmed_ui", "ui.c")):
        source = Path(graph[component]["dir"]) / filename
        entries = [row for row in commands if Path(row["directory"], row["file"]).resolve() == source]
        if len(entries) != 1:
            raise ValueError(f"Expected one compiler invocation for {component}")
        args = entries[0].get("arguments") or shlex.split(entries[0]["command"])
        if "-DKATMED_C3_ALARM_DIAG=1" not in args or f"-I{header.parent}" not in args:
            raise ValueError(f"Missing diagnostic compiler inputs for {component}")


def check_linked_provider(build: Path) -> None:
    elf = build / "katmed_foundation.elf"
    nm = shutil.which("riscv32-esp-elf-nm")
    if nm is None:
        raise ValueError("RISC-V toolchain nm is required")
    symbols = subprocess.check_output([nm, "--defined-only", str(elf)], text=True)  # nosec B603
    names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
    if not {"lvgl_port_c3_alarm_probe_snapshot", "lvgl_port_c3_alarm_probe_render_step"} <= names:
        raise ValueError("Diagnostic caller symbols are absent from the linked ELF")
    map_file = build / "katmed_foundation.map"
    reference = "espressif__esp_lvgl_port/liblvgl_port_lib.a(esp_lvgl_port_c3_alarm_probe.c.obj)"
    if reference not in map_file.read_text():
        raise ValueError("The linked diagnostic symbols lack prepared port archive provenance")
    with (build / "katmed_foundation.bin").open("rb") as image, elf.open("rb") as executable:
        image.seek(24 + 8 + 144)  # ESP image, segment header and app descriptor prefix
        if image.read(32) != hashlib.file_digest(executable, "sha256").digest():
            raise ValueError("App descriptor is not bound to the linked ELF")


def rejected(operation, message: str) -> None:
    try:
        operation()
    except ValueError as error:
        if message not in str(error):
            raise
    else:
        raise AssertionError(f"Guard failed to reject {message}")


def check_inputs(build: Path) -> None:
    root = build / "katmed-overrides"
    lock = build / "override-c3.lock"
    idf = Path(json.loads((build / "project_description.json").read_text())["idf_path"])
    provider = root / compat.DIAGNOSTIC_PORT
    header = provider / "include/esp_lvgl_port_c3_alarm_probe.h"

    def verify():
        compat.verify_override_end_state(root, idf, TARGET, lock, development001=True)

    verify()
    rejected(lambda: compat.verify_override_end_state(root, idf, "esp32c6", build / "override-c6.lock",
                                                      development001=True), "requires ESP32-C3")
    rejected(lambda: compat.verify_override_end_state(root, idf, TARGET, lock), "in a normal override tree")
    read, glob = Path.read_bytes, Path.rglob
    with patch.object(Path, "read_bytes", lambda path: read(path) + b"\n" if path == header else read(path)):
        rejected(verify, "Override end tree differs")
    with patch.object(Path, "rglob", lambda path, pattern: (p for p in glob(path, pattern) if p != header)):
        rejected(verify, "Override end tree differs")
    adapter = compat.DIAGNOSTIC_COMPAT / "esp-lvgl-port-2.9.0-c3-diagnostic.patch"
    with patch.object(Path, "read_bytes", lambda path: read(path) + b"\n" if path == adapter else read(path)):
        rejected(verify, "port adapter differs")

    registry = yaml.safe_load((REPO / "firmware/dependencies.esp32c3.lock").read_text())["dependencies"][PORT]
    for version, digest in (("2.9.1", registry["component_hash"]), (registry["version"], "0" * 64)):
        component = SimpleNamespace(version=version, component_hash=digest)
        rejected(lambda: compat._prepare_diagnostic_port(provider, component), "registry version or component hash")


def check_graph_mutations(build: Path) -> None:
    root = build / "katmed-overrides"
    lock = build / "override-c3.lock"
    description = build / "project_description.json"
    read = Path.read_text

    def verify():
        compat.verify_override_graph(root, TARGET, lock, build, development001=True)

    verify()
    graph = json.loads(read(description))
    graph["build_component_info"]["katmed_ui"]["priv_reqs"].remove(compat.DIAGNOSTIC_PORT)
    with patch.object(Path, "read_text", lambda p: json.dumps(graph) if p == description else read(p)):
        rejected(verify, "Development001 graph edge missing")
    graph = json.loads(read(description))
    graph["build_component_info"][compat.DIAGNOSTIC_PORT]["dir"] = str(build / "unbound-provider")
    with patch.object(Path, "read_text", lambda p: json.dumps(graph) if p == description else read(p)):
        rejected(verify, "Override graph path differs")
    resolved = yaml.safe_load(read(lock))
    resolved["dependencies"][PORT]["source"]["path"] = "unbound-provider"
    with patch.object(Path, "read_text", lambda p: yaml.safe_dump(resolved) if p == lock else read(p)):
        rejected(verify, "Override lock path differs")
    config = Path(graph["config_file"])
    changed = read(config).replace("CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y",
                                   "# CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT is not set")
    with patch.object(Path, "read_text", lambda p: changed if p == config else read(p)):
        rejected(verify, "Resolved old-C3 setting differs")


def check_rejected_setup() -> None:
    cmake = shutil.which("cmake")
    if cmake is None:
        raise ValueError("CMake is required")
    cases = (
        ("esp32c6", DEFAULTS, "ON", "ON", "requires the complete C3 Development001 profile"),
        (TARGET, "sdkconfig.defaults;boards/c3.defaults", "ON", "ON", "requires the complete C3 Development001 profile"),
        (TARGET, DEFAULTS, "ON", "OFF", "requires the complete C3 Development001 profile"),
        (TARGET, DEFAULTS, "OFF", "ON", "requires its diagnostic port"),
    )
    with tempfile.TemporaryDirectory(prefix="katmed-development001-rejections-") as directory:
        for index, (target, defaults, diagnostic, stack, reason) in enumerate(cases):
            command = [cmake, "-S", str(REPO / "firmware"), "-B", str(Path(directory) / str(index)),
                       f"-DIDF_TARGET={target}", f"-DSDKCONFIG_DEFAULTS={defaults}",
                       f"-DKATMED_C3_ALARM_DIAG={diagnostic}", f"-DKATMED_FULL_STACK={stack}"]
            result = subprocess.run(command, text=True, capture_output=True, check=False)  # nosec B603
            if result.returncode != 1 or reason not in result.stderr:
                raise AssertionError(f"Development001 configure rejection case {index + 1} failed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    build = parser.parse_args().build.resolve()
    check_callers(build)
    check_linked_provider(build)
    check_inputs(build)
    check_graph_mutations(build)
    check_rejected_setup()
    print("Development001 compiler, linked app, source/graph binding and negative cases passed")
