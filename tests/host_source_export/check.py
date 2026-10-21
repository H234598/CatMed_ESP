#!/usr/bin/env python3
"""Exercise the production source adapter against mock NVS and real POSIX files."""
from pathlib import Path
import json
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="katmed-export-") as base:
    os.umask(0o077)
    root = Path(base) / "store"
    root.mkdir()
    (root / "user-images").mkdir()
    (root / "acme").mkdir()
    (root / "user-images/img-00000001.jpg").write_bytes(b"jpeg")
    (root / "acme/account.json").write_bytes(b"{}")
    before = {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
    binary = Path(base) / "check"
    codec = Path(base) / "codec"
    subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1",
        "-I", str(ROOT / "firmware/components/katmed_core/include"),
        "-I", str(ROOT / "firmware/components/katmed_store/include"),
        "-I", str(ROOT / "firmware/components/katmed_mesh/include"),
        str(ROOT / "tools/security_migration_codec.cpp"),
        str(ROOT / "firmware/components/katmed_core/katmed_core.cpp"),
        str(ROOT / "firmware/components/katmed_mesh/katmed_mesh_status.cpp"),
        "-lz", "-o", str(codec),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fixture = Path(base) / "synthetic.bin"
    with fixture.open("wb") as stream:
        subprocess.run([str(codec), "synthetic-fixture"], check=True, stdout=stream)
    logical = Path(base) / "logical"
    for name in ("nvs/katmed", "nvs/katmed_web", "nvs/katmed_reset",
                 "littlefs/user-images", "littlefs/acme"):
        (logical / name).mkdir(parents=True, exist_ok=True, mode=0o700)
    subprocess.run([
        "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-pthread",
        f'-DKATMED_SOURCE_EXPORT_HOST_TEST="{root}"',
        "-I", str(ROOT / "tests/host_source_export/stubs"),
        "-I", str(ROOT / "firmware/components/katmed_security/include"),
        str(ROOT / "tests/host_source_export/check.c"),
        str(ROOT / "firmware/components/katmed_security/katmed_source_export.c"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary), str(fixture), str(logical)], check=True)
    after = {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
    assert before == after
    paths = sorted(p.relative_to(logical).as_posix() for p in logical.rglob("*") if p.is_file())
    (logical / "source-inventory.json").write_text(json.dumps({
        "nvs": [p for p in paths if p.startswith("nvs/")],
        "littlefs": [p for p in paths if p.startswith("littlefs/")],
    }))
    package = Path(base) / "package"
    subprocess.run(["python3", str(ROOT / "tools/security_migration.py"), "export",
                    "--source", str(logical), "--output", str(package)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert (package / "manifest.json").is_file()
print("source adapter to host package: read-only contract passed")
