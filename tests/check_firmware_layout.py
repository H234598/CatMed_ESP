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
OVERRIDE_TOOL = REPO / "tools/prepare_iot_bridge_compat.py"
C3_DIAGNOSTIC_RECIPE = REPO / "firmware/compat/esp-lvgl-port-2.9.0-c3-diagnostic.json"
FIRMWARE_BOARDS = "firmware/boards"
GENERATED_ASSETS = "assets/generated"
MAIN_SOURCE = "firmware/main/main.c"
COMPILE_DATABASE = "compile_commands.json"
PARTITION_BINARY = "partition_table/partition-table.bin"
BOOTLOADER_BINARY = "bootloader/bootloader.bin"
APP_BINARY = "katmed_foundation.bin"
USER_IMAGES_BINARY = "user_images.bin"
FACTORY_CONTRACT_ERROR = "Security partition factory differs"
CMAKE_CACHE = "CMakeCache.txt"
C3_DIAGNOSTIC_ON = "-DKATMED_C3_ALARM_DIAG=1"
C3_DIAGNOSTIC_OFF = "-DKATMED_C3_ALARM_DIAG=0"
C3_NEUTRAL_CONFIG_TABLE = "boards/c3.partitions.csv"
C3_SECURITY_TABLE = "firmware/boards/c3.security.partitions.csv"
MAINTENANCE_BINARY = "katmed_appmaint_c6.bin"
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
                               f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="{C3_NEUTRAL_CONFIG_TABLE}"\n')
        self.header.write_text(
            "#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 11264\n"
            "#define CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH 3072\n"
            "#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 1\n")
        self.write_partition_fixture(self.root / "firmware/boards/c3.partitions.csv",
                                     self.build / PARTITION_BINARY)
        (self.build / "bootloader").mkdir()
        (self.build / BOOTLOADER_BINARY).write_bytes(b"test bootloader")
        (self.build / APP_BINARY).write_bytes(self.app_image("katmed_foundation"))
        (self.build / USER_IMAGES_BINARY).write_bytes(b"\xff" * 0x180000)

    def tearDown(self):
        self.temp.cleanup()

    def test_real_c3_partition_and_existing_pack(self):
        result = layout.check_layout(self.root, "c3", self.build)
        self.assertEqual(result["image_free"], 0x180000 - 1232856)
        self.assertEqual(result["runtime_heap"], "not_measured")

    def security_fixture(self):
        self.config.write_text(self.config.read_text().replace(
            C3_NEUTRAL_CONFIG_TABLE, "boards/c3.security.partitions.csv") +
            "CONFIG_PARTITION_TABLE_OFFSET=0x10000\n"
            "CONFIG_SECURE_FLASH_ENC_ENABLED=y\n"
            "CONFIG_SECURE_FLASH_ENCRYPTION_KEY_SOURCE_EFUSES=y\n"
            "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y\n"
            "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE=y\n"
            "CONFIG_SOC_FLASH_ENCRYPTION_XTS_AES_128=y\n"
            "CONFIG_NVS_ENCRYPTION=y\n"
            "CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC=y\n")
        self.write_partition_fixture(
            self.root / C3_SECURITY_TABLE,
            self.build / PARTITION_BINARY)
        (self.build / USER_IMAGES_BINARY).write_bytes(b"\xff" * 0x1ca000)

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
            C3_NEUTRAL_CONFIG_TABLE, "boards/c6.partitions.csv"))
        self.write_partition_fixture(self.root / "firmware/boards/c6.partitions.csv",
                                     self.build / PARTITION_BINARY)
        (self.build / USER_IMAGES_BINARY).write_bytes(b"\xff" * 0x580000)
        result = layout.check_layout(self.root, "c6", self.build)
        self.assertEqual(result["app_image"], APP_BINARY)

    def test_c6_private_project_role(self):
        self.security_fixture()
        self.config.write_text(self.config.read_text().replace(
            "boards/c3.security.partitions.csv", "boards/c6.security.partitions.csv"))
        self.write_partition_fixture(self.root / "firmware/boards/c6.security.partitions.csv",
                                     self.build / PARTITION_BINARY)
        (self.build / USER_IMAGES_BINARY).write_bytes(b"\xff" * 0x5ca000)
        (self.build / MAINTENANCE_BINARY).write_bytes(self.app_image("katmed_appmaint_c6"))
        result = layout.check_layout(self.root, "c6", self.build, True, MAINTENANCE_BINARY)
        self.assertEqual(result["app_image"], MAINTENANCE_BINARY)

    def test_normal_name_rejects_maintenance_project_role(self):
        (self.build / APP_BINARY).write_bytes(self.app_image("katmed_appmaint_c6"))
        with self.assertRaisesRegex(ValueError, "project role"):
            layout.check_layout(self.root, "c3", self.build)

    def test_missing_app_descriptor_rejected(self):
        image = bytearray(self.app_image("katmed_foundation"))
        image[32:36] = b"\0" * 4
        (self.build / APP_BINARY).write_bytes(image)
        with self.assertRaisesRegex(ValueError, "descriptor"):
            layout.check_layout(self.root, "c3", self.build)

    def test_truncated_app_descriptor_rejected(self):
        (self.build / APP_BINARY).write_bytes(
            self.app_image("katmed_foundation")[:100])
        with self.assertRaisesRegex(ValueError, "descriptor"):
            layout.check_layout(self.root, "c3", self.build)

    def test_security_profile_rejects_old_image_boundary(self):
        self.security_fixture()
        partition = self.root / C3_SECURITY_TABLE
        partition.write_text(partition.read_text().replace(
            "user_images, data, littlefs, 0x236000, 0x1ca000, encrypted",
            "user_images, data, littlefs, 0x280000, 0x180000, encrypted"))
        self.write_partition_fixture(partition,
                                     self.build / PARTITION_BINARY)
        with self.assertRaisesRegex(ValueError, "Security partition user_images differs"):
            layout.check_layout(self.root, "c3", self.build, True)

    def test_security_profile_rejects_stale_factory_budget(self):
        self.security_fixture()
        partition = self.root / C3_SECURITY_TABLE
        partition.write_text(partition.read_text().replace("0x216000", "0x210000"))
        self.write_partition_fixture(partition, self.build / PARTITION_BINARY)
        with self.assertRaisesRegex(ValueError, FACTORY_CONTRACT_ERROR):
            layout.check_layout(self.root, "c3", self.build, True)

    def test_security_profile_rejects_short_filesystem_extent(self):
        self.security_fixture()
        partition = self.root / C3_SECURITY_TABLE
        partition.write_text(partition.read_text().replace("0x1ca000", "0x1c9000"))
        self.write_partition_fixture(partition, self.build / PARTITION_BINARY)
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
        partition = self.root / C3_SECURITY_TABLE
        partition.write_text(partition.read_text().replace(
            "nvs, data, nvs, 0x11000, 0x7000,",
            "nvs, data, nvs, 0x11000, 0x6000,"))
        with self.assertRaisesRegex(ValueError, "does not match"):
            layout.check_layout(self.root, "c3", self.build, True)

    def test_security_profile_rejects_old_phy_position(self):
        self.security_fixture()
        partition = self.root / C3_SECURITY_TABLE
        partition.write_text(partition.read_text().replace(
            "phy_init, data, phy, 0x1A000, 0x1000,",
            "phy_init, data, phy, 0x1B000, 0x1000,"))
        self.write_partition_fixture(partition,
                                     self.build / PARTITION_BINARY)
        with self.assertRaisesRegex(ValueError, "Security partition phy_init differs"):
            layout.check_layout(self.root, "c3", self.build, True)

    def test_modified_standard_file_rejected(self):
        p = next((self.root / "assets/generated/c3_240x240").glob("*.jpg"))
        with p.open("r+b") as f:
            f.write(b"bad")
        with self.assertRaisesRegex(ValueError, "SHA-256/length mismatch"):
            layout.check_layout(self.root, "c3", self.build)

    def test_app_overflow_rejected(self):
        with (self.build / APP_BINARY).open("wb") as f:
            f.truncate(0x2B0001)
        with self.assertRaisesRegex(ValueError, "exceeds"):
            layout.check_layout(self.root, "c3", self.build)

    def test_wrong_target_binary_rejected(self):
        self.config.write_text(self.config.read_text().replace(C3_NEUTRAL_CONFIG_TABLE,
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
                               f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="{C3_NEUTRAL_CONFIG_TABLE}"\n')
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
    """Use synthetic files and offline SDK parsers."""

    def install_guards(self):
        self.allowed_files.update({Path(__file__).resolve(),
                                   LAYOUT_TOOL,
                                   OVERRIDE_TOOL,
                                   C3_DIAGNOSTIC_RECIPE,
                                   REPO / "tools/generate_firmware_catalog.py",
                                   REPO / C3_SECURITY_TABLE,
                                   REPO / "firmware/boards/c6.security.partitions.csv"})
        super().install_guards()

    def setUp(self):
        # Load the graph checker's offline version parser before the filesystem guard.
        import idf_component_tools.semver  # noqa: F401
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
        shutil.copyfile(REPO / C3_SECURITY_TABLE,
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
        (self.build / CMAKE_CACHE).write_text("\n".join(cache) + "\n")
        self.compile_entry = {"directory": str(self.build),
                              "file": str(self.root / MAIN_SOURCE),
                              "arguments": ["synthetic-compiler", "-I" + str(self.build / "config"),
                                            "-DKATMED_FULL_STACK=1", "-DKATMED_WITH_CORE=1",
                                            C3_DIAGNOSTIC_ON,
                                            "-DKATMED_APPSTORE_MAINTENANCE=0",
                                            "-DKATMED_ACME_RESOURCE_PROBE=0",
                                            "-DKATMED_ACME_RESOURCE_CLIENT=0"]}
        (self.build / COMPILE_DATABASE).write_text(json.dumps([self.compile_entry]))
        FirmwareLayoutTests.write_partition_fixture(boards / "c3.security.partitions.csv",
                                                    self.build / PARTITION_BINARY)
        (self.build / BOOTLOADER_BINARY).write_bytes(b"synthetic bootloader")
        (self.build / USER_IMAGES_BINARY).write_bytes(b"\xff" * 0x1ca000)
        self.synthetic_catalogue()
        self.app = self.build / APP_BINARY
        self.write_sdk_image()

    def write_settings(self):
        self.config.write_text("".join(f"CONFIG_{k}={v}\n" for k, v in self.settings.items()))
        self.header.write_text("#pragma once\n\n" + "".join(f"#define CONFIG_{k} {1 if v == 'y' else v}\n"
                                       for k, v in self.settings.items()))

    def test_c3_final_override_graph_checks_compiled_profile(self):
        compat = guard_fixture.load_public("graph247", OVERRIDE_TOOL)
        target = "esp32c3"
        firmware = self.root / "firmware"
        root = self.build / "katmed-overrides"
        port = "espressif/esp_lvgl_port"
        bridge = "espressif/iot_bridge"
        recipe = json.loads(C3_DIAGNOSTIC_RECIPE.read_text())
        origin = {"version": recipe["version"], "component_hash": recipe["registry_component_hash"]}
        prepared = {**origin, "source": {"type": "local", "path": f"{root.name}/{compat.DIAGNOSTIC_PORT}"}}
        original = {"target": target, "dependencies": {port: origin}, "direct_dependencies": [port]}
        resolved = {**original, "dependencies": {port: prepared}, "direct_dependencies": [port, bridge]}
        (firmware / f"dependencies.{target}.lock").write_text(json.dumps(original))
        (firmware / "main/idf_component.yml").write_text(json.dumps({"dependencies": {port: "==2.9.0"}}))
        lock = self.build / "override-c3.lock"
        lock.write_text(json.dumps(resolved))
        manifest = compat.DIAGNOSTIC_MANIFEST
        provider = compat.DIAGNOSTIC_PORT
        bridge_component = bridge.replace("/", "__")
        graph = {
            "main": {"reqs": [manifest, provider, bridge_component]},
            manifest: {"dir": str(firmware / "compat" / manifest), "reqs": [provider]},
            provider: {"dir": str(root / provider), "reqs": ["lvgl__lvgl"]},
            "board": {"reqs": [provider]},
            "katmed_ui": {"priv_reqs": [provider]},
            "espressif__mesh_lite": {"priv_reqs": [bridge_component]},
        }
        description = self.build / "project_description.json"
        project_description = {"config_file": str(self.config), "build_component_info": graph}
        good_description = json.dumps(project_description)
        description.write_text(good_description)
        self.config.write_text(self.config.read_text() + "".join(
            f"# CONFIG_LV_USE_{name} is not set\n" for name in ("FREETYPE", "LIBJPEG_TURBO", "LIBPNG", "LZ4")))
        with (mock.patch.object(compat, "__file__", str(self.root / "tools/prepare_iot_bridge_compat.py")),
              mock.patch.dict(sys.modules, {"check_firmware_layout": self.layout})):
            compat.verify_override_graph(root, target, lock, self.build, development001=True)
            graph["katmed_ui"]["priv_reqs"].clear()
            description.write_text(json.dumps(project_description))
            with self.assertRaisesRegex(ValueError, "Development001 graph edge missing"):
                compat.verify_override_graph(root, target, lock, self.build, development001=True)
            description.write_text(good_description)
            self.header.write_text(self.header.read_text().replace(
                "#define CONFIG_SECURE_FLASH_REQUIRE_ALREADY_ENABLED 1\n", ""))
            with self.assertRaisesRegex(ValueError, "Compiled old-C3 setting differs"):
                compat.verify_override_graph(root, target, lock, self.build, development001=True)
        self.assertEqual(self.blocked, [])

    def synthetic_catalogue(self, board="c3"):
        generated = self.root / GENERATED_ASSETS
        generated.mkdir(parents=True, exist_ok=True)
        profiles = {"c3_240x240": (240, 240)} if board == "c3" else {
            "c6_172x320": (172, 320), "c6_320x172": (320, 172)}
        data = b"\xff\xd8synthetic\xff\xd9"
        sha = hashlib.sha256(data).hexdigest()
        images = []
        for index in range(72):
            source = f"synthetic-{index:02}.jpg"
            source_sha = hashlib.sha256(source.encode()).hexdigest()
            (generated / source).write_bytes(data)
            images.append({"source": source, "source_sha256": source_sha, "id": source_sha[:16],
                           "sort_index": index, "variants": {profile: {
                               "file": source, "bytes": len(data), "sha256": sha}
                               for profile in profiles}})
        logical = [(x["source"], x["source_sha256"]) for x in images]
        catalogue_sha = hashlib.sha256(json.dumps(logical, separators=(",", ":")).encode()).hexdigest()
        manifest = {"schema_version": 3, "image_count": 72, "images": images,
                    "catalogue_sha256": catalogue_sha,
                    "profiles": {p: {"width": w, "height": h} for p, (w, h) in profiles.items()}}
        (generated / "manifest.json").write_text(json.dumps(manifest))

    def write_sdk_image(self, *, board="c3", chip=5, minimum=3, maximum=199, security=0,
                        flash=0x20, payload_bytes=256):
        descriptor = bytearray(256)
        struct.pack_into("<II", descriptor, 0, 0xABCD5432, security)
        descriptor[16:48] = b"synthetic143".ljust(32, b"\0")
        descriptor[48:80] = b"katmed_foundation".ljust(32, b"\0")
        descriptor[144:176] = bytes(range(32))
        image_class = (self.images.ESP32C3FirmwareImage if board == "c3"
                       else self.images.ESP32C6FirmwareImage)
        image = image_class()
        image.chip_id = chip
        image.min_rev = minimum
        image.min_rev_full = minimum
        image.max_rev_full = maximum
        image.flash_mode = 2
        image.flash_size_freq = flash
        address = 0x3C000020 if board == "c3" else 0x42000020
        image.segments = [self.images.ImageSegment(address, bytes(descriptor).ljust(payload_bytes, b"\0"))]
        image.save(str(self.app))
        # save() writes the class's chip ID. Make the negative fixture's actual
        # header disagree, while retaining a valid checksum and digest footer.
        if chip != (5 if board == "c3" else 13):
            data = bytearray(self.app.read_bytes())
            struct.pack_into("<H", data, 12, chip)
            data[-32:] = hashlib.sha256(data[:-32]).digest()
            self.app.write_bytes(data)
        parsed = self.images.LoadFirmwareImage(f"esp32{board}", self.app.read_bytes())
        self.assertEqual(parsed.chip_id, chip)
        self.assertEqual(os.environ["ESPTOOL_CFGFILE"], str(self.cfg))

    def check(self, board="c3", security=True):
        return self.layout.check_layout(self.root, board, self.build, security,
                                        old_c3_development001=True)

    def c6_fixture(self):
        self.settings.pop("ESPTOOLPY_FLASHSIZE_4MB")
        self.settings.update({
            "IDF_TARGET": '"esp32c6"', "ESPTOOLPY_FLASHSIZE_8MB": "y",
            "ESP32C6_REV_MIN_FULL": "0", "ESP32C6_REV_MAX_FULL": "99",
            "BOOTLOADER_OFFSET_IN_FLASH": "0x0", "ESPTOOLPY_NO_STUB": "y",
            "SECURE_FLASH_CHECK_ENC_EN_IN_APP": "y",
            "PARTITION_TABLE_CUSTOM_FILENAME": '"boards/c6.security.partitions.csv"',
        })
        self.write_settings()
        cache = self.build / CMAKE_CACHE
        cache.write_text(cache.read_text().replace("KATMED_C3_ALARM_DIAG:BOOL=ON",
                                                  "KATMED_C3_ALARM_DIAG:BOOL=OFF") +
                         "KATMED_C6_DEVELOPMENT001:BOOL=ON\n")
        arguments = self.compile_entry["arguments"]
        arguments[arguments.index(C3_DIAGNOSTIC_ON)] = C3_DIAGNOSTIC_OFF
        (self.build / COMPILE_DATABASE).write_text(json.dumps([self.compile_entry]))
        self.c6_partition = self.root / FIRMWARE_BOARDS / "c6.security.partitions.csv"
        shutil.copyfile(REPO / FIRMWARE_BOARDS / self.c6_partition.name, self.c6_partition)
        self.c6_binary = self.build / PARTITION_BINARY
        FirmwareLayoutTests.write_partition_fixture(self.c6_partition, self.c6_binary)
        (self.build / USER_IMAGES_BINARY).write_bytes(b"\xff" * 0x5ca000)
        self.synthetic_catalogue("c6")
        self.flasher = self.build / "flasher_args.json"
        self.flasher.write_text(json.dumps({
            "flash_files": {"0x0": BOOTLOADER_BINARY,
                            "0x10000": PARTITION_BINARY,
                            "0x20000": APP_BINARY, "0x236000": USER_IMAGES_BINARY},
            "app": {"offset": "0x20000", "file": APP_BINARY},
        }))
        self.write_c6_image()

    def write_c6_image(self, **overrides):
        image = {"chip": 13, "minimum": 0, "maximum": 99, "flash": 0x30}
        self.write_sdk_image(board="c6", **(image | overrides))

    def check_c6(self):
        return self.layout.check_layout(self.root, "c6", self.build, True, c6_development001=True)

    def test_c6_development001_complete_and_distinct(self):
        self.c6_fixture()
        result = self.check_c6()
        self.assertTrue(result["c6_development001"])
        self.assertFalse(result["old_c3_development001"])
        self.assertEqual(result["app_budget"], 0x216000)
        self.assertEqual(result["app_sector_end"], 0x21000)
        self.assertEqual(result["app_sector_free"], 0x215000)
        self.assertEqual(result["image_budget"], 0x5ca000)
        for board, security in (("c3", True), ("c6", False)):
            with self.subTest(board=board, security=security):
                with self.assertRaisesRegex(ValueError, "requires the C6 app security profile"):
                    self.layout.check_layout(self.root, board, self.build, security, c6_development001=True)
        with self.assertRaisesRegex(ValueError, "MODE_RELEASE"):
            self.layout.check_layout(self.root, "c6", self.build, True)
        self.assertEqual(self.blocked, [])

    def test_c6_development001_rejects_compiled_profile_drift(self):
        self.c6_fixture()
        original = self.config.read_text()
        for name in ("SECURE_BOOT", "SECURE_BOOT_V2_ENABLED", "SECURE_FLASH_ENCRYPTION_MODE_RELEASE",
                     "SECURE_DISABLE_ROM_DL_MODE",
                     "SECURE_INSECURE_ALLOW_DL_MODE", "SECURE_BOOT_ALLOW_JTAG", "EFUSE_VIRTUAL"):
            with self.subTest(setting=name):
                self.config.write_text(original + f"CONFIG_{name}=y\n")
                with self.assertRaisesRegex(ValueError, name):
                    self.check_c6()
        self.config.write_text(original)
        header = self.header.read_text()
        for name in ("SECURE_FLASH_REQUIRE_ALREADY_ENABLED", "SECURE_FLASH_CHECK_ENC_EN_IN_APP",
                     "SECURE_ENABLE_SECURE_ROM_DL_MODE", "ESPTOOLPY_NO_STUB"):
            with self.subTest(setting=name):
                self.header.write_text(header.replace(f"#define CONFIG_{name} 1", ""))
                with self.assertRaisesRegex(ValueError, name):
                    self.check_c6()
        self.header.write_text(header)
        args = self.compile_entry["arguments"]
        args[args.index(C3_DIAGNOSTIC_OFF)] = C3_DIAGNOSTIC_ON
        (self.build / COMPILE_DATABASE).write_text(json.dumps([self.compile_entry]))
        with self.assertRaisesRegex(ValueError, "Compiler definition differs: KATMED_C3_ALARM_DIAG"):
            self.check_c6()

    def test_c6_development001_rejects_image_and_layout_drift(self):
        self.c6_fixture()
        for change, reason in (({"chip": 5}, "chip/revision"), ({"minimum": 1}, "chip/revision"),
                               ({"flash": 0x20}, "flash mode/size"),
                               ({"security": 1}, "security version"),
                               ({"payload_bytes": 0x216000}, "exceeds budget")):
            with self.subTest(change=change):
                self.write_c6_image(**change)
                with self.assertRaisesRegex(ValueError, reason):
                    self.check_c6()
        self.write_c6_image()
        original = self.flasher.read_text()
        self.flasher.write_text(original.replace("0x10000", "0x8000"))
        with self.assertRaisesRegex(ValueError, "generated flash offsets"):
            self.check_c6()
        args = json.loads(original)
        args["app"]["offset"] = "0x10000"
        self.flasher.write_text(json.dumps(args))
        with self.assertRaisesRegex(ValueError, "generated factory app entry"):
            self.check_c6()
        self.flasher.write_text(original)
        self.c6_partition.write_text(self.c6_partition.read_text().replace("0x216000", "0x210000"))
        FirmwareLayoutTests.write_partition_fixture(self.c6_partition, self.c6_binary)
        with self.assertRaisesRegex(ValueError, FACTORY_CONTRACT_ERROR):
            self.check_c6()

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
        cache_path = self.build / CMAKE_CACHE
        cache = cache_path.read_text()
        cache_path.write_text(cache + "KATMED_C3_ALARM_DIAG:BOOL=OFF\n")
        with self.subTest(cache_override=True):
            with self.assertRaisesRegex(ValueError, "Compiler CMake"):
                self.check()
        cache_path.write_text(cache)
        commands_path = self.build / COMPILE_DATABASE
        for override in (C3_DIAGNOSTIC_OFF, "-UKATMED_C3_ALARM_DIAG"):
            with self.subTest(compiler_override=override):
                entry = dict(self.compile_entry)
                entry["arguments"] = self.compile_entry["arguments"] + [override]
                commands_path.write_text(json.dumps([entry]))
                with self.assertRaisesRegex(ValueError, "Compiler definition"):
                    self.check()
        self.compile_entry["arguments"].remove(C3_DIAGNOSTIC_ON)
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
        partition = self.root / C3_SECURITY_TABLE
        partition.write_text(partition.read_text().replace("0x216000", "0x210000"))
        FirmwareLayoutTests.write_partition_fixture(partition,
                                                    self.build / PARTITION_BINARY)
        with self.assertRaisesRegex(ValueError, FACTORY_CONTRACT_ERROR):
            self.check()

    def test_isolation_rejects_external_io(self):
        self.assertEqual(self.cfg.resolve(), self.cfg)
        self.assertTrue(LAYOUT_TOOL.is_file())
        import socket
        # The fixture replaces process creation with a rejecting mock.
        import subprocess  # nosec B404
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
                       # Both process calls are replaced by the fixture's rejecting mock.
                       lambda: subprocess.Popen(["synthetic-command"]),  # nosec B603, B607
                       lambda: os.system("synthetic-command"),  # nosec B605, B607
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
