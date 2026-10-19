#!/usr/bin/env python3
"""Independent synthetic IDF6.1 NVS and real Store-v10 codec review."""

import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import zlib


ROOT = Path(__file__).resolve().parents[2]
SDK = (Path.home() / '.local/share/katmed-tools/esp-idf-v6.1-acme-20261019')
IDF_PY = (Path.home() / '.local/share/katmed-tools/esp-idf-v6.1-tools-acme-20261019/python_env/idf6.1_py3.14_env/bin/python3')
TOOL = ROOT / "tools/katmed_store_v10_evidence.py"
SOURCE = r'''
#include "katmed_store.cpp"
#include <cstdlib>
#include <cstdio>
#include <cstring>

struct DurableFixture {
    uint8_t bytes[KATMED_STORE_BLOB_MAX]{};
    size_t length = 0;
};
static StoreResult durable_read(void *context, uint8_t *out, size_t *length) {
    auto &disk = *static_cast<DurableFixture *>(context);
    if (!disk.length) return STORE_NOT_FOUND;
    if (*length < disk.length) return STORE_CORRUPT;
    std::memcpy(out, disk.bytes, disk.length);
    *length = disk.length;
    return STORE_OK;
}
static StoreResult durable_write(void *context, const uint8_t *bytes, size_t length) {
    auto &disk = *static_cast<DurableFixture *>(context);
    if (length > sizeof(disk.bytes)) return STORE_FULL;
    std::memcpy(disk.bytes, bytes, length);
    disk.length = length;
    return STORE_OK;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 5) return 2;
    const unsigned long long boot = std::strtoull(argv[1], nullptr, 10);
    const unsigned version = argc >= 3 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 10;
    const bool roundtrip = argc >= 4 && std::strcmp(argv[3], "roundtrip") == 0;
    const unsigned requested_profiles = argc == 5 ? static_cast<unsigned>(std::strtoul(argv[4], nullptr, 10)) : KATMED_STORE_WIFI_PROFILE_MAX;
    StoreRecord r{};
    r.device = 91;
    r.epoch = boot;
    r.next_sequence = 30;
    r.image_count = 75;
    r.shared = app_default_settings();
    r.shared.revision.coordinator = r.device;
    r.local = {ORIENTATION_270, DIGITAL_LARGE, r.device};
    const ClockSample old_clock{3000, 7, 0, false, CLOCK_UNAVAILABLE};
    const EventId old_boot{r.device, 7, 1};
    if (app_init(&r.history, &r.shared, &old_clock, old_boot, 75,
                 r.catalog_id) != APP_OK) return 3;
    r.history.recent_event_count = KATMED_RECENT_EVENTS;
    r.history.recent_event_next = 0;
    for (unsigned i = 0; i < KATMED_RECENT_EVENTS; ++i)
        r.history.recent_events[i] = {200 + i, 4, i + 1};
    r.has_history = true;
    r.last_confirmation.valid = true;
    r.last_confirmation.id = {91, 5, 13};
    r.last_confirmation.round = {{91, 5, 1}, 4};
    r.last_confirmation.occurred_at = {2000, 5, 1700000000000LL,
                                       true, CLOCK_SYNCHRONIZED};
    for (unsigned i = 0; i < KATMED_STORE_PENDING_MAX; ++i) {
        r.pending[i] = {{91, 6, 20 + i}, 91, {{91, 6, 1}, 5 + i},
                        {1, 91}, {1000 + i, 6, 0, false, CLOCK_UNAVAILABLE}};
    }
    r.pending_count = KATMED_STORE_PENDING_MAX;
    for (unsigned i = 0; i < KATMED_STORE_CONFLICT_MAX; ++i) {
        r.conflicts[i] = {{91, 7, 40 + i}, 91, {{91, 7, 1}, 20 + i},
                          {2, 91}, {4000 + i, 7, 0, false, CLOCK_UNAVAILABLE}};
    }
    r.conflict_count = KATMED_STORE_CONFLICT_MAX;
    r.group_divergent = true;
    r.provision.configured = true;
    for (unsigned i = 0; i < 16; ++i) {
        r.provision.group[i] = r.bound_group[i] = static_cast<uint8_t>(i + 1);
    }
    for (unsigned i = 0; i < 32; ++i) r.provision.secret[i] = 0xab;
    const char ssid[] = "OWN_SSID_SECRET_661";
    const char password[] = "OWN_PASSWORD_SECRET_662";
    r.provision.ssid_length = sizeof(r.provision.ssid);
    r.provision.password_length = sizeof(r.provision.password);
    r.provision.time_source_length = sizeof(r.provision.time_source);
    std::memset(r.provision.ssid, 'S', sizeof(r.provision.ssid));
    std::memset(r.provision.password, 'P', sizeof(r.provision.password));
    std::memset(r.provision.time_source, 't', sizeof(r.provision.time_source));
    std::memcpy(r.provision.ssid, ssid, sizeof(ssid) - 1);
    std::memcpy(r.provision.password, password, sizeof(password) - 1);
    r.mesh_name_state = STORE_MESH_NAME_ASSIGNED;
    r.mesh_name_index = 1;
    std::memcpy(r.mesh_name, "KatmedESP-001", sizeof("KatmedESP-001"));
    r.mesh_name_count = KATMED_MESH_NAME_MAX_ENTRIES;
    r.mesh_name_next_index = KATMED_MESH_NAME_MAX_ENTRIES + 1;
    for (unsigned i = 0; i < KATMED_MESH_NAME_MAX_ENTRIES; ++i) {
        r.mesh_names[i] = {i == 0 ? r.device : 1000 + i,
                           static_cast<uint16_t>(i + 1)};
    }
    r.wifi_profile_count = static_cast<uint8_t>(requested_profiles);
    r.next_wifi_profile_id = requested_profiles + 1;
    for (unsigned i = 0; i < r.wifi_profile_count; ++i) {
        StoreWifiProfile &profile = r.wifi_profiles[i];
        profile.id = i + 1;
        profile.priority = i + 1;
        profile.ssid_length = sizeof(profile.ssid);
        std::memset(profile.ssid, 'A' + i, sizeof(profile.ssid));
        profile.secured = true;
        profile.password_length = sizeof(profile.password);
        std::memset(profile.password, 'a' + i, sizeof(profile.password));
    }
    std::memcpy(r.wifi_profiles[0].password, password, sizeof(password) - 1);
    uint8_t blob[KATMED_STORE_BLOB_MAX]{};
    size_t size = 0;
    if (encode_record(r, blob, &size, static_cast<uint16_t>(version)) != STORE_OK) return 4;
    if (roundtrip) {
        DurableFixture disk{};
        Store writer{};
        writer.backend = {&disk, durable_read, durable_write};
        if (write_record(&writer, r) != STORE_OK || disk.length != size) return 6;
        Store reopened{};
        StoreRecovery recovery{};
        if (store_open(&reopened, {&disk, durable_read, durable_write}, r.device,
                       r.catalog_id, r.image_count, &recovery) != STORE_OK) return 7;
        const StoreRecord &d = reopened.record;
        if (d.mesh_name_count != r.mesh_name_count ||
            d.mesh_name_next_index != r.mesh_name_next_index ||
            d.mesh_name_state != r.mesh_name_state || d.mesh_name_index != r.mesh_name_index ||
            std::strcmp(d.mesh_name, r.mesh_name) != 0 || d.wifi_profile_count != r.wifi_profile_count ||
            d.pending_count != r.pending_count || d.conflict_count != r.conflict_count ||
            d.group_divergent != r.group_divergent || d.has_history != r.has_history ||
            d.last_confirmation.valid != r.last_confirmation.valid) return 8;
        for (unsigned i = 0; i < r.mesh_name_count; ++i)
            if (d.mesh_names[i].device != r.mesh_names[i].device ||
                d.mesh_names[i].index != r.mesh_names[i].index) return 9;
        for (unsigned i = 0; i < r.wifi_profile_count; ++i) {
            const StoreWifiProfile &a = r.wifi_profiles[i], &b = d.wifi_profiles[i];
            if (a.id != b.id || a.priority != b.priority || a.ssid_length != b.ssid_length ||
                a.password_length != b.password_length || a.secured != b.secured ||
                std::memcmp(a.ssid, b.ssid, sizeof(a.ssid)) != 0 ||
                std::memcmp(a.password, b.password, sizeof(a.password)) != 0) return 10;
        }
        for (unsigned i = 0; i < r.pending_count; ++i)
            if (std::memcmp(&d.pending[i], &r.pending[i], sizeof(r.pending[i])) != 0) return 11;
        for (unsigned i = 0; i < r.conflict_count; ++i)
            if (std::memcmp(&d.conflicts[i], &r.conflicts[i], sizeof(r.conflicts[i])) != 0) return 12;
        if (std::memcmp(&d.history, &r.history, sizeof(r.history)) != 0 ||
            std::memcmp(&d.last_confirmation, &r.last_confirmation,
                        sizeof(r.last_confirmation)) != 0 ||
            d.provision.ssid_length != r.provision.ssid_length ||
            d.provision.password_length != r.provision.password_length ||
            d.provision.time_source_length != r.provision.time_source_length ||
            d.provision.configured != r.provision.configured ||
            std::memcmp(d.provision.group, r.provision.group, sizeof(r.provision.group)) != 0 ||
            std::memcmp(d.provision.secret, r.provision.secret, sizeof(r.provision.secret)) != 0 ||
            std::memcmp(d.bound_group, r.bound_group, sizeof(r.bound_group)) != 0 ||
            std::memcmp(d.provision.ssid, r.provision.ssid, sizeof(r.provision.ssid)) != 0 ||
            std::memcmp(d.provision.password, r.provision.password, sizeof(r.provision.password)) != 0 ||
            std::memcmp(d.provision.time_source, r.provision.time_source,
                        sizeof(r.provision.time_source)) != 0) return 13;
        if (d.history.recent_event_count != KATMED_RECENT_EVENTS) return 14;
        std::printf("roundtrip profiles=%u bytes=%zu names=%u history_events=%u pending=%u conflicts=%u\n",
                    requested_profiles, size, static_cast<unsigned>(d.mesh_name_count),
                    static_cast<unsigned>(d.history.recent_event_count),
                    static_cast<unsigned>(d.pending_count), static_cast<unsigned>(d.conflict_count));
        return 0;
    }
    return std::fwrite(blob, 1, size, stdout) == size ? 0 : 5;
}
'''


def run(command, *, env=None):
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=False, timeout=90, env=env)


def private_write(path, data):
    with os.fdopen(os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600), "wb") as out:
        out.write(data)
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    return path


def generate(tmp, name, blob, *, extra=""):
    source = private_write(tmp / (name + ".blob"), blob)
    csv = private_write(tmp / (name + ".csv"),
                        ("key,type,encoding,value\nkatmed,namespace,,\n"
                         f"state,file,binary,{source}\n" + extra).encode())
    output = tmp / (name + ".bin")
    result = run([str(IDF_PY), "-m", "esp_idf_nvs_partition_gen", "generate",
                  str(csv), str(output), "0x6000"])
    assert result.returncode == 0, "pinned SDK generator failed"
    output.chmod(0o600)
    return output


def inspect(path, expected, *, kind="nvs", sdk=SDK, tmp=None):
    before = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
    env = dict(os.environ)
    if tmp is not None:
        env["TMPDIR"] = str(tmp)
    result = run([sys.executable, str(TOOL), "--input", str(path),
                  "--kind", kind, "--sdk", str(sdk)], env=env)
    assert result.returncode == expected, (path.name, result.returncode)
    assert result.stderr == b"", (path.name, len(result.stderr))
    assert len(result.stdout) <= 16384, path.name
    assert b"OWN_SSID_SECRET_661" not in result.stdout
    assert b"OWN_PASSWORD_SECRET_662" not in result.stdout
    assert b"FOREIGN_SECRET_663" not in result.stdout
    assert b"OWN_SSID_SECRET_661" not in result.stderr
    assert b"OWN_PASSWORD_SECRET_662" not in result.stderr
    assert b"FOREIGN_SECRET_663" not in result.stderr
    if before is not None:
        assert hashlib.sha256(path.read_bytes()).hexdigest() == before, path.name
    return json.loads(result.stdout)


def check_status(result, expected):
    assert result["status"] == expected, result.get("status")


def main():
    assert SDK.is_dir() and IDF_PY.is_file()
    passed = []
    with tempfile.TemporaryDirectory(prefix="katmed-private-review-") as path:
        tmp = Path(path)
        assert stat.S_IMODE(tmp.stat().st_mode) == 0o700
        source = private_write(tmp / "independent_fixture.cpp", SOURCE.encode())
        binary = tmp / "independent_fixture"
        built = run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1",
                     "-I", str(ROOT / "firmware/components/katmed_store"),
                     "-I", str(ROOT / "firmware/components/katmed_store/include"),
                     "-I", str(ROOT / "firmware/components/katmed_core/include"),
                     str(source), str(ROOT / "firmware/components/katmed_core/katmed_core.cpp"),
                     "-lz", "-o", str(binary)])
        assert built.returncode == 0, "independent fixture compilation failed"
        for profile_count, expected_bytes in ((3, 3961), (4, 4064)):
            roundtrip = run([str(binary), "10", "10", "roundtrip", str(profile_count)])
            assert roundtrip.returncode == 0, "durable Store reopen comparison failed"
            assert roundtrip.stdout.decode().strip() == (
                f"roundtrip profiles={profile_count} bytes={expected_bytes} names=64 history_events=16 pending=8 conflicts=8")
        fifth_row = struct.pack("<IBB", 5, 5, 32) + b"N" * 32 + b"\x01\x3f" + b"p" * 63
        assert len(fifth_row) == 103
        five_profile_blob = bytearray(run([str(binary), "10"]).stdout)
        wifi_suffix = 5 + 4 * 103
        wifi_count_offset = len(five_profile_blob) - wifi_suffix
        assert five_profile_blob[wifi_count_offset] == 4
        five_profile_blob[wifi_count_offset] = 5
        five_profile_blob.extend(fifth_row)
        five_profile_blob[6:8] = struct.pack("<H", len(five_profile_blob))
        five_profile_blob[8:12] = struct.pack("<I", zlib.crc32(five_profile_blob[12:], 0))
        assert len(five_profile_blob) == 4167 and len(five_profile_blob) > 4096
        passed.append("four profiles is the full-capacity boundary; a fifth 103-byte row exceeds 4096")
        blobs = {}
        states = {}
        work = tmp / "tool-temp"
        work.mkdir(mode=0o700)
        for boot in (8, 9):
            encoded = run([str(binary), str(boot)])
            assert encoded.returncode == 0, "independent fixture serialization failed"
            blob = encoded.stdout
            assert 12 <= len(blob) <= 4096 and blob[:6] == b"KMST\x0a\x00"
            assert len(blob) == 4064
            blobs[boot] = blob
            nvs = generate(tmp, f"boot{boot}", blob)
            states[boot] = inspect(nvs, 0, tmp=work)
            check_status(states[boot], "valid")
            assert list(work.iterdir()) == [], "temporary decoder files remain"
        a, b = states[8], states[9]
        expected_id = {"device": 91, "epoch": 5, "sequence": 13}
        assert a["record"]["last_confirmation"]["id"] == expected_id
        assert b["record"]["last_confirmation"] == a["record"]["last_confirmation"]
        assert a["record"]["boot_epoch"] == 8 and b["record"]["boot_epoch"] == 9
        assert a["record"]["has_history"] is True
        assert len(a["record"]["pending"]) == 8
        assert len(a["record"]["conflicts"]) == 8
        assert a["record"]["wifi_profile_count"] == 4
        assert a["record"]["wifi_priorities"] == [1, 2, 3, 4]
        assert a["record"]["mesh_name_count"] == 64
        assert a["record"]["mesh_name_next_index"] == 65
        assert a["record"]["mesh_names"] == [
            {"device": 91, "index": 1},
            *[{"device": 1000 + i, "index": i + 1} for i in range(1, 64)],
        ]
        assert a["record"]["wifi_profiles"] == [
            {"id": i, "priority": i, "ssid_length": 32,
             "secured": True, "password_length": 63}
            for i in range(1, 5)
        ]
        assert [p["id"]["sequence"] for p in a["record"]["pending"]] == list(range(20, 28))
        assert all(p["sender"] == 91 and p["parent_revision"] ==
                   {"number": 1, "coordinator": 91} for p in a["record"]["pending"])
        assert [p["id"]["sequence"] for p in a["record"]["conflicts"]] == list(range(40, 48))
        assert all(p["sender"] == 91 and p["parent_revision"] ==
                   {"number": 2, "coordinator": 91} for p in a["record"]["conflicts"])
        assert a["record"]["last_confirmation"]["occurred_at"]["domain"] == 5
        assert a["record"]["pending"][0]["id"]["sequence"] == 20
        passed.append("two saved boot epochs preserve ID, round, time and bounded journals")

        legacy_encoded = run([str(binary), "10", "5"])
        assert legacy_encoded.returncode == 0, "historical v5 fixture serialization failed"
        legacy_blob = legacy_encoded.stdout
        assert 12 <= len(legacy_blob) <= 4096 and legacy_blob[:6] == b"KMST\x05\x00"
        legacy_nvs = generate(tmp, "historical-v5", legacy_blob)
        legacy_state = inspect(legacy_nvs, 0, tmp=work)
        check_status(legacy_state, "valid")
        assert legacy_state["record"]["wifi_profile_count"] == 1
        assert legacy_state["record"]["wifi_priorities"] == [1]
        assert list(work.iterdir()) == [], "legacy migration left temporary decoder files"
        passed.append("historical Store-v5 input migrates through current v10 codec")

        legacy_v7_encoded = run([str(binary), "11", "7"])
        assert legacy_v7_encoded.returncode == 0, "historical v7 fixture serialization failed"
        legacy_v7_blob = legacy_v7_encoded.stdout
        assert 12 <= len(legacy_v7_blob) <= 4096 and legacy_v7_blob[:6] == b"KMST\x07\x00"
        legacy_v7_nvs = generate(tmp, "historical-v7", legacy_v7_blob)
        legacy_v7_state = inspect(legacy_v7_nvs, 0, tmp=work)
        check_status(legacy_v7_state, "valid")
        assert legacy_v7_state["record"]["wifi_profile_count"] == 1
        assert legacy_v7_state["record"]["wifi_priorities"] == [1]
        assert list(work.iterdir()) == [], "v7 migration left temporary decoder files"
        passed.append("historical Store-v7 input migrates through current v10 codec")

        full_data = b"\xff" * 0x9000 + (tmp / "boot8.bin").read_bytes()
        for kind, total in (("full-c3", 0x400000), ("full-c6", 0x800000)):
            full = private_write(tmp / (kind + ".bin"), full_data + b"\xff" * (total - len(full_data)))
            result = inspect(full, 0, kind=kind, tmp=work)
            assert result["record"] == a["record"]
            assert result["nvs_sha256"] == a["nvs_sha256"]
            assert result["input_sha256"] == hashlib.sha256(full.read_bytes()).hexdigest()
            passed.append(kind + " partition offset and full-image hash")

        foreign = generate(tmp, "foreign", blobs[8],
                           extra="foreign,namespace,,\ncredential,data,string,FOREIGN_SECRET_663\n")
        assert inspect(foreign, 0, tmp=work)["record"] == a["record"]
        passed.append("foreign namespace and own provision secrets suppressed")

        unused_foreign = generate(tmp, "unused-foreign", blobs[8],
                                  extra="other,namespace,,\n")
        assert inspect(unused_foreign, 0, tmp=work)["record"] == a["record"]
        passed.append("unrelated empty foreign namespace ignored")

        unknown_page = bytearray((tmp / "boot8.bin").read_bytes())
        assert unknown_page[8] == 0xfe  # SDK generator produced NVS page version 2.
        unknown_page[8] = 0x7f  # The SDK parser reports version 129.
        unknown_page[28:32] = struct.pack(
            "<I", zlib.crc32(unknown_page[4:28], 0xffffffff))
        unknown_path = private_write(tmp / "unknown-page-version.bin", unknown_page)
        check_status(inspect(unknown_path, 1, tmp=work), "invalid_or_ambiguous")
        passed.append("unknown NVS page format rejected")

        duplicate = generate(tmp, "duplicate", blobs[8],
                             extra=f"state,file,binary,{tmp / 'duplicate.blob'}\n")
        check_status(inspect(duplicate, 1, tmp=work), "invalid_or_ambiguous")
        passed.append("ambiguous second live state rejected")

        entry = bytearray((tmp / "boot8.bin").read_bytes())
        at = entry.find(b"state\x00")
        assert at > 0
        entry[at] ^= 1
        check_status(inspect(private_write(tmp / "entry-corrupt.bin", entry), 1, tmp=work),
                     "invalid_or_ambiguous")
        passed.append("NVS entry CRC corruption rejected")

        data = bytearray((tmp / "boot8.bin").read_bytes())
        at = data.find(b"OWN_PASSWORD_SECRET_662")
        assert at > 0
        data[at] ^= 1
        check_status(inspect(private_write(tmp / "data-corrupt.bin", data), 1, tmp=work),
                     "invalid_or_ambiguous")
        passed.append("NVS blob-data CRC corruption rejected")

        for name, blob in (("bad-store-crc", blobs[8][:20] + bytes([blobs[8][20] ^ 1]) + blobs[8][21:]),
                           ("truncated-store", blobs[8][:-1]),
                           ("future-schema", blobs[8][:4] + b"\x63\x00" + blobs[8][6:]),
                           ("bad-magic", b"BAD!" + blobs[8][4:])):
            invalid = generate(tmp, name, blob)
            check_status(inspect(invalid, 1, tmp=work), "invalid_or_ambiguous")
            passed.append(name + " rejected via actual codec")

        short = private_write(tmp / "short-nvs.bin", (tmp / "boot8.bin").read_bytes()[:-1])
        check_status(inspect(short, 1, tmp=work), "invalid_or_ambiguous")
        link = tmp / "link-nvs.bin"
        link.symlink_to(tmp / "boot8.bin")
        check_status(inspect(link, 1, tmp=work), "invalid_or_ambiguous")
        passed.append("short input and symlink fail closed")

        (tmp / "boot8.bin").chmod(0o644)
        check_status(inspect(tmp / "boot8.bin", 1, tmp=work), "invalid_or_ambiguous")
        (tmp / "boot8.bin").chmod(0o600)
        check_status(inspect(tmp / "boot8.bin", 2, sdk=tmp, tmp=work), "binding_error")
        passed.append("private modes and pinned SDK binding enforced")
        assert list(work.iterdir()) == [], "temporary output remains after error"

    for item in passed:
        print("PASS " + item)
    print(f"independent private Store-v10/IDF6.1 review: {len(passed)} checks OK")


if __name__ == "__main__":
    main()
