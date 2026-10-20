#!/usr/bin/env python3
"""Local, read-only Katmed state evidence. No NVS or product codec reimplementation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import stat
import subprocess
import sys
import tempfile

SDK_PARSER_SHA = "621bdbf0ac60e34ae190f63be10f0f1a4dd4d18c25ebab0cf56ff53a6f2b6c2c"
SDK_CHECK_SHA = "8e427b0245c4dff41e8923cbab09c2d6618d953636c51cb6add722500c50b66a"
STORE_SHA = "4312060293bd160a32ff1bbdfc9baf0a62dfb0c22808486d702d5125b6ce1d13"
CORE_SHA = "426510c837394b09d5f6e78db05a41feefc5155fc64de414b7a7f726259493f6"
NVS_SIZE = 0x6000
OFF = 0x9000


class Invalid(Exception):
    pass


class Binding(Exception):
    pass


class QuietLog:
    def __init__(self):
        self.errors = 0

    def info(self, *unused, **ignored):
        pass

    def red(self, unused):
        self.errors += 1
        return ""

    def yellow(self, message):
        # SDK check_namespaces() reports an unused namespace as a warning.
        # It does not affect the integrity or selection of katmed/state.
        if message == "Found unused namespace.":
            return ""
        self.errors += 1
        return ""

    def __getattr__(self, name):
        if name in ("green", "blue", "cyan", "bold"):
            return lambda unused: ""
        raise AttributeError(name)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_binding(sdk, root):
    tool = sdk / "components/nvs_flash/nvs_partition_tool"
    parser, check = tool / "nvs_parser.py", tool / "nvs_check.py"
    store = root / "firmware/components/katmed_store/katmed_store.cpp"
    core = root / "firmware/components/katmed_core/katmed_core.cpp"
    for file, expected in ((parser, SDK_PARSER_SHA), (check, SDK_CHECK_SHA),
                           (store, STORE_SHA), (core, CORE_SHA)):
        if not file.is_file() or sha(file) != expected:
            raise Binding()
    sys.path.insert(0, str(tool))
    import nvs_parser  # type: ignore
    import nvs_check  # type: ignore
    return nvs_parser, nvs_check


def private_input(path, expected):
    parent = path.parent
    ps = parent.stat()
    if not stat.S_ISDIR(ps.st_mode) or ps.st_uid != os.getuid() or ps.st_mode & 0o077:
        raise Invalid()
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        s = os.fstat(fd)
        if not stat.S_ISREG(s.st_mode) or s.st_uid != os.getuid() or s.st_mode & 0o077 or s.st_size != expected:
            raise Invalid()
        data = bytearray()
        while len(data) < expected:
            chunk = os.read(fd, min(65536, expected-len(data)))
            if not chunk:
                raise Invalid()
            data.extend(chunk)
        if os.read(fd, 1):
            raise Invalid()
        return data
    finally:
        os.close(fd)


def extract(nvs, parser, check):
    partition = parser.NVS_Partition("private", bytearray(nvs))
    log = QuietLog()
    try:
        for page in partition.pages:
            status = page.header["status"]
            if status == "Empty":
                continue
            # The pinned SDK parser exposes but does not reject unknown page
            # versions. Only its defined NVS v1/v2 formats are admissible.
            if status not in ("Active", "Full") or page.header["version"] not in (1, 2):
                raise Invalid()
        check.integrity_check(partition, log)
        if log.errors:
            raise Invalid()
        check.reset_global_variables()
        for page in partition.pages:
            check.check_page_entries(page, log)
        check.assemble_blobs(log)
        check.check_blob_data(log)
        if log.errors:
            raise Invalid()
        namespaces = [e for p in partition.pages for e in p.entries
                      if e.state == "Written" and e.metadata["namespace"] == 0
                      and e.key == "katmed" and e.metadata["type"] == "uint8_t"]
        if len(namespaces) != 1:
            raise Invalid()
        ns = namespaces[0].data["value"]
        indexes = [e for p in partition.pages for e in p.entries
                   if e.state == "Written" and e.metadata["namespace"] == ns
                   and e.key == "state" and e.metadata["type"] == "blob_index"]
        if len(indexes) != 1:
            raise Invalid()
        idx = indexes[0]
        chunks = check.blobs.get(f"{ns:03d}state")
        if not chunks or chunks[0] is not idx or len(chunks) != idx.data["chunk_count"] + 1:
            raise Invalid()
        if not 12 <= idx.data["size"] <= 4096:
            raise Invalid()
        if any(c is check.EMPTY_ENTRY or c.state != "Written" or c.key != "state"
               or c.metadata["namespace"] != ns for c in chunks[1:]):
            raise Invalid()
        raw = b"".join(bytes(child.raw) for c in chunks[1:] for child in c.children)
        if len(raw) < idx.data["size"] or len(raw) - idx.data["size"] >= 32:
            raise Invalid()
        return raw[:idx.data["size"]]
    finally:
        check.reset_global_variables()


def decode(blob, root):
    src = root / "tools/katmed_store_v5_codec.cpp"
    inc1 = root / "firmware/components/katmed_core/include"
    inc2 = root / "firmware/components/katmed_store/include"
    core = root / "firmware/components/katmed_core/katmed_core.cpp"
    with tempfile.TemporaryDirectory(prefix="katmed-evidence-") as tmp:
        out = Path(tmp) / "codec"
        cmd = ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1",
               "-I", str(inc1), "-I", str(inc2), str(src), str(core), "-lz", "-o", str(out)]
        built = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               timeout=60, check=False)
        if built.returncode:
            raise Binding()
        def disable_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        result = subprocess.run([str(out)], input=blob, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, timeout=5, check=False,
                                preexec_fn=disable_core)
        if result.returncode or len(result.stdout) > 16384:
            raise Invalid()
        data = json.loads(result.stdout)
        if data.get("schema") != 5:
            raise Invalid()
        return data


def main():
    arg = argparse.ArgumentParser(description=__doc__)
    arg.add_argument("--input", required=True, type=Path)
    arg.add_argument("--kind", required=True, choices=("nvs", "full-c3", "full-c6"))
    arg.add_argument("--sdk", required=True, type=Path)
    a = arg.parse_args()
    root = Path(__file__).resolve().parent.parent
    try:
        parser, check = check_binding(a.sdk, root)
        expected = {"nvs": NVS_SIZE, "full-c3": 0x400000, "full-c6": 0x800000}[a.kind]
        data = private_input(a.input, expected)
        file_hash = hashlib.sha256(data).hexdigest()
        nvs = data if a.kind == "nvs" else data[OFF:OFF+NVS_SIZE]
        if len(nvs) != NVS_SIZE:
            raise Invalid()
        blob = extract(nvs, parser, check)
        record = decode(blob, root)
        print(json.dumps({"status": "valid", "input_kind": a.kind,
                          "input_bytes": expected, "input_sha256": file_hash,
                          "decoder_id": "store-v5@4b5b9d8",
                          "nvs_sha256": hashlib.sha256(nvs).hexdigest(),
                          "store_blob_sha256": hashlib.sha256(blob).hexdigest(),
                          "record": record}, sort_keys=True, separators=(",", ":")))
        return 0
    except Binding:
        print('{"status":"binding_error"}')
        return 2
    except Exception:
        # SDK/parser/compiler exceptions must never expose raw NVS, paths or tracebacks.
        print('{"status":"invalid_or_ambiguous"}')
        return 1


if __name__ == "__main__":
    sys.exit(main())
