#!/usr/bin/env python3
"""Exercise offline layout gates with isolated copies, never mutate assets."""
import importlib.util
import csv
from pathlib import Path
import shutil
import struct
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("layout", REPO / "tools/check_firmware_layout.py")
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)


class FirmwareLayoutTests(unittest.TestCase):
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
        self.temp = tempfile.TemporaryDirectory(dir=REPO / "build-host")
        self.root = Path(self.temp.name)
        shutil.copytree(REPO / "firmware/boards", self.root / "firmware/boards")
        generated = self.root / "assets/generated"
        generated.mkdir(parents=True)
        for name in ("manifest.json", "c3_images.bin", "c6_images.bin"):
            shutil.copyfile(REPO / "assets/generated" / name, generated / name)
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
        (self.build / "katmed_foundation.bin").write_bytes(b"test app")

    def tearDown(self):
        self.temp.cleanup()

    def test_real_c3_partition_and_existing_pack(self):
        result = layout.check_layout(self.root, "c3", self.build)
        self.assertEqual(result["image_free"], 77864)
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

    def test_security_profile_checks_real_boundaries(self):
        self.security_fixture()
        result = layout.check_layout(self.root, "c3", self.build, True)
        self.assertTrue(result["security_profile"])
        self.assertEqual(result["app_budget"], 0x216000)
        self.assertEqual(result["nvs_available_entries"], 756)
        self.assertGreater(result["app_free"], 0)
        self.assertEqual(result["image_free"], 53288)

    def test_security_profile_rejects_old_image_boundary(self):
        self.security_fixture()
        partition = self.root / "firmware/boards/c3.security.partitions.csv"
        partition.write_text(partition.read_text().replace(
            "user_images, data, littlefs, 0x236000, 0x90000, encrypted",
            "user_images, data, littlefs, 0x280000, 0x40000, encrypted"))
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

    def test_security_profile_rejects_wrong_pack_extent(self):
        self.security_fixture()
        partition = self.root / "firmware/boards/c3.security.partitions.csv"
        partition.write_text(partition.read_text().replace("0x13A000", "0x139000"))
        self.write_partition_fixture(partition, self.build / "partition_table/partition-table.bin")
        with self.assertRaisesRegex(ValueError, "Image partition differs"):
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

    def test_modified_pack_rejected(self):
        p = self.root / "assets/generated/c3_images.bin"
        with p.open("r+b") as f:
            f.write(b"bad")
        with self.assertRaisesRegex(ValueError, "differs"):
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
        p.write_text(p.read_text().replace("0x2C0000", "0x2B0000"))
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


if __name__ == "__main__":
    (REPO / "build-host").mkdir(exist_ok=True)
    unittest.main()
