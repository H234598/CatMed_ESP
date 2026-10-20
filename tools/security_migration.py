#!/usr/bin/env python3
"""Offline logical migration package. Never reads flash or mounts NVS/LittleFS."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
STATIC = {
    "nvs/katmed/state", "nvs/katmed/mesh_history",
    "nvs/katmed_web/auth", "nvs/katmed_web/identity",
    "nvs/katmed_reset/journal",
}
# ARI may also create renewal.json; reject it until source, host, import and
# reset semantics are decided together. Unknown paths fail inventory().
ACME = {"account.json", "directory.json", "account.pem", "order.json",
        "certkey.pem", "certificate.pem"}
IMAGE = re.compile(r"(?:img-[0-9a-f]{8}|cat-[0-9a-f]{2}-[01])\.jpg\Z")
MAX_FILES = 128
MAX_FILE = 128 * 1024
MAX_CONTENT = 0x40000 - 8192  # preflight only; LittleFS metadata needs a later runtime test
DIRECTORIES = {"nvs", "nvs/katmed", "nvs/katmed_web", "nvs/katmed_reset",
               "littlefs", "littlefs/user-images", "littlefs/acme"}


class Invalid(Exception):
    pass


def under_repo(path):
    return path == ROOT or ROOT in path.parents


def private_dir(path):
    s = path.lstat()
    if not stat.S_ISDIR(s.st_mode) or s.st_uid != os.getuid() or s.st_mode & 0o077:
        raise Invalid()


def read_private(path):
    private_dir(path.parent)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        s = os.fstat(fd)
        if not stat.S_ISREG(s.st_mode) or s.st_uid != os.getuid() or s.st_mode & 0o077 or s.st_size > MAX_FILE:
            raise Invalid()
        data = bytearray()
        while len(data) < s.st_size:
            chunk = os.read(fd, s.st_size - len(data))
            if not chunk:
                raise Invalid()
            data.extend(chunk)
        if os.read(fd, 1):
            raise Invalid()
        return bytes(data)
    finally:
        os.close(fd)


def allowed(name):
    if name in STATIC:
        return True
    p = Path(name)
    return (len(p.parts) == 3 and p.parts[:2] == ("littlefs", "user-images")
            and bool(IMAGE.fullmatch(p.name))) or (len(p.parts) == 3
            and p.parts[:2] == ("littlefs", "acme") and p.name in ACME)


def inventory(folder, package=False):
    private_dir(folder)
    found = {}
    for current, dirs, files in os.walk(folder, topdown=True, followlinks=False):
        base = Path(current)
        private_dir(base)
        for name in dirs:
            path = base / name
            if path.relative_to(folder).as_posix() not in DIRECTORIES:
                raise Invalid()
            private_dir(path)
        for name in files:
            path = base / name
            rel = path.relative_to(folder).as_posix()
            if rel == "source-inventory.json" or (package and rel in ("manifest.json", "canonical-state.bin")):
                continue
            if not allowed(rel) or rel in found or len(found) >= MAX_FILES:
                raise Invalid()
            found[rel] = read_private(path)
    if "nvs/katmed/state" not in found:
        raise Invalid()
    declared = json.loads(read_private(folder / "source-inventory.json"))
    actual = {"nvs": sorted(name for name in found if name.startswith("nvs/")),
              "littlefs": sorted(name for name in found if name.startswith("littlefs/"))}
    if declared != actual:
        raise Invalid()
    return found


def compile_bridge(folder):
    binary = folder / "migration-codec"
    cmd = ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1",
           "-I", str(ROOT / "firmware/components/katmed_core/include"),
           "-I", str(ROOT / "firmware/components/katmed_store/include"),
           "-I", str(ROOT / "firmware/components/katmed_mesh/include"),
           str(ROOT / "tools/security_migration_codec.cpp"),
           str(ROOT / "firmware/components/katmed_core/katmed_core.cpp"),
           str(ROOT / "firmware/components/katmed_mesh/katmed_mesh_status.cpp"),
           "-lz", "-o", str(binary)]
    result = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            timeout=90, check=False)
    if result.returncode:
        raise Invalid()
    return binary


def validated_state(files, bridge):
    state = files["nvs/katmed/state"]
    history = files.get("nvs/katmed/mesh_history", b"")
    request = struct.pack("<I", len(state)) + state + struct.pack("<I", len(history)) + history
    result = subprocess.run([str(bridge), "validate"], input=request,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            timeout=5, check=False)
    if result.returncode or len(result.stdout) < 8:
        raise Invalid()
    length = struct.unpack_from("<I", result.stdout)[0]
    if length > 4096 or len(result.stdout) < length + 8:
        raise Invalid()
    history_length = struct.unpack_from("<I", result.stdout, length + 4)[0]
    if len(result.stdout) != 8 + length + history_length or history_length != len(history):
        raise Invalid()
    return result.stdout[4:4 + length]


def validate_other(files):
    auth = files.get("nvs/katmed_web/auth")
    if auth is not None:
        if len(auth) != 1112 or struct.unpack_from("<I", auth)[0] != 0x4b574131 or \
                struct.unpack_from("<H", auth, 4)[0] != 1:
            raise Invalid()
        cert_len = struct.unpack_from("<H", auth, 6)[0]
        if cert_len > 1024 or any(auth[88 + cert_len:]):
            raise Invalid()
    identity = files.get("nvs/katmed_web/identity")
    if identity is not None:
        if len(identity) != 1288 or struct.unpack_from("<I", identity)[0] != 0x4b574931:
            raise Invalid()
        key_len, cert_len = struct.unpack_from("<HH", identity, 4)
        if not (0 < key_len <= 256 and 0 < cert_len <= 1024):
            raise Invalid()
    journal = files.get("nvs/katmed_reset/journal")
    if journal is not None and (len(journal) != 2 or journal[0] != 0 or journal[1] > 5):
        raise Invalid()
    content_bytes = sum(len(value) for name, value in files.items() if name.startswith("littlefs/"))
    if content_bytes > MAX_CONTENT:
        raise Invalid()
    if any(len(value) > MAX_FILE for value in files.values()):
        raise Invalid()


def write_file(path, data):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def publish_new(staging, output):
    """Atomically publish a directory only if its name is still unused (Linux)."""
    renameat2 = getattr(ctypes.CDLL(None, use_errno=True), "renameat2", None)
    if renameat2 is None:
        raise Invalid()
    renameat2.argtypes = (ctypes.c_int, ctypes.c_char_p,
                          ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
    renameat2.restype = ctypes.c_int
    at_fdcwd = getattr(os, "AT_FDCWD", -100)
    rename_noreplace = 1
    if renameat2(at_fdcwd, os.fsencode(staging), at_fdcwd,
                 os.fsencode(output), rename_noreplace) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))


def manifest(files):
    return {"format": "katmed-semantic-v1", "files": {
        name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        for name, data in sorted(files.items())}}


def firmware_capsule(files):
    """Versioned, bounded handoff to the dormant firmware importer.

    The firmware checks the entire inventory again. CRC detects transport
    corruption; it does not authenticate the package or replace manifest SHA.
    """
    records = bytearray()
    for name, data in sorted(files.items()):
        path = name.encode("ascii")
        records += struct.pack("<HHII", len(path), 0, len(data),
                               zlib.crc32(data)) + path + data
    if len(records) + 16 > 0x40000 or len(files) > MAX_FILES:
        raise Invalid()
    return (b"KMI1" + struct.pack("<IHHI", len(records) + 16,
                                 len(files), 0, zlib.crc32(records))
            + records)


def export(source, output, bridge):
    files = inventory(source)
    validate_other(files)
    canonical = validated_state(files, bridge)
    if os.path.lexists(output) or under_repo(output.absolute()) or \
            under_repo(output.resolve()) or not output.parent.exists():
        raise Invalid()
    private_dir(output.parent)
    with tempfile.TemporaryDirectory(prefix=".katmed-migration-", dir=output.parent) as temporary:
        staging = Path(temporary)
        for name, data in files.items():
            write_file(staging / name, data)
        write_file(staging / "source-inventory.json", read_private(source / "source-inventory.json"))
        write_file(staging / "canonical-state.bin", canonical)
        write_file(staging / "manifest.json",
                   json.dumps(manifest(files), sort_keys=True, separators=(",", ":")).encode())
        publish_new(staging, output)


def import_stage(package, output, bridge, max_bytes):
    files = inventory(package, package=True)
    if json.loads(read_private(package / "manifest.json")) != manifest(files):
        raise Invalid()
    validate_other(files)
    canonical = validated_state(files, bridge)
    if canonical != read_private(package / "canonical-state.bin"):
        raise Invalid()
    files["nvs/katmed/state"] = canonical
    if sum(map(len, files.values())) > max_bytes:
        raise Invalid()
    if os.path.lexists(output) or under_repo(output.absolute()) or \
            under_repo(output.resolve()) or not output.parent.exists():
        raise Invalid()
    private_dir(output.parent)
    with tempfile.TemporaryDirectory(prefix=".katmed-migration-", dir=output.parent) as temporary:
        staging = Path(temporary)
        for name, data in files.items():
            write_file(staging / name, data)
        write_file(staging / "firmware-import-v1.bin", firmware_capsule(files))
        publish_new(staging, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("export", "import-stage"))
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--max-stage-bytes", type=int, default=MAX_CONTENT)
    args = parser.parse_args()
    os.umask(0o077)
    try:
        if args.max_stage_bytes < 0 or under_repo(args.source.resolve()):
            raise Invalid()
        with tempfile.TemporaryDirectory(prefix="katmed-migration-codec-") as tmp:
            bridge = compile_bridge(Path(tmp))
            if args.action == "export":
                export(args.source, args.output, bridge)
            else:
                import_stage(args.source, args.output, bridge, args.max_stage_bytes)
        print("migration package: staged")
        return 0
    except (Invalid, OSError, ValueError, KeyError, subprocess.TimeoutExpired):
        print("migration package: rejected", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
