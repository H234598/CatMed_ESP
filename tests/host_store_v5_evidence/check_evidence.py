#!/usr/bin/env python3
"""Synthetic end-to-end SDK NVS and actual product Store-v5 codec checks."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from zlib import crc32

ROOT = Path(__file__).resolve().parents[2]
SDK = (Path.home() / '.local/share/katmed-tools/esp-idf-v5.5.5')
PY = (Path.home() / '.local/share/katmed-tools/idf-tools/python_env/idf5.5_py3.13_env/bin/python')


def command(args, *, input=None):
    return subprocess.run(args, input=input, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


def private_write(path, data):
    with os.fdopen(os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600), "wb") as out:
        out.write(data)


def nvs_generate(directory, blob, name, extra=""):
    source = directory / (name + ".blob")
    csv = directory / (name + ".csv")
    out = directory / (name + ".bin")
    private_write(source, blob)
    private_write(csv, (f"key,type,encoding,value\nkatmed,namespace,,\nstate,file,binary,{source}\n"+extra).encode())
    result = command([str(PY), "-m", "esp_idf_nvs_partition_gen", "generate",
                      str(csv), str(out), "0x6000"])
    assert result.returncode == 0, "pinned SDK generator failed"
    os.chmod(out, 0o600)
    return out


def inspect(path, kind="nvs", sdk=SDK):
    return command([sys.executable, str(ROOT / "tools/katmed_store_v5_evidence.py"),
                    "--input", str(path), "--kind", kind, "--sdk", str(sdk)])


def result_json(result, expected):
    assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
    assert not result.stderr, "no secret-bearing diagnostics allowed"
    data = json.loads(result.stdout)
    assert len(result.stdout) <= 16384
    return data


def main():
    assert PY.is_file() and SDK.is_dir()
    with tempfile.TemporaryDirectory(prefix="katmed-store-v5-synthetic-") as name:
        directory = Path(name)
        os.chmod(directory, 0o700)
        binary = directory / "fixture"
        built = command(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1",
                         "-I", str(ROOT / "firmware/components/katmed_core/include"),
                         "-I", str(ROOT / "firmware/components/katmed_store/include"),
                         str(ROOT / "tests/host_store_v5_evidence/synthetic_fixture.cpp"),
                         str(ROOT / "firmware/components/katmed_core/katmed_core.cpp"),
                         "-lz", "-o", str(binary)])
        assert built.returncode == 0, built.stderr.decode(errors="replace")
        blob_result = command([str(binary)])
        assert blob_result.returncode == 0
        blob = blob_result.stdout
        assert len(blob) > 12 and blob[:4] == b"KMST" and blob[4] == 5
        nvs = nvs_generate(directory, blob, "good")
        unchanged = nvs.read_bytes()
        good = result_json(inspect(nvs), 0)
        assert nvs.read_bytes() == unchanged
        assert good["record"]["schema"] == 5
        assert good["record"]["last_confirmation"]["id"] == {"device": 17, "epoch": 2, "sequence": 5}
        assert len(good["record"]["pending"]) == 1
        assert "synthetic-pass" not in json.dumps(good)
        assert "a0a1a2" not in json.dumps(good)

        full = directory / "full-c3.bin"
        private_write(full, b"\xff" * 0x9000 + nvs.read_bytes() + b"\xff" * (0x400000-0x9000-0x6000))
        c3 = result_json(inspect(full, "full-c3"), 0)
        assert c3["record"] == good["record"]
        assert c3["nvs_sha256"] == good["nvs_sha256"]
        full_c6 = directory / "full-c6.bin"
        private_write(full_c6, b"\xff" * 0x9000 + nvs.read_bytes() + b"\xff" * (0x800000-0x9000-0x6000))
        assert result_json(inspect(full_c6, "full-c6"), 0)["record"] == good["record"]

        foreign = nvs_generate(directory, blob, "foreign", "private,namespace,,\npassword,data,string,foreign-synthetic-secret\n")
        foreign_json = result_json(inspect(foreign), 0)
        assert foreign_json["record"] == good["record"]
        assert b"foreign-synthetic-secret" not in json.dumps(foreign_json).encode()
        unused_foreign = nvs_generate(directory, blob, "unused-foreign", "other,namespace,,\n")
        assert result_json(inspect(unused_foreign), 0)["record"] == good["record"]
        duplicate = nvs_generate(directory, blob, "duplicate", f"state,file,binary,{directory / 'duplicate.blob'}\n")
        assert result_json(inspect(duplicate), 1)["status"] == "invalid_or_ambiguous"

        corrupt_blob = bytearray(blob)
        corrupt_blob[20] ^= 1
        bad_product = nvs_generate(directory, bytes(corrupt_blob), "bad-product")
        assert result_json(inspect(bad_product), 1)["status"] == "invalid_or_ambiguous"

        corrupt_nvs = directory / "bad-nvs.bin"
        b = bytearray(nvs.read_bytes())
        b[0x130] ^= 1
        private_write(corrupt_nvs, b)
        assert result_json(inspect(corrupt_nvs), 1)["status"] == "invalid_or_ambiguous"

        unknown_page = directory / "unknown-page-version.bin"
        unknown = bytearray(nvs.read_bytes())
        unknown[8] = 0x7f
        unknown[28:32] = crc32(unknown[4:28], 0xffffffff).to_bytes(4, "little")
        private_write(unknown_page, unknown)
        assert result_json(inspect(unknown_page), 1)["status"] == "invalid_or_ambiguous"

        link = directory / "link.bin"
        link.symlink_to(nvs)
        assert result_json(inspect(link), 1)["status"] == "invalid_or_ambiguous"
        os.chmod(nvs, 0o644)
        assert result_json(inspect(nvs), 1)["status"] == "invalid_or_ambiguous"
        os.chmod(nvs, 0o600)
        assert result_json(inspect(nvs, sdk=directory), 2)["status"] == "binding_error"
        wrong_size = directory / "short.bin"
        private_write(wrong_size, nvs.read_bytes()[:-1])
        assert result_json(inspect(wrong_size), 1)["status"] == "invalid_or_ambiguous"
        print("store-v5 private synthetic SDK/codec check: 13 input cases OK")


if __name__ == "__main__":
    main()
