#!/usr/bin/env python3
import json
import pathlib
import re
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


def resource_record(cycle: int, seq: int, name: str, *, variant: str = "client",
                    status: str = "running", free: int = 64000) -> str:
    return (
        f"I (1) katmed_acme_resource: schema=2 variant={variant} cycle={cycle} "
        f"seq={seq} phase={name} status={status} uptime_ms={cycle * 10000 + seq} "
        f"internal_free={free} internal_min={free - 1000} "
        f"internal_largest={free - 2000} dma_free={free - 10000} "
        f"dma_min={free - 11000} dma_largest={free - 12000} monitor_valid=1\n"
    )


def resource_capture(variant: str = "client") -> bytes:
    lines = []
    for cycle in range(1, 4):
        names = (["idle", "key", "connection", "challenge", "issuance",
                  "certificate_switch", "cleanup"] if cycle == 1 else
                 ["idle", "renewal", "connection", "challenge", "issuance",
                  "certificate_switch", "cleanup"])
        if variant == "baseline":
            names = ["idle", "cleanup"]
        for seq, name in enumerate(names, 1):
            lines.append(resource_record(
                cycle, seq, name, variant=variant,
                status="ok" if name == "cleanup" else "running",
                free=64000 if name in ("idle", "cleanup") else 60000,
            ))
    return "".join(lines).encode("ascii")


class AcmeResourceCaptureCheck(unittest.TestCase):
    def test_compiled_producer_field_order_matches_file_consumer(self):
        source = (ROOT / "firmware/components/katmed_acme/katmed_acme.cpp").read_text()
        start = source.index('"schema=2 variant=%s')
        end = source.index('monitor_valid=%u"', start) + len('monitor_valid=%u"')
        record_format = "".join(re.findall(r'"([^"]*)"', source[start:end]))
        self.assertEqual(
            ["schema", "variant", "cycle", "seq", "phase", "status", "uptime_ms",
             "internal_free", "internal_min", "internal_largest", "dma_free",
             "dma_min", "dma_largest", "monitor_valid"],
            re.findall(r"([a-z_]+)=", record_format),
        )

    def test_three_synthetic_cycles_are_only_source_qualified(self):
        summary = katmed_acme_ram.evaluate_resource(resource_capture(), "client")
        self.assertEqual("source_capture_valid", summary["qualification"])
        self.assertFalse(summary["hardware_pass"])
        self.assertEqual(3, summary["cycles"])
        self.assertEqual([], summary["missing_phases"])
        baseline = katmed_acme_ram.evaluate_resource(resource_capture("baseline"), "baseline")
        self.assertEqual("source_capture_valid", baseline["qualification"])

    def test_missing_phase_is_reported_and_cli_cannot_pass(self):
        raw = resource_capture().replace(
            resource_record(2, 4, "challenge", free=60000).encode("ascii"), b""
        )
        # A capture with a missing numeric sequence is rejected before qualification.
        with self.assertRaises(katmed_acme_ram.RamBenchError):
            katmed_acme_ram.evaluate_resource(raw, "client")
        text = resource_capture().decode("ascii")
        text = text.replace("phase=challenge", "phase=connection", 1)
        summary = katmed_acme_ram.evaluate_resource(text.encode("ascii"), "client")
        self.assertEqual("incomplete", summary["qualification"])
        self.assertIn("challenge", summary["missing_by_cycle"]["1"])
        with tempfile.TemporaryDirectory() as temporary:
            source = pathlib.Path(temporary) / "capture.log"
            output = pathlib.Path(temporary) / "summary.json"
            source.write_text(text)
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/katmed_acme_ram.py"),
                 "--mode", "resource", "--variant", "client", "--input", str(source),
                 "--output", str(output)], capture_output=True, text=True, check=False,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertEqual("incomplete", json.loads(output.read_text())["qualification"])

    def test_error_abort_cleanup_loss_and_missing_cycle_fail_closed(self):
        raw = resource_capture()
        for damaged in (
            raw.replace(b"phase=cleanup status=ok", b"phase=cleanup status=error", 1),
            raw.replace(resource_record(1, 7, "cleanup", status="ok", free=64000).encode("ascii"),
                        resource_record(1, 7, "cleanup", status="ok", free=59000).encode("ascii"), 1),
            raw.split(b"cycle=3")[0],
        ):
            with self.assertRaises(katmed_acme_ram.RamBenchError):
                katmed_acme_ram.evaluate_resource(damaged, "client")

    def test_malformed_incomplete_and_wrong_variant_never_qualify(self):
        raw = resource_capture()
        for damaged in (
            raw.replace(b"monitor_valid=1", b"monitor_valid=0", 1),
            raw.replace(b"internal_free=64000", b"internal_free=x", 1),
            raw.replace(b"variant=client", b"variant=baseline", 1),
            raw.replace(b"seq=2", b"seq=4", 1),
            b"",
        ):
            with self.assertRaises(katmed_acme_ram.RamBenchError):
                katmed_acme_ram.evaluate_resource(damaged, "client")


if __name__ == "__main__":
    unittest.main(verbosity=2)
