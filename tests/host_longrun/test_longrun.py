"""Controlled-time tests for the real KM1 long-run parser path."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import katmed_usb_setup as usb
import katmed_longrun as longrun


DEVICE = "ac276e46bc2c"
FIRMWARE = "abc1234"
BUILD = "abc123"
CATALOG = "a" * 64


def status(*, device=DEVICE, firmware=FIRMWARE, build=BUILD) -> bytes:
    return ("KM1 OK STATUS configured=1 durable=1 pending=0 clock=2 "
            f"device={device} catalog={CATALOG} firmware={firmware} "
            f"build={build} error=OK\n").encode("ascii")


def metric(uptime: int) -> bytes:
    values = [f"{key}={uptime if key == 'uptime_ms' else 0}"
              for key in usb.METRIC_KEYS]
    return ("I (123) katmed_metrics: " + " ".join(values) + "\n").encode()


class Clock:
    def __init__(self):
        self.now = 0.0

    def mono(self):
        return self.now

    def wall(self):
        return 1_800_000_000.0 + self.now


class FakeSerial:
    def __init__(self, clock, *, statuses=None, schedule=None):
        self.clock = clock
        self.statuses = list(statuses or [status()] * 20)
        self.schedule = list(schedule or [])
        self.pending = []
        self.writes = []
        self.closed = False

    def write(self, data):
        self.writes.append(data)
        if data == b"KM1 STATUS\n":
            self.pending.append(self.statuses.pop(0) if self.statuses else status())
        return len(data)

    def flush(self):
        pass

    def read_until(self, expected=b"\n", size=None):
        if self.pending:
            return self.pending.pop(0)
        if self.schedule:
            delay, line = self.schedule.pop(0)
            self.clock.now += delay
            return line
        self.clock.now += 1
        return b""

    def close(self):
        self.closed = True


def continuous(base=100_000, seconds=60):
    return [(0, metric(base))] + [(10, metric(base + n * 10_000))
                                  for n in range(1, seconds // 10 + 1)]


class LongrunTests(unittest.TestCase):
    def invoke(self, port, clock, directory, *, duration=60):
        args = argparse.Namespace(port="/dev/serial/by-id/fake", expect_device=DEVICE,
                                  expect_firmware=FIRMWARE, expect_build=BUILD,
                                  evidence_dir=str(directory), duration_seconds=duration)
        return longrun.run_longrun(args, opener=lambda _: port, startup=lambda _: None,
                                   monotonic=clock.mono, wall=clock.wall, emit=lambda _: None)

    def test_48_hours_and_uptime_over_49_days(self):
        clock = Clock()
        base = 4_300_000_000
        schedule = [(0, metric(base))] + [(10, metric(base + n * 10_000))
                                          for n in range(1, 17_281)]
        port = FakeSerial(clock, schedule=schedule)
        with tempfile.TemporaryDirectory() as root:
            evidence = Path(root) / "run"
            result = self.invoke(port, clock, evidence, duration=172800)
            saved = json.loads((evidence / "summary.json").read_text())
            self.assertEqual("complete", result["state"])
            self.assertEqual("complete", saved["state"])
            self.assertGreaterEqual(saved["elapsed_seconds"], 172800)
            self.assertGreaterEqual(saved["uptime_elapsed_ms"], 172800000)
            self.assertLess((evidence / "summary.json").stat().st_size, 8192)
            self.assertEqual(0o700, evidence.stat().st_mode & 0o777)
            self.assertEqual(0o600, (evidence / "summary.json").stat().st_mode & 0o777)
            self.assertTrue(port.closed)

    def test_48_hours_allows_normal_cumulative_clock_drift(self):
        clock = Clock()
        base = 4_300_000_000
        # 20 ppm yields 3.456 seconds of device/host difference after 48 h.
        schedule = [(0, metric(base))] + [(10, metric(base + int(n * 10_000 * 0.99998)))
                                          for n in range(1, 17_283)]
        port = FakeSerial(clock, schedule=schedule)
        with tempfile.TemporaryDirectory() as root:
            result = self.invoke(port, clock, Path(root) / "run", duration=172800)
            self.assertEqual("complete", result["state"])
            self.assertGreater(clock.now, 172800)

    def check_failure(self, port, clock, *, message, duration=60):
        with tempfile.TemporaryDirectory() as root:
            evidence = Path(root) / "run"
            with self.assertRaisesRegex(longrun.LongrunError, message):
                self.invoke(port, clock, evidence, duration=duration)
            saved = json.loads((evidence / "summary.json").read_text())
            self.assertEqual("failed", saved["state"])
            self.assertLess((evidence / "summary.json").stat().st_size, 8192)
            self.assertTrue(port.closed)
            return saved

    def test_short_complete_and_no_resume(self):
        clock = Clock()
        port = FakeSerial(clock, schedule=continuous())
        with tempfile.TemporaryDirectory() as root:
            evidence = Path(root) / "run"
            self.assertEqual("complete", self.invoke(port, clock, evidence)["state"])
            with self.assertRaisesRegex(longrun.LongrunError, "existiert"):
                self.invoke(FakeSerial(clock), clock, evidence)
            self.assertEqual([b"KM1 STATUS\n"], port.writes)

    def test_early_unmarked_reset_does_not_credit_priming_time(self):
        clock = Clock()
        # A markerless reset makes the second uptime larger than the first,
        # yet the two deltas differ by only 0.6 s under the permitted 2 s.
        port = FakeSerial(clock, schedule=[(10, metric(100)),
                                           (10, metric(9_500)),
                                           (10, metric(19_500)),
                                           (10, metric(29_500)),
                                           (10, metric(39_500)),
                                           (10, metric(49_500))])
        with tempfile.TemporaryDirectory() as root:
            result = self.invoke(port, clock, Path(root) / "run", duration=30)
            self.assertEqual("complete", result["state"])
            self.assertEqual(2, result["priming_metric_count"])
            self.assertEqual(4, result["metric_count"])
            self.assertEqual(19_500, result["uptime_start_ms"])
            self.assertEqual(1_800_000_030, result["start_unix_seconds"])
            self.assertEqual(30, result["elapsed_seconds"])
            self.assertEqual([b"KM1 STATUS\n", b"KM1 STATUS\n"], port.writes)

    def test_priming_without_safe_uptime_fails_without_start_credit(self):
        clock = Clock()
        port = FakeSerial(clock, schedule=[(10, metric(100)),
                                           (10, metric(200)),
                                           (10, metric(300)),
                                           (10, metric(400))])
        saved = self.check_failure(port, clock, message="unstimmig|Vorphase")
        self.assertIsNone(saved["start_unix_seconds"])

    def test_uptime_rollback_and_boot_marker(self):
        clock = Clock()
        self.check_failure(FakeSerial(clock, schedule=[(0, metric(200_000)),
                                                       (10, metric(100_000))]), clock,
                           message="sprang zurück")
        clock = Clock()
        self.check_failure(FakeSerial(clock, schedule=[(0, metric(100_000)),
                                                       (10, b"ESP-ROM: reboot\n")]), clock,
                           message="neustart")

    def test_identity_and_firmware_mismatch(self):
        clock = Clock()
        self.check_failure(FakeSerial(clock, statuses=[status(device="000000000000")]),
                           clock, message="kennung")
        clock = Clock()
        self.check_failure(FakeSerial(clock, statuses=[status(), status(firmware="deadbee")],
                                      schedule=continuous(seconds=70)),
                           clock, message="kennung", duration=120)
        clock = Clock()
        catalog_changed = status().replace(CATALOG.encode(), ("b" * 64).encode())
        self.check_failure(FakeSerial(clock, statuses=[status(), catalog_changed],
                                      schedule=continuous(seconds=70)),
                           clock, message="Katalog", duration=120)

    def test_missing_or_damaged_periodic_status(self):
        clock = Clock()
        class MissingSecondStatus(FakeSerial):
            def write(self, data):
                self.writes.append(data)
                if len(self.writes) == 1:
                    self.pending.append(status())
                return len(data)
        self.check_failure(MissingSecondStatus(clock,
                                               schedule=continuous(seconds=100)),
                           clock, message="Statusantwort", duration=120)
        clock = Clock()
        self.check_failure(FakeSerial(clock, statuses=[status(),
                                                      status().replace(b"error=OK", b"error=WRONG")],
                                      schedule=continuous(seconds=70)),
                           clock, message="Fehlercode", duration=120)

    def test_port_open_failure_persists_private_failure(self):
        clock = Clock()
        with tempfile.TemporaryDirectory() as root:
            evidence = Path(root) / "run"
            args = argparse.Namespace(port="/dev/serial/by-id/fake", expect_device=DEVICE,
                                      expect_firmware=FIRMWARE, expect_build=BUILD,
                                      evidence_dir=str(evidence), duration_seconds=60)
            def fail_open(_path):
                raise RuntimeError("secret=must-not-leak")
            with self.assertRaisesRegex(longrun.LongrunError, "Abbruch"):
                longrun.run_longrun(args, opener=fail_open, startup=lambda _: None,
                                    monotonic=clock.mono, wall=clock.wall,
                                    emit=lambda _: None)
            saved = json.loads((evidence / "summary.json").read_text())
            self.assertEqual("failed", saved["state"])
            self.assertEqual("Abbruch oder unerwarteter Laufzeitfehler", saved["reason"])
            self.assertNotIn("must-not-leak", json.dumps(saved))
            self.assertIsNone(saved["start_unix_seconds"])

    def test_missing_and_corrupt_diagnostic(self):
        clock = Clock()
        self.check_failure(FakeSerial(clock), clock, message="Erste gültige Diagnose")
        clock = Clock()
        bad = metric(100_000).replace(b"internal_min_free=0", b"internal_min_free=NaN")
        self.check_failure(FakeSerial(clock, schedule=[(0, metric(100_000)), (10, bad)]),
                           clock, message="beschädigt")
        clock = Clock()
        self.check_failure(FakeSerial(clock, schedule=[(0, metric(100_000))]),
                           clock, message="Diagnose")

    def test_overlong_eof_and_input_limit(self):
        clock = Clock()
        self.check_failure(FakeSerial(clock, schedule=[(0, metric(100_000)),
                                                       (1, b"SECRET" * 150 + b"\n")]),
                           clock, message="zu lang")
        class EofPort(FakeSerial):
            def read_until(self, expected=b"\n", size=None):
                if self.pending:
                    return self.pending.pop(0)
                raise EOFError("secret raw")
        clock = Clock()
        saved = self.check_failure(EofPort(clock), clock, message="beendet")
        self.assertNotIn("secret raw", json.dumps(saved))
        clock = Clock()
        with mock.patch.object(longrun, "MAX_INPUT_LINES", 2):
            self.check_failure(FakeSerial(clock, schedule=[(0, metric(100_000)),
                                                           (1, b"unknown\n")]),
                               clock, message="Ressourcengrenze")

    def test_host_clock_jump_and_uptime_progress_gate(self):
        clock = Clock()
        class JumpClock(Clock):
            def wall(self):
                return 1_800_000_000 + self.now * 10
        jump = JumpClock()
        self.check_failure(FakeSerial(jump, schedule=continuous(seconds=30)),
                           jump, message="Hostzeit")
        slow = [(0, metric(100_000))] + [(10, metric(100_000 + n)) for n in range(1, 10)]
        saved = self.check_failure(FakeSerial(clock, schedule=slow), clock,
                                   message="unstimmig", duration=60)
        self.assertLess(saved["uptime_elapsed_ms"], 60_000)
        clock = Clock()
        fast = [(0, metric(100_000))] + [(10, metric(100_000 + n * 100_000))
                                            for n in range(1, 8)]
        self.check_failure(FakeSerial(clock, schedule=fast), clock,
                           message="unstimmig")

    def test_unknown_input_is_counted_not_saved_and_interrupt_is_failure(self):
        clock = Clock()
        class InterruptPort(FakeSerial):
            def read_until(self, expected=b"\n", size=None):
                if self.pending:
                    return self.pending.pop(0)
                if self.schedule:
                    return super().read_until(expected, size)
                raise KeyboardInterrupt()
        port = InterruptPort(clock, schedule=[(0, metric(100_000)),
                                             (1, b"secret=never-save-this\n")])
        saved = self.check_failure(port, clock, message="Abbruch")
        self.assertEqual(1, saved["unknown_line_count"])
        self.assertNotIn("never-save-this", json.dumps(saved))


if __name__ == "__main__":
    unittest.main()
