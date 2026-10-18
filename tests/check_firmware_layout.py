#!/usr/bin/env python3
"""Exercise offline layout gates with isolated copies, never mutate assets."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("layout", REPO / "tools/check_firmware_layout.py")
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)


class FirmwareLayoutTests(unittest.TestCase):
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
        self.header.write_text(
            "#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 11264\n"
            "#define CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH 3072\n"
            "#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 1\n")
        shutil.copyfile(REPO / "firmware/build-c3/partition_table/partition-table.bin",
                        self.build / "partition_table/partition-table.bin")
        (self.build / "katmed_foundation.bin").write_bytes(b"test app")

    def tearDown(self):
        self.temp.cleanup()

    def test_real_c3_partition_and_existing_pack(self):
        result = layout.check_layout(self.root, "c3", self.build)
        self.assertEqual(result["image_free"], 25804)
        self.assertEqual(result["runtime_heap"], "not_measured")

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
                               "CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y\n")
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
