"""Focused port-free boundaries for the production startup phase handoff."""

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
from tests.host_c3_km1_startup_review.test_early_phase_during_drain import (
    Clock, safe_numeric_phase,
)


class ScheduledPort:
    def __init__(self, clock: Clock, chunks: list[bytes], *, tick: float = 0.25):
        self.clock = clock
        self.chunks = chunks.copy()
        self.tick = tick
        self.reads = 0
        self.reset_count = 0
        self.reply_reads = 0
        self.writes: list[bytes] = []
        self.closed = False

    def read(self, size: int = 1) -> bytes:
        self.clock.now += self.tick
        self.reads += 1
        part = self.chunks.pop(0) if self.chunks else b""
        assert len(part) <= size
        return part

    def reset_input_buffer(self) -> None:
        self.reset_count += 1

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        return len(data)

    def flush(self) -> None:
        pass

    def read_until(self, expected: bytes = b"\n", size: int | None = None) -> bytes:
        self.clock.now += self.tick
        self.reply_reads += 1
        return b""

    def close(self) -> None:
        self.closed = True


def invoke(port: ScheduledPort, clock: Clock, evidence: Path) -> tuple[str, dict]:
    args = argparse.Namespace(port="/dev/serial/by-id/synthetic-not-opened",
                              expect_device="ac276e46bc2c",
                              expect_firmware="5042056", expect_build="6c9d77e5b",
                              evidence_dir=str(evidence), duration_seconds=30)
    try:
        longrun.run_longrun(args, opener=lambda _: port,
                            monotonic=clock.monotonic, wall=clock.wall,
                            emit=lambda _: None)
    except longrun.LongrunError as error:
        return str(error), json.loads((evidence / "summary.json").read_text("ascii"))
    raise AssertionError("A synthetic run without STATUS must fail")


class StartupPhaseLimits(unittest.TestCase):
    def test_fragmented_phase_and_discarded_other_lines(self) -> None:
        clock = Clock()
        line = safe_numeric_phase()
        secret = b"fictional-secret-never-persist"
        port = ScheduledPort(clock, [b"ESP-ROM: " + secret + b"\n" + line[:40],
                                     line[40:85], line[85:] + b"other " + secret + b"\n"])
        with tempfile.TemporaryDirectory() as root:
            error, saved = invoke(port, clock, Path(root) / "run")
            self.assertIn("Statusantwort fehlt", error)
            self.assertEqual("prestatus_unverified", saved["phase_coverage"])
            self.assertEqual(1, saved["phase_prestatus_unverified"]["read"])
            self.assertEqual(0, saved["status_count"])
            self.assertNotIn(secret.decode(), json.dumps(saved))
            self.assertEqual([b"KM1 STATUS\n"], port.writes)
            self.assertEqual(1, port.reset_count)
            self.assertTrue(port.closed)

    def test_legacy_startup_remains_missing(self) -> None:
        clock = Clock()
        port = ScheduledPort(clock, [b"ESP-ROM: ordinary synthetic boot\n"])
        with tempfile.TemporaryDirectory() as root:
            error, saved = invoke(port, clock, Path(root) / "run")
            self.assertIn("Statusantwort fehlt", error)
            self.assertEqual("legacy_missing", saved["phase_coverage"])
            self.assertIsNone(saved["phase_prestatus_unverified"])
            self.assertEqual(20, port.reads)  # 5 s / 0.25 s, no extension.
            self.assertEqual(1, port.reset_count)

    def test_damaged_known_phase_keeps_first_valid_observation(self) -> None:
        clock = Clock()
        valid = safe_numeric_phase()
        malformed = valid.replace(b"task=1", b"task=private")
        joined = valid + malformed
        port = ScheduledPort(clock, [joined[n:n + 256]
                                     for n in range(0, len(joined), 256)])
        with tempfile.TemporaryDirectory() as root:
            error, saved = invoke(port, clock, Path(root) / "run")
            self.assertEqual("Numerischer KM1-Phasenbeleg ist beschädigt", error)
            self.assertEqual("prestatus_unverified", saved["phase_coverage"])
            self.assertEqual(1, saved["phase_prestatus_unverified"]["task"])
            self.assertEqual([], port.writes)
            self.assertTrue(port.closed)

    def test_overlong_known_and_partial_known_lines_fail_closed(self) -> None:
        for line in (b"X" * 790 + safe_numeric_phase(),
                     safe_numeric_phase()[:-1]):
            with self.subTest(length=len(line)):
                clock = Clock()
                chunks = [line[n:n + 256] for n in range(0, len(line), 256)]
                port = ScheduledPort(clock, chunks)
                with tempfile.TemporaryDirectory() as root:
                    error, saved = invoke(port, clock, Path(root) / "run")
                    self.assertIn("KM1-Phasenbeleg", error)
                    self.assertEqual("failed", saved["state"])
                    self.assertEqual([], port.writes)
                    self.assertTrue(port.closed)

    def test_byte_cap_and_counter_regression_preserve_failure(self) -> None:
        clock = Clock()
        port = ScheduledPort(clock, [b"X" * 256] * 32, tick=0.05)
        with tempfile.TemporaryDirectory() as root:
            error, saved = invoke(port, clock, Path(root) / "run")
            self.assertIn("sichere Grenze", error)
            self.assertEqual(32, port.reads)
            self.assertEqual([], port.writes)
            self.assertEqual("failed", saved["state"])
        clock = Clock()
        early = safe_numeric_phase()
        backwards = early.replace(b"phase_seq=2", b"phase_seq=3").replace(
            b"read=1", b"read=0")
        joined = early + backwards
        port = ScheduledPort(clock, [joined[n:n + 256]
                                     for n in range(0, len(joined), 256)])
        with tempfile.TemporaryDirectory() as root:
            error, saved = invoke(port, clock, Path(root) / "run")
            self.assertIn("sprang", error)
            self.assertEqual("prestatus_unverified", saved["phase_coverage"])
            self.assertEqual(1, saved["phase_prestatus_unverified"]["read"])
            self.assertEqual([], port.writes)

    def test_bound_inner_diagnostic_startup_uses_same_private_path(self) -> None:
        clock = Clock()
        port = ScheduledPort(clock, [safe_numeric_phase()])
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root) / "run"
            directory.mkdir(mode=0o700)
            args = argparse.Namespace(expect_device="ac276e46bc2c",
                                      expect_firmware="5042056", expect_build="6c9d77e5b",
                                      duration_seconds=30)
            recorder = longrun.Recorder(args, port, directory,
                                       clock.monotonic, clock.wall)
            longrun._startup_with_early_phases(recorder, port, clock.monotonic)
            with self.assertRaisesRegex(longrun.LongrunError, "Statusantwort fehlt"):
                recorder.status()
            recorder.fail("KM1-Statusantwort fehlt innerhalb von acht Sekunden")
            port.close()
            saved = json.loads((directory / "summary.json").read_text("ascii"))
            self.assertEqual("failed", saved["state"])
            self.assertEqual("prestatus_unverified", saved["phase_coverage"])
            self.assertEqual(0, saved["status_count"])
            self.assertEqual([b"KM1 STATUS\n"], port.writes)

    def test_saturated_phase_is_recorded_as_exhausted_and_fails(self) -> None:
        clock = Clock()
        line = safe_numeric_phase().replace(
            b"phase_seq=2", b"phase_seq=18446744073709551615")
        port = ScheduledPort(clock, [line])
        with tempfile.TemporaryDirectory() as root:
            error, saved = invoke(port, clock, Path(root) / "run")
            self.assertIn("erschöpft", error)
            self.assertEqual("exhausted", saved["phase_coverage"])
            self.assertEqual([], port.writes)
            self.assertTrue(port.closed)

    def test_read_larger_than_requested_chunk_fails_before_status(self) -> None:
        clock = Clock()

        class OversizePort(ScheduledPort):
            def read(self, size: int = 1) -> bytes:
                self.clock.now += self.tick
                self.reads += 1
                return b"X" * (size + 1)

        port = OversizePort(clock, [])
        with tempfile.TemporaryDirectory() as root:
            error, saved = invoke(port, clock, Path(root) / "run")
            self.assertEqual("Ungültige USB-Startausgabe", error)
            self.assertEqual(0, saved["status_count"])
            self.assertEqual([], port.writes)
            self.assertTrue(port.closed)


if __name__ == "__main__":
    unittest.main()
