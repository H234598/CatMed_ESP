"""Port-free repro of a valid early phase through the real startup/recorder path."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import katmed_longrun as longrun
import katmed_usb_setup as usb


class Clock:
    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def wall(self) -> float:
        return 1_800_000_000.0 + self.now


def safe_numeric_phase() -> bytes:
    # The key order and bounded numeric shape are the existing product
    # formatter contract exercised by host_c3_km1_phases/test_phase_host.py.
    # This is synthetic metadata; it contains no captured device bytes.
    values = dict.fromkeys(usb.USB_PHASE_KEYS, 0)
    values.update(schema=1, phase_seq=2, uptime_ms=1200, boot_domain=391,
                  task=1, read=1)
    return ("I (1200) katmed_usb_phase: " +
            " ".join(f"{key}={values[key]}" for key in usb.USB_PHASE_KEYS) +
            "\n").encode("ascii")


class FakeSerial:
    def __init__(self, clock: Clock) -> None:
        self.clock = clock
        self.early_line = safe_numeric_phase()
        self.drain_reads = 0
        self.reply_reads = 0
        self.resets = 0
        self.writes: list[bytes] = []
        self.closed = False

    def read(self, size: int = 1) -> bytes:
        self.clock.now += 0.25
        self.drain_reads += 1
        if self.drain_reads == 1:
            assert len(self.early_line) <= size
            return self.early_line
        return b""

    def reset_input_buffer(self) -> None:
        self.resets += 1

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        return len(data)

    def flush(self) -> None:
        pass

    def read_until(self, expected: bytes = b"\n", size: int | None = None) -> bytes:
        self.clock.now += 0.25
        self.reply_reads += 1
        return b""  # Nothing after the actual startup drain, including STATUS.

    def close(self) -> None:
        self.closed = True


class EarlyPhaseDuringDrain(unittest.TestCase):
    def test_early_task_reader_phase_survives_failed_first_status(self) -> None:
        clock = Clock()
        port = FakeSerial(clock)
        with tempfile.TemporaryDirectory(prefix="katmed-km1-startup-review-") as root:
            evidence = Path(root) / "run"
            args = argparse.Namespace(
                port="/dev/serial/by-id/synthetic-not-opened",
                expect_device="ac276e46bc2c", expect_firmware="5042056",
                expect_build="6c9d77e5b", evidence_dir=str(evidence),
                duration_seconds=30,
            )
            with self.assertRaisesRegex(longrun.LongrunError,
                                        "KM1-Statusantwort fehlt innerhalb von acht Sekunden"):
                longrun.run_longrun(
                    args, opener=lambda _: port, startup=usb._drain_startup,
                    monotonic=clock.monotonic, wall=clock.wall, emit=lambda _: None,
                )
            saved = json.loads((evidence / "summary.json").read_text("ascii"))
            self.assertEqual("failed", saved["state"])
            self.assertEqual("KM1-Statusantwort fehlt innerhalb von acht Sekunden",
                             saved["reason"])
            self.assertEqual(0, saved["status_count"])
            self.assertIsNone(saved["phase_last_verified"])
            self.assertEqual([b"KM1 STATUS\n"], port.writes)
            self.assertGreater(port.drain_reads, 1)
            self.assertGreater(port.reply_reads, 0)
            self.assertEqual(1, port.resets)
            self.assertTrue(port.closed)
            self.assertEqual("prestatus_unverified", saved["phase_coverage"])
            self.assertEqual(1, saved["phase_prestatus_unverified"]["task"])
            self.assertEqual(1, saved["phase_prestatus_unverified"]["read"])
            self.assertEqual(0, saved["phase_prestatus_unverified"]["status"])


if __name__ == "__main__":
    unittest.main()
