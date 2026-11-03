#!/usr/bin/env python3
"""Exercise offline layout gates with isolated copies, never mutate assets."""
import importlib.util
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
LAYOUT_TOOL = REPO / "tools/check_firmware_layout.py"
FIRMWARE_BOARDS = "firmware/boards"
GENERATED_ASSETS = "assets/generated"
MAIN_SOURCE = "firmware/main/main.c"
COMPILE_DATABASE = "compile_commands.json"
CONFIG_INCLUDE_ERROR = "Compiler generated sdkconfig.h"
spec = importlib.util.spec_from_file_location(
    "layout_guard_fixture", REPO / "tests/layout_test_fixture.py")
guard_fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard_fixture)


class FirmwareLayoutTests(unittest.TestCase):
    @staticmethod
    def app_image(project_name: str) -> bytes:
        """Synthetic IDF image header and first DROM esp_app_desc_t."""
        name = project_name.encode("ascii")
        if len(name) >= 32:
            raise ValueError("Synthetic project name is too long")
        header = bytearray(24)
        header[0:2] = b"\xe9\x01"
        descriptor = bytearray(256)
        struct.pack_into("<I", descriptor, 0, 0xABCD5432)
        descriptor[48:80] = name.ljust(32, b"\0")
        return bytes(header) + struct.pack("<II", 0x3C000000, len(descriptor)) + descriptor

    @staticmethod
    def write_partition_fixture(csv_path: Path, binary_path: Path) -> None:
        """Build IDF-format entries from the checked-in CSV, without a firmware build."""
        subtypes = {"nvs": 2, "nvs_keys": 4, "phy": 1,
                    "factory": 0, "littlefs": 0x83}
        entries = bytearray()
        with csv_path.open() as source:
            for row in csv.reader(line for line in source if line.strip() and
                                  not line.startswith("#")):
                name, kind, subtype, offset, size, flags = (part.strip() for part in row)
                subtype_id = subtypes[subtype] if subtype in subtypes else int(subtype, 0)
                entries.extend(struct.pack("<2sBBII16sI", b"\xaa\x50",
                                           {"app": 0, "data": 1}[kind], subtype_id,
                                           int(offset, 0), int(size, 0),
                                           name.encode().ljust(16, b"\0"),
                                           {"": 0, "encrypted": 1, "readonly": 2}[flags]))
        binary_path.write_bytes(entries)

    def setUp(self):
        global layout
        layout = guard_fixture.load_public("layout", LAYOUT_TOOL)
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        shutil.copytree(REPO / FIRMWARE_BOARDS, self.root / FIRMWARE_BOARDS)
        generated = self.root / GENERATED_ASSETS
        generated.parent.mkdir(parents=True)
        shutil.copytree(REPO / GENERATED_ASSETS, generated)
        self.build = self.root / "build"
        (self.build / "partition_table").mkdir(parents=True)
        (self.build / "config").mkdir()
        self.config = self.build / "sdkconfig"
        self.header = self.build / "config/sdkconfig.h"
        self.config.write_text(
            "CONFIG_ESP_MAIN_TASK_STACK_SIZE=11264\n"
            "CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=3072\n"
            "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y\n")
        self.config.write_text(self.config.read_text() +
                               'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="boards/c3.partitions.csv"\n')
        self.header.write_text(
            "#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 11264\n"
            "#define CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH 3072\n"
            "#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 1\n")
        self.write_partition_fixture(self.root / "firmware/boards/c3.partitions.csv",
                                     self.build / "partition_table/partition-table.bin")
        (self.build / "bootloader").mkdir()
        (self.build / "bootloader/bootloader.bin").write_bytes(b"test bootloader")
        (self.build / "katmed_foundation.bin").write_bytes(self.app_image("katmed_foundation"))
        (self.build / "user_images.bin").write_bytes(b"\xff" * 0x180000)

    def tearDown(self):
        self.temp.cleanup()

    def test_real_c3_partition_and_existing_pack(self):
        result = layout.check_layout(self.root, "c3", self.build)
        self.assertEqual(result["image_free"], 0x180000 - 1232856)
        self.assertEqual(result["runtime_heap"], "not_measured")

    def security_fixture(self):
        self.config.write_text(self.config.read_text().replace(
            "boards/c3.partitions.csv", "boards/c3.security.partitions.csv") +
            "CONFIG_PARTITION_TABLE_OFFSET=0x10000\n"
            "CONFIG_SECURE_FLASH_ENC_ENABLED=y\n"
            "CONFIG_SECURE_FLASH_ENCRYPTION_KEY_SOURCE_EFUSES=y\n"
            "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y\n"
            "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE=y\n"
            "CONFIG_SOC_FLASH_ENCRYPTION_XTS_AES_128=y\n"
            "CONFIG_NVS_ENCRYPTION=y\n"
            "CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC=y\n")
        self.write_partition_fixture(
            self.root / "firmware/boards/c3.security.partitions.csv",
            self.build / "partition_table/partition-table.bin")
        (self.build / "user_images.bin").write_bytes(b"\xff" * 0x1ca000)

    def test_security_profile_checks_real_boundaries(self):
        self.security_fixture()
        result = layout.check_layout(self.root, "c3", self.build, True)
        self.assertTrue(result["security_profile"])
        self.assertEqual(result["app_budget"], 0x216000)
        self.assertEqual(result["nvs_available_entries"], 756)
        self.assertGreater(result["app_free"], 0)
        self.assertEqual(result["image_free"], 0x1ca000 - 1232856)

    def test_c6_normal_project_role(self):
        self.config.write_text(self.config.read_text().replace(
            "boards/c3.partitions.csv", "boards/c6.partitions.csv"))
        self.write_partition_fixture(self.root / "firmware/boards/c6.partitions.csv",
                                     self.build / "partition_table/partition-table.bin")
        (self.build / "user_images.bin").write_bytes(b"\xff" * 0x580000)
        result = layout.check_layout(self.root, "c6", self.build)
        self.assertEqual(result["app_image"], "katmed_foundation.bin")

    def test_c6_private_project_role(self):
        self.security_fixture()
        self.config.write_text(self.config.read_text().replace(
            "boards/c3.security.partitions.csv", "boards/c6.security.partitions.csv"))
        self.write_partition_fixture(self.root / "firmware/boards/c6.security.partitions.csv",
                                     self.build / "partition_table/partition-table.bin")
        (self.build / "user_images.bin").write_bytes(b"\xff" * 0x5ca000)
        (self.build / "katmed_appmaint_c6.bin").write_bytes(self.app_image("katmed_appmaint_c6"))
        result = layout.check_layout(self.root, "c6", self.build, True, "katmed_appmaint_c6.bin")
        self.assertEqual(result["app_image"], "katmed_appmaint_c6.bin")

    def test_normal_name_rejects_maintenance_project_role(self):
        (self.build / "katmed_foundation.bin").write_bytes(self.app_image("katmed_appmaint_c6"))
        with self.assertRaisesRegex(ValueError, "project role"):
            layout.check_layout(self.root, "c3", self.build)

    def test_missing_app_descriptor_rejected(self):
        image = bytearray(self.app_image("katmed_foundation"))
        image[32:36] = b"\0" * 4
        (self.build / "katmed_foundation.bin").write_bytes(image)
        with self.assertRaisesRegex(ValueError, "descriptor"):
            layout.check_layout(self.root, "c3", self.build)

    def test_truncated_app_descriptor_rejected(self):
        (self.build / "katmed_foundation.bin").write_bytes(
            self.app_image("katmed_foundation")[:100])
        with self.assertRaisesRegex(ValueError, "descriptor"):
            layout.check_layout(self.root, "c3", self.build)

    def test_security_profile_rejects_old_image_boundary(self):
        self.security_fixture()
        partition = self.root / "firmware/boards/c3.security.partitions.csv"
        partition.write_text(partition.read_text().replace(
            "user_images, data, littlefs, 0x236000, 0x1ca000, encrypted",
            "user_images, data, littlefs, 0x280000, 0x180000, encrypted"))
        self.write_partition_fixture(partition,
                                     self.build / "partition_table/partition-table.bin")
        with self.assertRaisesRegex(ValueError, "Security partition user_images differs"):
            layout.check_layout(self.root, "c3", self.build, True)

    def test_security_profile_rejects_stale_factory_budget(self):
        self.security_fixture()
        partition = self.root / "firmware/boards/c3.security.partitions.csv"
        partition.write_text(partition.read_text().replace("0x216000", "0x210000"))
        self.write_partition_fixture(partition, self.build / "partition_table/partition-table.bin")
        with self.assertRaisesRegex(ValueError, "Security partition factory differs"):
            layout.check_layout(self.root, "c3", self.build, True)

    def test_security_profile_rejects_short_filesystem_extent(self):
        self.security_fixture()
        partition = self.root / "firmware/boards/c3.security.partitions.csv"
        partition.write_text(partition.read_text().replace("0x1ca000", "0x1c9000"))
        self.write_partition_fixture(partition, self.build / "partition_table/partition-table.bin")
        with self.assertRaisesRegex(ValueError, "Security partition user_images differs"):
            layout.check_layout(self.root, "c3", self.build, True)

    def test_security_profile_rejects_missing_release_mode(self):
        self.security_fixture()
        self.config.write_text(self.config.read_text().replace(
            "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y\n", ""))
        with self.assertRaisesRegex(ValueError, "missing resolved setting"):
            layout.check_layout(self.root, "c3", self.build, True)

    def test_security_profile_rejects_changed_nvs_size(self):
        self.security_fixture()
        partition = self.root / "firmware/boards/c3.security.partitions.csv"
        partition.write_text(partition.read_text().replace(
            "nvs, data, nvs, 0x11000, 0x7000,",
            "nvs, data, nvs, 0x11000, 0x6000,"))
        with self.assertRaisesRegex(ValueError, "does not match"):
            layout.check_layout(self.root, "c3", self.build, True)

    def test_security_profile_rejects_old_phy_position(self):
        self.security_fixture()
        partition = self.root / "firmware/boards/c3.security.partitions.csv"
        partition.write_text(partition.read_text().replace(
            "phy_init, data, phy, 0x1A000, 0x1000,",
            "phy_init, data, phy, 0x1B000, 0x1000,"))
        self.write_partition_fixture(partition,
                                     self.build / "partition_table/partition-table.bin")
        with self.assertRaisesRegex(ValueError, "Security partition phy_init differs"):
            layout.check_layout(self.root, "c3", self.build, True)

    def test_modified_standard_file_rejected(self):
        p = next((self.root / "assets/generated/c3_240x240").glob("*.jpg"))
        with p.open("r+b") as f:
            f.write(b"bad")
        with self.assertRaisesRegex(ValueError, "SHA-256/length mismatch"):
            layout.check_layout(self.root, "c3", self.build)

    def test_app_overflow_rejected(self):
        with (self.build / "katmed_foundation.bin").open("wb") as f:
            f.truncate(0x2B0001)
        with self.assertRaisesRegex(ValueError, "exceeds"):
            layout.check_layout(self.root, "c3", self.build)

    def test_wrong_target_binary_rejected(self):
        self.config.write_text(self.config.read_text().replace("boards/c3.partitions.csv",
                                                        "boards/c6.partitions.csv"))
        with self.assertRaisesRegex(ValueError, "does not match"):
            layout.check_layout(self.root, "c6", self.build)

    def test_overlap_rejected(self):
        p = self.root / "firmware/boards/c3.partitions.csv"
        p.write_text(p.read_text().replace("0x280000", "0x270000"))
        with self.assertRaisesRegex(ValueError, "Overlapping"):
            layout.check_layout(self.root, "c3", self.build)

    def test_cached_output_only_console_rejected(self):
        self.config.write_text("CONFIG_ESP_MAIN_TASK_STACK_SIZE=11264\n"
                               "CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=3072\n"
                               "CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y\n"
                               'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="boards/c3.partitions.csv"\n')
        with self.assertRaisesRegex(ValueError, "primary USB Serial/JTAG input"):
            layout.check_layout(self.root, "c3", self.build)

    def test_stale_compiled_console_rejected(self):
        self.header.write_text("#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 11264\n"
                               "#define CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH 3072\n"
                               "#define CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG 1\n")
        with self.assertRaisesRegex(ValueError, "Compiled firmware lacks"):
            layout.check_layout(self.root, "c3", self.build)


    def test_previous_main_stack_rejected(self):
        self.config.write_text(self.config.read_text().replace("11264", "8192"))
        with self.assertRaisesRegex(ValueError, "stack differs"):
            layout.check_layout(self.root, "c3", self.build)

    def test_previous_timer_stack_rejected(self):
        self.config.write_text(self.config.read_text().replace("DEPTH=3072", "DEPTH=2048"))
        with self.assertRaisesRegex(ValueError, "timer-task stack differs"):
            layout.check_layout(self.root, "c3", self.build)

    def test_previous_compiled_timer_stack_rejected(self):
        self.header.write_text(self.header.read_text().replace("DEPTH 3072", "DEPTH 2048"))
        with self.assertRaisesRegex(ValueError, "timer-task stack differs"):
            layout.check_layout(self.root, "c3", self.build)

    def test_previous_compiled_main_stack_rejected(self):
        self.header.write_text(self.header.read_text().replace("11264", "8192"))
        with self.assertRaisesRegex(ValueError, "stack differs"):
            layout.check_layout(self.root, "c3", self.build)


class OldC3Development001NoIO(guard_fixture.BuildIdContractNoIO):
    """Only synthetic files and the pure SDK image parser may be used here."""

    def install_guards(self):
        self.allowed_files.update({Path(__file__).resolve(),
                                   LAYOUT_TOOL,
                                   REPO / "tools/generate_firmware_catalog.py",
                                   REPO / "firmware/boards/c3.security.partitions.csv"})
        super().install_guards()

    def setUp(self):
        super().setUp()
        self.stack.enter_context(mock.patch.dict(os.environ, {"LC_ALL": "C", "LANGUAGE": "C"}))
        # Retain all filesystem/process/network guards. Only the import-name
        # ban is relaxed for the explicitly approved, pure SDK image parser.
        finder = next(f for f in sys.meta_path if type(f).__name__ == "BlockLiveImports")
        self.stack.enter_context(mock.patch.object(finder, "find_spec", return_value=None))
        import serial
        self.stack.enter_context(mock.patch.object(serial, "Serial", side_effect=self.forbidden))
        self.stack.enter_context(mock.patch.object(serial, "serial_for_url", side_effect=self.forbidden))
        from esptool import bin_image
        from esptool.loader import ESPLoader
        self.stack.enter_context(mock.patch.object(ESPLoader, "connect", side_effect=self.forbidden))
        self.images = bin_image
        self.layout = guard_fixture.load_public("layout143", LAYOUT_TOOL)
        self.root = self.work / "project"
        self.allowed_files.add(self.root / MAIN_SOURCE)
        boards = self.root / FIRMWARE_BOARDS
        boards.mkdir(parents=True)
        shutil.copyfile(REPO / "firmware/boards/c3.security.partitions.csv",
                        boards / "c3.security.partitions.csv")
        (self.root / "firmware/main").mkdir()
        (self.root / MAIN_SOURCE).write_text("/* synthetic */\n")
        self.build = self.root / "build"
        for name in ("config", "partition_table", "bootloader"):
            (self.build / name).mkdir(parents=True, exist_ok=True)
        self.config = self.build / "sdkconfig"
        self.header = self.build / "config/sdkconfig.h"
        self.settings = {
            "ESP_MAIN_TASK_STACK_SIZE": "11264", "FREERTOS_TIMER_TASK_STACK_DEPTH": "3072",
            "ESP_CONSOLE_USB_SERIAL_JTAG": "y", "PARTITION_TABLE_OFFSET": "0x10000",
            "IDF_TARGET": '"esp32c3"', "ESPTOOLPY_FLASHSIZE_4MB": "y",
            "ESPTOOLPY_FLASHMODE_DIO": "y", "SECURE_FLASH_ENC_ENABLED": "y",
            "SECURE_FLASH_ENCRYPTION_KEY_SOURCE_EFUSES": "y",
            "SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT": "y",
            "SECURE_FLASH_REQUIRE_ALREADY_ENABLED": "y", "SECURE_ENABLE_SECURE_ROM_DL_MODE": "y",
            "SOC_FLASH_ENCRYPTION_XTS_AES_128": "y", "NVS_ENCRYPTION": "y",
            "NVS_SEC_KEY_PROTECT_USING_FLASH_ENC": "y", "APP_RETRIEVE_LEN_ELF_SHA": "24",
            "PARTITION_TABLE_CUSTOM_FILENAME": '"boards/c3.security.partitions.csv"',
        }
        self.write_settings()
        cache = ["KATMED_FULL_STACK:BOOL=ON", "KATMED_WITH_CORE:BOOL=ON",
                 "KATMED_C3_ALARM_DIAG:BOOL=ON", "KATMED_APPSTORE_MAINTENANCE:BOOL=OFF",
                 "KATMED_ACME_RESOURCE_PROBE:BOOL=OFF", "KATMED_ACME_RESOURCE_CLIENT:BOOL=OFF"]
        (self.build / "CMakeCache.txt").write_text("\n".join(cache) + "\n")
        self.compile_entry = {"directory": str(self.build),
                              "file": str(self.root / MAIN_SOURCE),
                              "arguments": ["synthetic-compiler", "-I" + str(self.build / "config"),
                                            "-DKATMED_FULL_STACK=1", "-DKATMED_WITH_CORE=1",
                                            "-DKATMED_C3_ALARM_DIAG=1",
                                            "-DKATMED_APPSTORE_MAINTENANCE=0",
                                            "-DKATMED_ACME_RESOURCE_PROBE=0",
                                            "-DKATMED_ACME_RESOURCE_CLIENT=0"]}
        (self.build / COMPILE_DATABASE).write_text(json.dumps([self.compile_entry]))
        FirmwareLayoutTests.write_partition_fixture(boards / "c3.security.partitions.csv",
                                                    self.build / "partition_table/partition-table.bin")
        (self.build / "bootloader/bootloader.bin").write_bytes(b"synthetic bootloader")
        (self.build / "user_images.bin").write_bytes(b"\xff" * 0x1ca000)
        self.synthetic_catalogue()
        self.app = self.build / "katmed_foundation.bin"
        self.write_sdk_image()

    def write_settings(self):
        self.config.write_text("".join(f"CONFIG_{k}={v}\n" for k, v in self.settings.items()))
        self.header.write_text("#pragma once\n\n" + "".join(f"#define CONFIG_{k} {1 if v == 'y' else v}\n"
                                       for k, v in self.settings.items()))

    def synthetic_catalogue(self):
        generated = self.root / GENERATED_ASSETS
        generated.mkdir(parents=True)
        data = b"\xff\xd8synthetic\xff\xd9"
        sha = hashlib.sha256(data).hexdigest()
        images = []
        for index in range(72):
            source = f"synthetic-{index:02}.jpg"
            source_sha = hashlib.sha256(source.encode()).hexdigest()
            (generated / source).write_bytes(data)
            images.append({"source": source, "source_sha256": source_sha, "id": source_sha[:16],
                           "sort_index": index, "variants": {"c3_240x240": {
                               "file": source, "bytes": len(data), "sha256": sha}}})
        logical = [(x["source"], x["source_sha256"]) for x in images]
        catalogue_sha = hashlib.sha256(json.dumps(logical, separators=(",", ":")).encode()).hexdigest()
        manifest = {"schema_version": 3, "image_count": 72, "images": images,
                    "catalogue_sha256": catalogue_sha,
                    "profiles": {"c3_240x240": {"width": 240, "height": 240}}}
        (generated / "manifest.json").write_text(json.dumps(manifest))

    def write_sdk_image(self, *, chip=5, minimum=3, maximum=199, security=0, flash=0x20):
        descriptor = bytearray(256)
        struct.pack_into("<II", descriptor, 0, 0xABCD5432, security)
        descriptor[16:48] = b"synthetic143".ljust(32, b"\0")
        descriptor[48:80] = b"katmed_foundation".ljust(32, b"\0")
        descriptor[144:176] = bytes(range(32))
        image = self.images.ESP32C3FirmwareImage()
        image.chip_id = chip
        image.min_rev = minimum
        image.min_rev_full = minimum
        image.max_rev_full = maximum
        image.flash_mode = 2
        image.flash_size_freq = flash
        image.segments = [self.images.ImageSegment(0x3C000020, bytes(descriptor))]
        image.save(str(self.app))
        # save() writes the class's chip ID. Make the negative fixture's actual
        # header disagree, while retaining a valid checksum and digest footer.
        if chip != 5:
            data = bytearray(self.app.read_bytes())
            struct.pack_into("<H", data, 12, chip)
            data[-32:] = hashlib.sha256(data[:-32]).digest()
            self.app.write_bytes(data)
        parsed = self.images.LoadFirmwareImage("esp32c3", self.app.read_bytes())
        self.assertEqual(parsed.chip_id, chip)
        self.assertEqual(os.environ["ESPTOOL_CFGFILE"], str(self.cfg))

    def check(self, board="c3", security=True):
        return self.layout.check_layout(self.root, board, self.build, security,
                                        old_c3_development001=True)

    def test_development001_complete_contract(self):
        parsed = self.images.LoadFirmwareImage("esp32c3", self.app.read_bytes())
        self.assertEqual(parsed.chip_id, 5)
        self.assertEqual(parsed.calculate_checksum(), parsed.checksum)
        self.assertEqual(parsed.stored_digest, parsed.calc_digest)
        result = self.check()
        self.assertTrue(result["old_c3_development001"])
        self.assertEqual(result["app_budget"], 0x216000)
        self.assertEqual(result["nvs_available_entries"], 756)
        self.assertEqual(result["runtime_heap"], "not_measured")
        self.assertEqual(self.blocked, [])

    def test_development001_generated_profile_include_preserves_config_binding(self):
        generated = self.build / "generated"
        generated.mkdir()
        arguments = self.compile_entry["arguments"]
        arguments.insert(1, "-I" + str(generated))
        commands = self.build / COMPILE_DATABASE
        commands.write_text(json.dumps([self.compile_entry]))
        self.assertTrue(self.check()["old_c3_development001"])
        shadow = generated / "sdkconfig.h"
        shadow.write_text(self.header.read_text())
        with self.assertRaisesRegex(ValueError, CONFIG_INCLUDE_ERROR):
            self.check()
        shadow.unlink()
        arguments[1] = "-I" + str(self.build / "unrelated")
        commands.write_text(json.dumps([self.compile_entry]))
        with self.assertRaisesRegex(ValueError, CONFIG_INCLUDE_ERROR):
            self.check()
        arguments[1:] = ["-I" + str(generated)]
        with self.assertRaisesRegex(ValueError, CONFIG_INCLUDE_ERROR):
            self.layout.require_compiler_config_include(arguments, self.build, self.build)
        self.assertEqual(self.blocked, [])

    def test_development001_config_header_and_compiler_guards(self):
        for name in ("APP_RETRIEVE_LEN_ELF_SHA", "SECURE_FLASH_REQUIRE_ALREADY_ENABLED"):
            with self.subTest(setting=name):
                original = self.header.read_text()
                self.header.write_text(original.replace(f"#define CONFIG_{name} ", "#define STALE_"))
                with self.assertRaisesRegex(ValueError, "Compiled old-C3"):
                    self.check()
                self.header.write_text(original)
        original = self.config.read_text()
        for bad in (original.replace("CONFIG_APP_RETRIEVE_LEN_ELF_SHA=24", "CONFIG_APP_RETRIEVE_LEN_ELF_SHA=9"),
                    original + "CONFIG_SECURE_BOOT=y\n",
                    original + "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y\n"):
            with self.subTest(config=bad != original):
                self.config.write_text(bad)
                with self.assertRaises(ValueError):
                    self.check()
        self.config.write_text(original)
        for suffix in ("#undef CONFIG_APP_RETRIEVE_LEN_ELF_SHA\n",
                       "#define CONFIG_APP_RETRIEVE_LEN_ELF_SHA 9\n"):
            with self.subTest(header_override=suffix):
                header = self.header.read_text()
                self.header.write_text(header + suffix)
                with self.assertRaisesRegex(ValueError, "Compiled old-C3"):
                    self.check()
                self.header.write_text(header)
        for suffix in ("CONFIG_APP_RETRIEVE_LEN_ELF_SHA=9\n",
                       "CONFIG_SECURE_FLASH_REQUIRE_ALREADY_ENABLED=n\n"):
            with self.subTest(config_override=suffix):
                self.config.write_text(original + suffix)
                with self.assertRaisesRegex(ValueError, "Resolved old-C3"):
                    self.check()
        self.config.write_text(original)
        cache_path = self.build / "CMakeCache.txt"
        cache = cache_path.read_text()
        cache_path.write_text(cache + "KATMED_C3_ALARM_DIAG:BOOL=OFF\n")
        with self.subTest(cache_override=True):
            with self.assertRaisesRegex(ValueError, "Compiler CMake"):
                self.check()
        cache_path.write_text(cache)
        commands_path = self.build / COMPILE_DATABASE
        for override in ("-DKATMED_C3_ALARM_DIAG=0", "-UKATMED_C3_ALARM_DIAG"):
            with self.subTest(compiler_override=override):
                entry = dict(self.compile_entry)
                entry["arguments"] = self.compile_entry["arguments"] + [override]
                commands_path.write_text(json.dumps([entry]))
                with self.assertRaisesRegex(ValueError, "Compiler definition"):
                    self.check()
        self.compile_entry["arguments"].remove("-DKATMED_C3_ALARM_DIAG=1")
        commands_path.write_text(json.dumps([self.compile_entry]))
        with self.assertRaisesRegex(ValueError, "Compiler"):
            self.check()

    def test_development001_actual_parser_rejects_wrong_image(self):
        for options in ({"chip": 13}, {"minimum": 2}, {"maximum": 200},
                        {"security": 1}, {"flash": 0x30}):
            with self.subTest(options=options):
                self.write_sdk_image(**options)
                with self.assertRaises(ValueError):
                    self.check()
        self.write_sdk_image()
        original = self.app.read_bytes()
        parsed = self.images.LoadFirmwareImage("esp32c3", original)
        for position in (parsed.data_length - 1, len(original) - 1):
            with self.subTest(corruption=position):
                damaged = bytearray(original)
                damaged[position] ^= 1
                self.app.write_bytes(damaged)
                with self.assertRaisesRegex(ValueError, "checksum|footer"):
                    self.check()
        self.app.write_bytes(original[:-1])
        with self.assertRaises(ValueError):
            self.check()

    def test_development001_target_profile_and_geometry_guards(self):
        for action in (lambda: os.stat("/dev/ttyS-no-io"),
                       lambda: os.scandir(REPO.parent)):
            with self.assertRaisesRegex(AssertionError, "NO_IO"):
                action()
        for board, security in (("c6", True), ("c3", False)):
            with self.subTest(board=board, security=security):
                with self.assertRaisesRegex(ValueError, "old-C3"):
                    self.check(board, security)
        partition = self.root / "firmware/boards/c3.security.partitions.csv"
        partition.write_text(partition.read_text().replace("0x216000", "0x210000"))
        FirmwareLayoutTests.write_partition_fixture(partition,
                                                    self.build / "partition_table/partition-table.bin")
        with self.assertRaisesRegex(ValueError, "Security partition factory differs"):
            self.check()

    def test_isolation_rejects_external_io(self):
        self.assertEqual(self.cfg.resolve(), self.cfg)
        self.assertTrue(LAYOUT_TOOL.is_file())
        import socket
        import subprocess
        import serial
        from esptool.loader import ESPLoader
        for action in (lambda: open(REPO / "CMakeLists.txt"),
                       lambda: open(LAYOUT_TOOL, "w"),
                       lambda: os.open(LAYOUT_TOOL, os.O_WRONLY),
                       lambda: os.remove(LAYOUT_TOOL),
                       lambda: os.utime(LAYOUT_TOOL),
                       lambda: os.lstat(REPO.parent),
                       lambda: os.readlink(REPO.parent / "synthetic-link"),
                       lambda: os.readlink(self.work / "outside-link"),
                       lambda: (self.work / "outside-link" / "data").resolve(),
                       lambda: (self.work / "inside-link").resolve(),
                       lambda: open(self.work / "outside-link" / ".." / "data"),
                       lambda: socket.getaddrinfo("synthetic.invalid", 80),
                       lambda: socket.socket(),
                       lambda: socket.create_connection(("127.0.0.1", 1)),
                       lambda: subprocess.Popen(["synthetic-command"]),
                       lambda: os.system("synthetic-command"),
                       lambda: serial.Serial("synthetic-port"),
                       lambda: serial.serial_for_url("synthetic-port"),
                       lambda: ESPLoader.connect(None)):
            with self.subTest(action=action):
                with self.assertRaisesRegex(AssertionError, "NO_IO"):
                    action()

    def test_release_profile_still_passes(self):
        self.settings.pop("SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT")
        self.settings["SECURE_FLASH_ENCRYPTION_MODE_RELEASE"] = "y"
        self.write_settings()
        result = self.layout.check_layout(self.root, "c3", self.build, True)
        self.assertEqual(result["app_budget"], 0x216000)
        self.assertTrue(result["security_profile"])

    def test_release_profile_still_rejects_development(self):
        with self.assertRaisesRegex(ValueError, "missing resolved setting"):
            self.layout.check_layout(self.root, "c3", self.build, True)


if __name__ == "__main__":
    unittest.main()
