#!/usr/bin/env python3
"""Synthetic, protected, offline logical migration contract tests."""
import json
import importlib.util
import errno
import hashlib
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile
import unittest
import zlib
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/security_migration.py"
CODEC = ROOT / "tools/security_migration_codec.cpp"


def put(path, data):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    path.write_bytes(data)
    path.chmod(0o600)


class MigrationContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.old_umask = os.umask(0o077)
        cls.binary_dir = tempfile.TemporaryDirectory(prefix="katmed-synthetic-bridge-")
        cls.codec = Path(cls.binary_dir.name) / "codec"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1",
                        "-I", str(ROOT / "firmware/components/katmed_core/include"),
                        "-I", str(ROOT / "firmware/components/katmed_store/include"),
                        "-I", str(ROOT / "firmware/components/katmed_mesh/include"),
                        str(CODEC), str(ROOT / "firmware/components/katmed_core/katmed_core.cpp"),
                        str(ROOT / "firmware/components/katmed_mesh/katmed_mesh_status.cpp"),
                        "-lz", "-o", str(cls.codec)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.fixture = subprocess.run([str(cls.codec), "synthetic-fixture"], check=True,
                                     stdout=subprocess.PIPE).stdout
        cls.v11_fixture = subprocess.run([str(cls.codec), "synthetic-v11-fixture"], check=True,
                                         stdout=subprocess.PIPE).stdout
        cls.v10_fixture = subprocess.run([str(cls.codec), "synthetic-v10-fixture"], check=True,
                                         stdout=subprocess.PIPE).stdout
        cls.v9_fixture = subprocess.run([str(cls.codec), "synthetic-v9-fixture"], check=True,
                                        stdout=subprocess.PIPE).stdout
        cls.legacy_fixtures = {
            version: subprocess.run([str(cls.codec), f"synthetic-v{version}-fixture"],
                                    check=True, stdout=subprocess.PIPE).stdout
            for version in (5, 7, 8)
        }
        cls.store_codec = Path(cls.binary_dir.name) / "store-codec"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1",
                        "-I", str(ROOT / "firmware/components/katmed_core/include"),
                        "-I", str(ROOT / "firmware/components/katmed_store/include"),
                        str(ROOT / "tools/katmed_store_v10_codec.cpp"),
                        str(ROOT / "firmware/components/katmed_core/katmed_core.cpp"),
                        "-lz", "-o", str(cls.store_codec)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    @classmethod
    def tearDownClass(cls):
        cls.binary_dir.cleanup()
        os.umask(cls.old_umask)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="katmed-synthetic-migration-")
        self.root = Path(self.temp.name)
        self.source = self.root / "logical"
        self.source.mkdir(mode=0o700)
        state_len = struct.unpack_from("<I", self.fixture)[0]
        self.state = self.fixture[4:4 + state_len]
        history_len = struct.unpack_from("<I", self.fixture, 4 + state_len)[0]
        self.history = self.fixture[8 + state_len:]
        assert len(self.history) == history_len == 2152
        assert int.from_bytes(self.history[6:8], "big") == 256
        assert len(self.state) <= 4096
        put(self.source / "nvs/katmed/state", self.state)
        put(self.source / "nvs/katmed/mesh_history", self.history)
        auth = bytearray(1112)
        struct.pack_into("<IHH", auth, 0, 0x4b574131, 1, 0)
        put(self.source / "nvs/katmed_web/auth", auth)
        identity = bytearray(1288)
        struct.pack_into("<IHH", identity, 0, 0x4b574931, 32, 4)
        put(self.source / "nvs/katmed_web/identity", identity)
        put(self.source / "nvs/katmed_reset/journal", b"\0\0")
        put(self.source / "littlefs/user-images/img-00000001.jpg", b"\xff\xd8synthetic\xff\xd9")
        put(self.source / "littlefs/acme/account.json", b"{\"synthetic\":true}")
        paths = [path.relative_to(self.source).as_posix() for path in self.source.rglob("*")
                 if path.is_file()]
        put(self.source / "source-inventory.json", json.dumps({
            "nvs": sorted(path for path in paths if path.startswith("nvs/")),
            "littlefs": sorted(path for path in paths if path.startswith("littlefs/")),
        }).encode())
        self.package = self.root / "package"
        self.staged = self.root / "staged"

    def tearDown(self):
        self.temp.cleanup()

    def run_tool(self, action, source, output, *extra):
        return subprocess.run(["python3", str(TOOL), action, "--source", str(source),
                               "--output", str(output), *extra], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, check=False)

    def test_full_current_v13_roundtrip(self):
        self.assertEqual(len(self.state), 4096)
        decoded = json.loads(subprocess.run([str(self.store_codec)], input=self.state,
                                            stdout=subprocess.PIPE, check=True).stdout)
        self.assertEqual(decoded["schema"], 14)
        self.assertEqual(decoded["language"], 1)  # synthetic German selection
        self.assertEqual(decoded["wifi_profile_count"], 4)
        self.assertEqual(decoded["mesh_name_count"], 64)
        self.assertEqual(decoded["wifi_priorities"], [1, 2, 3, 4])
        self.assertTrue(decoded["has_history"])
        self.assertEqual(len(decoded["pending"]), 8)
        self.assertEqual(len(decoded["conflicts"]), 8)
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        self.assertEqual((self.staged / "nvs/katmed/state").read_bytes(), self.state)
        capsule = (self.staged / "firmware-import-v1.bin").read_bytes()
        self.assertEqual(capsule[:4], b"KMI1")
        self.assertEqual(struct.unpack_from("<I", capsule, 4)[0], len(capsule))
        self.assertEqual(struct.unpack_from("<H", capsule, 8)[0], 7)
        self.assertEqual((self.staged / "nvs/katmed/mesh_history").read_bytes(), self.history)
        for name in ("nvs/katmed_web/auth", "nvs/katmed_web/identity",
                     "nvs/katmed_reset/journal", "littlefs/user-images/img-00000001.jpg",
                     "littlefs/acme/account.json"):
            self.assertEqual((self.staged / name).read_bytes(), (self.source / name).read_bytes())
        for folder in (self.package, self.staged):
            self.assertEqual(stat.S_IMODE(folder.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE((folder / "nvs/katmed/state").stat().st_mode), 0o600)
        self.assertEqual(json.loads((self.package / "manifest.json").read_text())["format"],
                         "katmed-semantic-v1")

    def test_bridge_ip_and_u8_roundtrip_with_maximal_store(self):
        values = {
            "WIFI_AP_DEF": bytes(range(12)), "WIFI_AP__check": b"\x01",
            "WIFI_STA_DEF": bytes(range(12, 24)), "WIFI_STA_check": b"\x00",
        }
        inventory = json.loads((self.source / "source-inventory.json").read_text())
        for key, value in values.items():
            path = f"nvs/netif_ip_info/{key}"
            put(self.source / path, value)
            inventory["nvs"].append(path)
        inventory["nvs"].sort()
        put(self.source / "source-inventory.json", json.dumps(inventory).encode())
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        for key, value in values.items():
            self.assertEqual((self.staged / f"nvs/netif_ip_info/{key}").read_bytes(), value)
        self.assertEqual(len(self.state), 4096)
        self.assertEqual(len(self.history), 2152)
        self.assertEqual(struct.unpack_from("<H", (self.staged / "firmware-import-v1.bin").read_bytes(), 8)[0], 11)

    def test_bridge_orphan_check_and_invalid_records(self):
        path = "nvs/netif_ip_info/WIFI_AP__check"
        inventory = json.loads((self.source / "source-inventory.json").read_text())
        inventory["nvs"].append(path)
        inventory["nvs"].sort()
        put(self.source / "source-inventory.json", json.dumps(inventory).encode())
        put(self.source / path, b"\x00")
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        for value in (b"", b"\x02", b"\x00\x00"):
            with self.subTest(value=value):
                put(self.source / path, value)
                candidate = self.root / f"bad-bridge-{len(value)}-{value.hex()}"
                self.assertNotEqual(self.run_tool("export", self.source, candidate).returncode, 0)
                self.assertFalse(candidate.exists())

    def test_legacy_v10_migrates_to_v13_english_without_losing_state(self):
        length = struct.unpack_from("<I", self.v10_fixture)[0]
        old_state = self.v10_fixture[4:4 + length]
        self.assertEqual(length, 4064)
        put(self.source / "nvs/katmed/state", old_state)
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        self.assertEqual((self.package / "nvs/katmed/state").read_bytes(), old_state)
        migrated = (self.staged / "nvs/katmed/state").read_bytes()
        self.assertEqual(migrated[4], 14)
        self.assertEqual(len(migrated), 4096)
        self.assertEqual(migrated[12:-32], old_state[12:])
        self.assertEqual(migrated[-31:-28], b"\x00\x00\x00")  # Online, no setup request
        parsed = json.loads(subprocess.run([str(self.store_codec)], input=migrated,
                                           stdout=subprocess.PIPE, check=True).stdout)
        self.assertEqual(parsed["language"], 0)  # English for pre-v11 source
        self.assertEqual(parsed["wifi_profile_count"], 4)
        self.assertEqual(parsed["mesh_name_count"], 64)
        self.assertTrue(parsed["has_history"])
        self.assertEqual(len(parsed["pending"]), 8)
        self.assertEqual(len(parsed["conflicts"]), 8)
        self.assertEqual((self.staged / "nvs/katmed/mesh_history").read_bytes(), self.history)

    def test_legacy_v11_preserves_language_and_payload_in_current_stage(self):
        length = struct.unpack_from("<I", self.v11_fixture)[0]
        old_state = self.v11_fixture[4:4 + length]
        self.assertEqual(length, 4065)
        put(self.source / "nvs/katmed/state", old_state)
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        self.assertEqual((self.package / "nvs/katmed/state").read_bytes(), old_state)
        current = (self.staged / "nvs/katmed/state").read_bytes()
        self.assertEqual(current[4], 14)
        self.assertEqual(current[12:-31], old_state[12:])
        self.assertEqual(current[-32:-28], b"\x01\x00\x00\x00")
        self.assertEqual((self.staged / "nvs/katmed/mesh_history").read_bytes(), self.history)

    def test_legacy_v12_preserves_full_payload_and_defaults_auto(self):
        fixture = subprocess.run([str(self.codec), "synthetic-v12-fixture"],
                                 check=True, stdout=subprocess.PIPE).stdout
        length = struct.unpack_from("<I", fixture)[0]
        old_state = fixture[4:4 + length]
        self.assertEqual(length, 4067)
        put(self.source / "nvs/katmed/state", old_state)
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        current = (self.staged / "nvs/katmed/state").read_bytes()
        self.assertEqual(current[4], 14)
        self.assertEqual(len(current), length + 29)
        self.assertEqual(current[12:-29], old_state[12:])
        self.assertEqual(current[-29], 0)
        self.assertEqual((self.package / "nvs/katmed/state").read_bytes(), old_state)
        self.assertEqual((self.staged / "nvs/katmed/mesh_history").read_bytes(), self.history)

    def test_published_v13_preserves_full_payload_and_display_defaults(self):
        fixture = subprocess.run([str(self.codec), "synthetic-v13-fixture"], check=True,
                                 stdout=subprocess.PIPE).stdout
        length = struct.unpack_from("<I", fixture)[0]
        old_state = fixture[4:4 + length]
        self.assertEqual(length, 4068)
        put(self.source / "nvs/katmed/state", old_state)
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        current = (self.staged / "nvs/katmed/state").read_bytes()
        self.assertEqual(len(current), 4096)
        self.assertEqual(current[4], 14)
        self.assertEqual(current[12:-28], old_state[12:])
        self.assertEqual(current[-28], 100)  # brightness100, night disabled
        times = int.from_bytes(current[-27:-24], "little")
        self.assertEqual((times & 2047, times >> 11), (1320, 420))
        self.assertEqual(current[-24:], bytes(24))
        self.assertEqual((self.staged / "nvs/katmed/mesh_history").read_bytes(), self.history)

    def test_corrupt_state_rejected_without_package(self):
        state = bytearray(self.state)
        state[-1] ^= 1
        put(self.source / "nvs/katmed/state", state)
        self.assertNotEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertFalse(self.package.exists())

    def test_invalid_v13_language_rejected_with_valid_crc(self):
        state = bytearray(self.state)
        state[-32] = 12  # outside the twelve persisted locale choices
        struct.pack_into("<I", state, 8, zlib.crc32(state[12:]))
        put(self.source / "nvs/katmed/state", state)
        self.assertNotEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertFalse(self.package.exists())

    def test_current_network_fields_rejected_with_valid_crc(self):
        for name, offset, invalid in (("mode", -31, 3), ("setup", -30, 2), ("manual", -29, 2)):
            with self.subTest(field=name):
                state = bytearray(self.state)
                state[offset] = invalid
                struct.pack_into("<I", state, 8, zlib.crc32(state[12:]))
                put(self.source / "nvs/katmed/state", state)
                candidate = self.root / f"bad-current-{name}"
                self.assertNotEqual(self.run_tool("export", self.source, candidate).returncode, 0)
                self.assertFalse(candidate.exists())

    def test_legacy_v9_is_canonicalized_for_stage(self):
        length = struct.unpack_from("<I", self.v9_fixture)[0]
        previous_state = self.v9_fixture[4:4 + length]
        put(self.source / "nvs/katmed/state", previous_state)
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        self.assertEqual((self.package / "nvs/katmed/state").read_bytes(), previous_state)
        canonical = (self.staged / "nvs/katmed/state").read_bytes()
        self.assertEqual(canonical[4], 14)
        decoded = json.loads(subprocess.run([str(self.store_codec)], input=canonical,
                                            stdout=subprocess.PIPE, check=True).stdout)
        self.assertEqual(decoded["mesh_name_count"], 64)
        self.assertTrue(decoded["has_history"])
        self.assertEqual(decoded["language"], 0)
        self.assertEqual((self.staged / "nvs/katmed/mesh_history").read_bytes(), self.history)

    def test_legacy_v5_v7_v8_replay_uses_product_decoder(self):
        for version, fixture in self.legacy_fixtures.items():
            with self.subTest(version=version):
                length = struct.unpack_from("<I", fixture)[0]
                previous_state = fixture[4:4 + length]
                put(self.source / "nvs/katmed/state", previous_state)
                put(self.source / "nvs/katmed/mesh_history", fixture[8 + length:])
                package = self.root / f"package-v{version}"
                staged = self.root / f"staged-v{version}"
                self.assertEqual(self.run_tool("export", self.source, package).returncode, 0)
                self.assertEqual(self.run_tool("import-stage", package, staged).returncode, 0)
                self.assertEqual((package / "nvs/katmed/state").read_bytes(), previous_state)
                canonical = (staged / "nvs/katmed/state").read_bytes()
                self.assertEqual(canonical[4], 14)
                decoded = json.loads(subprocess.run([str(self.store_codec)], input=canonical,
                                                    stdout=subprocess.PIPE, check=True).stdout)
                self.assertEqual(decoded["language"], 0)
                self.assertEqual(decoded["mesh_name_count"], 9 if version == 8 else 0)
                self.assertEqual(decoded["wifi_profile_count"], 1)
                self.assertEqual(decoded["wifi_priorities"], [1])
                self.assertEqual((staged / "nvs/katmed/mesh_history").read_bytes(),
                                 fixture[8 + length:])

    def test_corrupt_mesh_history_rejected(self):
        history = bytearray(self.history)
        history[-1] ^= 1
        put(self.source / "nvs/katmed/mesh_history", history)
        self.assertNotEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertFalse(self.package.exists())

    def test_missing_mesh_history_remains_optional(self):
        (self.source / "nvs/katmed/mesh_history").unlink()
        inventory = json.loads((self.source / "source-inventory.json").read_text())
        inventory["nvs"].remove("nvs/katmed/mesh_history")
        put(self.source / "source-inventory.json", json.dumps(inventory).encode())
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        self.assertFalse((self.staged / "nvs/katmed/mesh_history").exists())

    def test_declared_invalid_mesh_history_length_rejected_at_both_boundaries(self):
        spec = importlib.util.spec_from_file_location("migration", TOOL)
        migration = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(migration)
        path = "nvs/katmed/mesh_history"
        for invalid in (b"", b"x"):
            with self.subTest(length=len(invalid), boundary="export"):
                put(self.source / path, invalid)
                candidate = self.root / f"invalid-history-{len(invalid)}-package"
                self.assertNotEqual(self.run_tool("export", self.source, candidate).returncode, 0)
                self.assertFalse(candidate.exists())

        put(self.source / path, self.history)
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        for invalid in (b"", b"x"):
            with self.subTest(length=len(invalid), boundary="import-stage"):
                put(self.package / path, invalid)
                manifest = json.loads((self.package / "manifest.json").read_text())
                manifest["files"][path] = {
                    "bytes": len(invalid), "sha256": hashlib.sha256(invalid).hexdigest(),
                }
                put(self.package / "manifest.json", json.dumps(manifest).encode())
                self.assertEqual(manifest, migration.manifest(
                    migration.inventory(self.package, package=True)))
                self.assertNotEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
                self.assertFalse(self.staged.exists())

    def test_unknown_key_rejected(self):
        put(self.source / "nvs/katmed/unknown", b"synthetic")
        self.assertNotEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertFalse(self.package.exists())

    def test_unreplayable_image_ids_rejected_without_package(self):
        for name in ("img-00000000.jpg", "img-80000000.jpg", "cat-48-0.jpg"):
            with self.subTest(name=name):
                path = self.source / "littlefs/user-images" / name
                put(path, b"\xff\xd8synthetic\xff\xd9")
                inventory = json.loads((self.source / "source-inventory.json").read_text())
                inventory["littlefs"].append("littlefs/user-images/" + name)
                inventory["littlefs"].sort()
                put(self.source / "source-inventory.json", json.dumps(inventory).encode())
                self.assertNotEqual(self.run_tool("export", self.source, self.package).returncode, 0)
                self.assertFalse(self.package.exists())
                path.unlink()
                inventory["littlefs"].remove("littlefs/user-images/" + name)
                put(self.source / "source-inventory.json", json.dumps(inventory).encode())

    def test_acme_ari_renewal_roundtrip(self):
        put(self.source / "littlefs/acme/renewal.json", b"{}")
        inventory = json.loads((self.source / "source-inventory.json").read_text())
        inventory["littlefs"] = sorted(inventory["littlefs"] +
                                      ["littlefs/acme/renewal.json"])
        put(self.source / "source-inventory.json", json.dumps(inventory).encode())
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        self.assertEqual((self.staged / "littlefs/acme/renewal.json").read_bytes(), b"{}")

    def test_acme_atomic_remainder_rejected(self):
        put(self.source / "littlefs/acme/renewal.json.katmed-1", b"synthetic")
        self.assertNotEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertFalse(self.package.exists())

    def test_acme_symlink_rejected(self):
        (self.source / "littlefs/acme/renewal.json").symlink_to("account.json")
        inventory = json.loads((self.source / "source-inventory.json").read_text())
        inventory["littlefs"] = sorted(inventory["littlefs"] +
                                      ["littlefs/acme/renewal.json"])
        put(self.source / "source-inventory.json", json.dumps(inventory).encode())
        self.assertNotEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertFalse(self.package.exists())

    def test_acme_renewal_manifest_tamper_rejected(self):
        put(self.source / "littlefs/acme/renewal.json", b"{}")
        inventory = json.loads((self.source / "source-inventory.json").read_text())
        inventory["littlefs"] = sorted(inventory["littlefs"] +
                                      ["littlefs/acme/renewal.json"])
        put(self.source / "source-inventory.json", json.dumps(inventory).encode())
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        put(self.package / "littlefs/acme/renewal.json", b"{\"changed\":true}")
        self.assertNotEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        self.assertFalse(self.staged.exists())

    def test_full_stage_rejected_without_partial_output(self):
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertNotEqual(self.run_tool("import-stage", self.package, self.staged,
                                          "--max-stage-bytes", "4096").returncode, 0)
        self.assertFalse(self.staged.exists())
        self.assertTrue(self.package.exists())

    def test_tampered_package_rejected(self):
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        put(self.package / "littlefs/user-images/img-00000001.jpg", b"other")
        self.assertNotEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        self.assertFalse(self.staged.exists())

    def test_malformed_manifest_has_generic_rejection(self):
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        put(self.package / "manifest.json", b'{"files":5}')
        result = self.run_tool("import-stage", self.package, self.staged)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stderr, b"migration package: rejected\n")
        self.assertFalse(self.staged.exists())

    def test_existing_output_aborts_without_overwrite(self):
        self.package.mkdir(mode=0o700)
        put(self.package / "sentinel", b"keep")
        self.assertNotEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertEqual((self.package / "sentinel").read_bytes(), b"keep")

    def assert_concurrent_empty_target_survives(self, action, source, output):
        spec = importlib.util.spec_from_file_location("migration", TOOL)
        migration = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(migration)
        original = migration.publish_new
        seen = []

        def occupy_before_publish(staging, destination):
            self.assertEqual(destination, output)
            self.assertTrue(staging.is_dir())
            destination.mkdir(mode=0o700)
            os.utime(destination, ns=(1_600_000_000_000_000_000,) * 2)
            before = destination.stat()
            self.assertEqual(list(destination.iterdir()), [])
            seen.append(before)
            return original(staging, destination)

        with mock.patch.object(migration, "publish_new", occupy_before_publish):
            with self.assertRaises(OSError) as error:
                if action == "export":
                    migration.export(source, output, self.codec)
                else:
                    migration.import_stage(source, output, self.codec, migration.MAX_CONTENT)
        self.assertEqual(error.exception.errno, errno.EEXIST)
        self.assertEqual(len(seen), 1)
        after = output.stat()
        self.assertEqual((after.st_dev, after.st_ino, after.st_mode, after.st_mtime_ns),
                         (seen[0].st_dev, seen[0].st_ino, seen[0].st_mode,
                          seen[0].st_mtime_ns))
        self.assertEqual(list(output.iterdir()), [])
        self.assertEqual(list(self.root.glob(".katmed-migration-*")), [])

    def test_export_concurrent_empty_target_is_not_replaced(self):
        self.assert_concurrent_empty_target_survives("export", self.source, self.package)
        self.assertEqual((self.source / "nvs/katmed/state").read_bytes(), self.state)

    def test_import_concurrent_empty_target_is_not_replaced(self):
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        original_manifest = (self.package / "manifest.json").read_bytes()
        self.assert_concurrent_empty_target_survives("import-stage", self.package, self.staged)
        self.assertEqual((self.package / "manifest.json").read_bytes(), original_manifest)

    def test_mid_export_failure_keeps_source_and_no_package(self):
        spec = importlib.util.spec_from_file_location("migration", TOOL)
        migration = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(migration)
        original = migration.write_file
        calls = 0

        def interrupted(path, data):
            nonlocal calls
            calls += 1
            if calls == 3:
                raise OSError("synthetic interruption")
            return original(path, data)

        with mock.patch.object(migration, "write_file", interrupted):
            with self.assertRaises(OSError):
                migration.export(self.source, self.package, self.codec)
        self.assertFalse(self.package.exists())
        self.assertEqual((self.source / "nvs/katmed/state").read_bytes(), self.state)

    def test_pending_reset_rejected(self):
        put(self.source / "nvs/katmed_reset/journal", b"\1\1")
        self.assertNotEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertFalse(self.package.exists())

    def test_web_auth_requires_existing_server_identity(self):
        (self.source / "nvs/katmed_web/identity").unlink()
        inventory = json.loads((self.source / "source-inventory.json").read_text())
        inventory["nvs"].remove("nvs/katmed_web/identity")
        put(self.source / "source-inventory.json", json.dumps(inventory).encode())
        self.assertNotEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertFalse(self.package.exists())

    def test_manifest_consistent_web_auth_without_identity_rejected_at_stage(self):
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        (self.package / "nvs/katmed_web/identity").unlink()
        inventory = json.loads((self.package / "source-inventory.json").read_text())
        inventory["nvs"].remove("nvs/katmed_web/identity")
        put(self.package / "source-inventory.json", json.dumps(inventory).encode())
        spec = importlib.util.spec_from_file_location("migration", TOOL)
        migration = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(migration)
        files = migration.inventory(self.package, package=True)
        put(self.package / "manifest.json", json.dumps(migration.manifest(files)).encode())
        self.assertNotEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        self.assertFalse(self.staged.exists())

    def test_server_identity_without_configured_auth_is_optional(self):
        (self.source / "nvs/katmed_web/auth").unlink()
        inventory = json.loads((self.source / "source-inventory.json").read_text())
        inventory["nvs"].remove("nvs/katmed_web/auth")
        put(self.source / "source-inventory.json", json.dumps(inventory).encode())
        self.assertEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertEqual(self.run_tool("import-stage", self.package, self.staged).returncode, 0)
        self.assertFalse((self.staged / "nvs/katmed_web/auth").exists())

    def test_insecure_source_rejected(self):
        (self.source / "nvs/katmed/state").chmod(0o644)
        self.assertNotEqual(self.run_tool("export", self.source, self.package).returncode, 0)
        self.assertFalse(self.package.exists())


if __name__ == "__main__":
    unittest.main()
