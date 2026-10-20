#!/usr/bin/env python3
"""Product importer with synthetic input and a committed in-memory NVS model."""
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("migration", ROOT / "tools/security_migration.py")
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)
os.umask(0o077)


def staged_capsule(root, label, files, codec):
    source = root / f"{label}-source"
    source.mkdir(mode=0o700)
    for name, data in files.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        path.write_bytes(data)
        path.chmod(0o600)
    inventory = {
        "nvs": sorted(name for name in files if name.startswith("nvs/")),
        "littlefs": sorted(name for name in files if name.startswith("littlefs/")),
    }
    declared = source / "source-inventory.json"
    declared.write_text(json.dumps(inventory))
    declared.chmod(0o600)
    package, stage = root / f"{label}-package", root / f"{label}-stage"
    migration.export(source, package, codec)
    migration.import_stage(package, stage, codec, migration.MAX_CONTENT)
    return stage / "firmware-import-v1.bin"

with tempfile.TemporaryDirectory(prefix="katmed-import-test-") as temporary:
    root = Path(temporary)
    codec = migration.compile_bridge(root)
    fixture = subprocess.run([str(codec), "synthetic-fixture"], check=True,
                             stdout=subprocess.PIPE).stdout
    length = struct.unpack_from("<I", fixture)[0]
    state = fixture[4:4 + length]
    legacy = subprocess.run([str(codec), "synthetic-v10-fixture"], check=True,
                            stdout=subprocess.PIPE).stdout
    legacy_length = struct.unpack_from("<I", legacy)[0]
    legacy_state = legacy[4:4 + legacy_length]
    binary = root / "import-check"
    subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1",
        "-I", str(ROOT / "firmware/components/katmed_core/include"),
        "-I", str(ROOT / "firmware/components/katmed_store/include"),
        "-I", str(ROOT / "firmware/components/katmed_import/include"),
        "-I", str(ROOT / "firmware/components/katmed_mesh/include"),
        str(ROOT / "tests/host_security_import/check.cpp"),
        str(ROOT / "firmware/components/katmed_import/katmed_import.cpp"),
        str(ROOT / "firmware/components/katmed_store/katmed_store.cpp"),
        str(ROOT / "firmware/components/katmed_mesh/katmed_mesh_status.cpp"),
        str(ROOT / "firmware/components/katmed_core/katmed_core.cpp"),
        "-lz", "-o", str(binary),
    ], check=True)
    partial = staged_capsule(root, "partial", {"nvs/katmed/state": state}, codec)
    with_history = staged_capsule(root, "history", {
        "nvs/katmed/state": state,
        "nvs/katmed/mesh_history": fixture[8 + length:],
    }, codec)
    auth = bytearray(1112)
    struct.pack_into("<IHH", auth, 0, 0x4b574131, 1, 0)
    full = staged_capsule(root, "full", {
        "nvs/katmed/state": state,
        "nvs/katmed/mesh_history": fixture[8 + length:],
        "nvs/katmed_web/auth": auth,
    }, codec)
    renewal = root / "renewal.bin"
    renewal.write_bytes(migration.firmware_capsule({
        "nvs/katmed/state": state,
        "littlefs/acme/renewal.json": b"{}",
    }))
    raw_v10 = root / "raw-v10.bin"
    raw_v10.write_bytes(migration.firmware_capsule({
        "nvs/katmed/state": legacy_state,
    }))
    for mode in ("success", "replay", "replay_wrong_device", "readback",
                 "orphan", "cleanup", "cleanup_wrong_device", "bad_journal",
                 "space", "occupied", "corrupt", "version",
                 "wrong_device"):
        subprocess.run([str(binary), str(partial), mode], check=True)
    for mode in ("history", "replay_history"):
        subprocess.run([str(binary), str(with_history), mode], check=True)
    subprocess.run([str(binary), str(full), "reject"], check=True)
    subprocess.run([str(binary), str(renewal), "reject"], check=True)
    subprocess.run([str(binary), str(raw_v10), "reject"], check=True)
print("synthetic product import: 18 cases passed")
