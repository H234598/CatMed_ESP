#!/usr/bin/env python3
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import katmed_acme_ram


PHASES = (
    "baseline", "initialization", "order", "challenge",
    "certificate", "error", "recovery", "retry",
)


def metric(uptime: int, minimum: int, largest: int, main: int, lvgl: int) -> str:
    return (
        "I (1) katmed_metrics: "
        f"uptime_ms={uptime} pack_hash_us=0 pack_valid=0 "
        f"variant_hash_us=0 variant_valid=0 jpeg_decode_us=0 jpeg_valid=0 "
        f"blink_cycle_us=0 blink_valid=0 blink_pixels=0 blink_segments=0 "
        f"blink_cycles=0 blink_changes=0 pack_status=0 variant_status=0 jpeg_status=0 "
        f"internal_min_free={minimum} "
        f"internal_largest_block={largest} main_stack_hwm_bytes={main} "
        f"lvgl_stack_hwm_bytes={lvgl} lvgl_stack_valid=1\n"
    )


def phase(sequence: int, name: str) -> str:
    return f"I (1) katmed_acme_phase: schema=1 phase_seq={sequence} phase={name}\n"


class AcmeRamBenchCheck(unittest.TestCase):
    def test_full_order_and_repeated_recovery_are_qualified(self):
        lines = []
        sequence = 1
        for repetition in range(2):
            for name in PHASES:
                lines.append(phase(sequence, name))
                lines.append(metric(
                    sequence * 1000,
                    64000 - repetition * 3000 - sequence * 100,
                    22000 - repetition * 500 - sequence * 50,
                    12000 - repetition * 100,
                    9000 - repetition * 75,
                ))
                sequence += 1
        summary = katmed_acme_ram.evaluate("".join(lines).encode("ascii"))
        self.assertEqual("qualified_host_synthetic", summary["qualification"])
        self.assertEqual(list(PHASES), summary["required_phases"])
        self.assertEqual(2, summary["repetitions"])
        self.assertEqual(64000 - 3000 - 1600, summary["internal_min_free_min"])
        self.assertEqual(22000 - 500 - 800, summary["internal_largest_block_min"])
        self.assertEqual(11900, summary["main_stack_hwm_min"])
        self.assertEqual(8925, summary["lvgl_stack_hwm_min"])

    def test_missing_phase_and_malformed_metric_fail_closed(self):
        text = phase(1, "baseline") + metric(1000, 1, 1, 1, 1)
        with self.assertRaises(katmed_acme_ram.RamBenchError):
            katmed_acme_ram.evaluate(text.encode("ascii"))
        malformed = phase(1, "baseline") + "I (1) katmed_metrics: uptime_ms=x\n"
        with self.assertRaises(katmed_acme_ram.RamBenchError):
            katmed_acme_ram.evaluate(malformed.encode("ascii"))

    def test_cli_writes_only_bounded_summary(self):
        text = phase(1, "baseline") + metric(1000, 100, 80, 60, 40)
        text += phase(2, "initialization") + metric(2000, 90, 70, 50, 30)
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / "fixture.log"
            output = pathlib.Path(temporary) / "summary.json"
            source.write_text(text)
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/katmed_acme_ram.py"),
                 "--input", str(source), "--output", str(output)],
                text=True, capture_output=True, check=False,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
