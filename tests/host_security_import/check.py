#!/usr/bin/env python3
"""Product importer with synthetic input and a committed in-memory NVS model."""
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

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
    auth[8:24] = bytes(range(16))  # synthetic opaque salt
    auth[24:56] = b"V" * 32        # synthetic opaque verifier
    identity = bytearray(1288)
    struct.pack_into("<IHH", identity, 0, 0x4b574931, 32, 4)
    identity[8:40] = b"K" * 32    # synthetic opaque key-shaped bytes
    identity[264:268] = b"CERT"   # product validator tests real DER on target
    full = staged_capsule(root, "full", {
        "nvs/katmed/state": state,
        "nvs/katmed/mesh_history": fixture[8 + length:],
        "nvs/katmed_web/auth": auth,
        "nvs/katmed_web/identity": identity,
        "nvs/katmed_reset/journal": b"\0\0",
    }, codec)
    identity_only = staged_capsule(root, "identity-only", {
        "nvs/katmed/state": state,
        "nvs/katmed_web/identity": identity,
    }, codec)
    image = root / "image.bin"
    image.write_bytes(migration.firmware_capsule({
        "nvs/katmed/state": state,
        "littlefs/user-images/img-00000001.jpg": b"\xff\xd8fixture\xff\xd9",
    }))
    invalid_classes = []
    for label, replacement in (
        ("auth-only", {"nvs/katmed/state": state, "nvs/katmed_web/auth": auth}),
        ("bad-auth", {"nvs/katmed/state": state,
                       "nvs/katmed_web/auth": bytes([0]) + auth[1:],
                       "nvs/katmed_web/identity": identity}),
        ("long-auth", {"nvs/katmed/state": state,
                        "nvs/katmed_web/auth": auth + b"x",
                        "nvs/katmed_web/identity": identity}),
        ("pending-reset", {"nvs/katmed/state": state,
                            "nvs/katmed_reset/journal": b"\1\1"}),
    ):
        path = root / f"invalid-{label}.bin"
        path.write_bytes(migration.firmware_capsule(replacement))
        invalid_classes.append(path)
    duplicate = full.read_bytes()
    body = duplicate[16:]
    first_path = struct.unpack_from("<H", body)[0]
    first_data = struct.unpack_from("<I", body, 4)[0]
    repeated = body + body[:12 + first_path + first_data]
    duplicate = b"KMI1" + struct.pack("<IHHI", 16 + len(repeated),
                                       struct.unpack_from("<H", duplicate, 8)[0] + 1,
                                       0, zlib.crc32(repeated)) + repeated
    duplicate_path = root / "invalid-duplicate.bin"
    duplicate_path.write_bytes(duplicate)
    invalid_classes.append(duplicate_path)
    renewal = root / "renewal.bin"
    renewal.write_bytes(migration.firmware_capsule({
        "nvs/katmed/state": state,
        "littlefs/acme/renewal.json": b"{}",
    }))
    raw_v10 = root / "raw-v10.bin"
    raw_v10.write_bytes(migration.firmware_capsule({
        "nvs/katmed/state": legacy_state,
    }))
    invalid_history = []
    for label, history_bytes in (("empty", b""), ("short", b"x"),
                                 ("bad-inner-crc", fixture[8 + length:-1] +
                                  bytes([fixture[-1] ^ 1]))):
        capsule = migration.firmware_capsule({
            "nvs/katmed/state": state,
            "nvs/katmed/mesh_history": history_bytes,
        })
        assert len(state) == 4065 and struct.unpack_from("<H", capsule, 8)[0] == 2
        assert struct.unpack_from("<I", capsule, 12)[0] == zlib.crc32(capsule[16:])
        path = root / f"invalid-history-{label}.bin"
        path.write_bytes(capsule)
        invalid_history.append(path)
    for mode in ("success", "replay", "replay_wrong_device", "readback",
                 "orphan", "cleanup", "cleanup_wrong_device", "bad_journal",
                 "space", "occupied", "corrupt", "version",
                 "wrong_device"):
        subprocess.run([str(binary), str(partial), mode], check=True)
    for mode in ("history", "replay_history"):
        subprocess.run([str(binary), str(with_history), mode], check=True)
    subprocess.run([str(binary), str(full), "full_nvs"], check=True)
    subprocess.run([str(binary), str(identity_only), "identity_only"], check=True)
    subprocess.run([str(binary), str(partial), "legacy_cleanup"], check=True)
    subprocess.run([str(binary), str(full), "full_space"], check=True)
    subprocess.run([str(binary), str(full), "reject_web"], check=True)
    subprocess.run([str(binary), str(full), "full_wrong_replay_id"], check=True)
    for key in ("katmed_imp/stage", "katmed_imp/journal", "katmed/state",
                "katmed/mesh_history", "katmed_web/identity",
                "katmed_web/auth", "katmed_reset/journal"):
        for mode in ("fail_write", "fail_readback"):
            subprocess.run([str(binary), str(full), mode, key], check=True)
    subprocess.run([str(binary), str(image), "reject"], check=True)
    for path in invalid_classes:
        subprocess.run([str(binary), str(path), "reject"], check=True)
    subprocess.run([str(binary), str(renewal), "reject"], check=True)
    subprocess.run([str(binary), str(raw_v10), "reject"], check=True)
    for path in invalid_history:
        subprocess.run([str(binary), str(path), "reject"], check=True)
print("synthetic product import: 46 cases passed")
