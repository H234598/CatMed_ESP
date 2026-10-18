"""Independent FakeSerial and controlled-time checks for the 48-hour recorder.

No device, network, flash, or real 48-hour clock is used by this suite.
"""

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
import katmed_longrun as recorder


DEVICE = "ac276e46bc2c"
FIRMWARE = "abc1234"
BUILD = "abc123"
POISON = "SYNTHETIC_PRIVATE_SECRET_VALUE"


def status(*, device: str = DEVICE, firmware: str = FIRMWARE,
           build: str = BUILD) -> bytes:
    return (
        "KM1 OK STATUS configured=1 durable=1 pending=0 clock=2 "
        f"device={device} catalog={'a' * 64} firmware={firmware} "
        f"build={build} error=OK\n"
    ).encode("ascii")


def metric(uptime: int, **overrides: int) -> bytes:
    values = " ".join(
        f"{key}={uptime if key == 'uptime_ms' else overrides.get(key, 0)}"
        for key in usb.METRIC_KEYS
    )
    return f"I (123) katmed_metrics: {values}\n".encode("ascii")


class Clock:
    def __init__(self, now: float = 0.0) -> None:
        self.now = now

    def monotonic(self) -> float:
        return self.now

    def wall(self) -> float:
        return 1_800_000_000.0 + self.now


class FakeSerial:
    """One fake port whose reads advance host time and supply real KM1 grammar."""

    def __init__(self, clock: Clock, *, interval: float = 10.0,
                 statuses: list[bytes] | None = None,
                 events: dict[int, bytes | BaseException] | None = None,
                 uptime_base: int = 4_300_000_000,
                 drift_over_48h_seconds: float = 0.0,
                 uptime_rate: float = 1.0) -> None:
        self.clock = clock
        self.interval = interval
        self.statuses = list(statuses or [])
        self.events = events or {}
        self.uptime_base = uptime_base
        self.drift_over_48h_seconds = drift_over_48h_seconds
        self.uptime_rate = uptime_rate
        self.pending: list[bytes] = []
        self.read_count = 0
        self.writes: list[bytes] = []
        self.read_sizes: list[int | None] = []
        self.closed = False

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        if data == b"KM1 STATUS\n":
            self.pending.append(self.statuses.pop(0) if self.statuses else status())
        return len(data)

    def flush(self) -> None:
        pass

    def read_until(self, expected: bytes = b"\n", size: int | None = None) -> bytes:
        self.read_sizes.append(size)
        if self.pending:
            return self.pending.pop(0)
        self.clock.now += self.interval
        self.read_count += 1
        event = self.events.get(self.read_count)
        if isinstance(event, BaseException):
            raise event
        if event is not None:
            return event
        drift = self.drift_over_48h_seconds * self.clock.now / 172800
        return metric(self.uptime_base + int((self.clock.now * self.uptime_rate - drift) * 1000))

    def close(self) -> None:
        self.closed = True


def call(port: FakeSerial, evidence: Path, *, duration: int = 60,
         expected_device: str = DEVICE, expected_firmware: str = FIRMWARE,
         expected_build: str = BUILD, output: list[str] | None = None):
    openings: list[str] = []
    def opener(path: str) -> FakeSerial:
        openings.append(path)
        return port
    args = argparse.Namespace(
        port="/dev/serial/by-id/independent-fake", expect_device=expected_device,
        expect_firmware=expected_firmware, expect_build=expected_build,
        evidence_dir=str(evidence), duration_seconds=duration,
    )
    try:
        return recorder.run_longrun(
            args, opener=opener, startup=lambda _: None,
            monotonic=port.clock.monotonic, wall=port.clock.wall,
            emit=(output if output is not None else []).append,
        )
    finally:
        assert len(openings) <= 1, "The recorder opened a second port"


def summary(evidence: Path) -> dict:
    return json.loads((evidence / "summary.json").read_text())


class IndependentLongrunTests(unittest.TestCase):
    def test_early_unmarked_reboot_cannot_credit_pre_reset_time_to_48_hours(self) -> None:
        # First accepted sample: 100 ms at host second 10. A reset then occurs
        # without a ROM marker; at host second 20 the new boot is already at
        # 9.5 s. Subsequent samples advance one device second per host second.
        # The two valid lines carry the same STATUS identity/build throughout.
        with tempfile.TemporaryDirectory() as tmp:
            clock = Clock()
            port = FakeSerial(clock, interval=10, uptime_base=-10_500,
                              events={1: metric(100)})
            evidence = Path(tmp) / "evidence"
            try:
                result = call(port, evidence, duration=172800)
            except recorder.LongrunError:
                result = summary(evidence)
            self.assertTrue(port.closed)
            self.assertTrue(all(write == b"KM1 STATUS\n" for write in port.writes))
            if result["state"] == "complete":
                self.assertGreaterEqual(
                    result["start_unix_seconds"], 1_800_000_020,
                    f"An unmarked reset was credited from the pre-reset first sample: {result}",
                )
                self.assertGreaterEqual(result["uptime_start_ms"], 9500)
                self.assertGreaterEqual(result["elapsed_seconds"], 172800)
            else:
                self.assertEqual("failed", result["state"])

    def test_same_unmarked_reboot_after_old_baseline_is_detected(self) -> None:
        # With a 1 h first Uptime the same restart necessarily moves the
        # uptime backwards, so the existing rollback guard must end the run.
        with tempfile.TemporaryDirectory() as tmp:
            clock = Clock()
            port = FakeSerial(clock, interval=10, uptime_base=-10_500,
                              events={1: metric(3_600_000)})
            evidence = Path(tmp) / "evidence"
            with self.assertRaises(recorder.LongrunError):
                call(port, evidence, duration=172800)
            journal = summary(evidence)
            self.assertEqual("failed", journal["state"])
            self.assertEqual(1, journal["metric_count"])
            self.assertTrue(port.closed)

    def test_post_reset_priming_needs_fresh_status_before_counted_start(self) -> None:
        # The first STATUS is at host t=0, before the unmarked reset between
        # first and second metric. EOF at t=60 stops before the old periodic
        # STATUS; a promoted counted start must therefore cause a fresh STATUS
        # after t=20 rather than relying on the pre-reset STATUS at t=0.
        with tempfile.TemporaryDirectory() as tmp:
            clock = Clock()
            port = FakeSerial(clock, interval=10, uptime_base=-10_500,
                              events={1: metric(100), 6: EOFError("stop")})
            evidence = Path(tmp) / "evidence"
            with self.assertRaises(recorder.LongrunError):
                call(port, evidence, duration=172800)
            journal = summary(evidence)
            self.assertEqual("failed", journal["state"])
            if journal["start_unix_seconds"] is not None:
                self.assertGreaterEqual(journal["start_unix_seconds"], 1_800_000_020)
                self.assertGreaterEqual(journal["status_count"], 2)
                self.assertGreaterEqual(journal["last_status_unix_seconds"], 1_800_000_020)
                self.assertLessEqual(journal["last_status_unix_seconds"],
                                     journal["start_unix_seconds"])
            self.assertTrue(port.closed)

    def test_summary_keeps_numeric_extrema_and_last_confirmed_probe(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            clock = Clock()
            base = 4_300_000_000
            samples = {
                1: metric(base + 10_000, internal_min_free=500,
                          internal_largest_block=1000, main_stack_hwm_bytes=300),
                2: metric(base + 20_000, internal_min_free=400,
                          internal_largest_block=900, main_stack_hwm_bytes=250),
                3: metric(base + 30_000, internal_min_free=450,
                          internal_largest_block=1200, main_stack_hwm_bytes=280),
                4: metric(base + 40_000, internal_min_free=420,
                          internal_largest_block=1100, main_stack_hwm_bytes=260),
            }
            port = FakeSerial(clock, events=samples)
            evidence = Path(tmp) / "evidence"
            call(port, evidence, duration=30)
            journal = summary(evidence)
            self.assertEqual("complete", journal["state"])
            self.assertEqual(4, journal["metric_count"])
            self.assertEqual(400, journal["metric_min"]["internal_min_free"])
            self.assertEqual(500, journal["metric_max"]["internal_min_free"])
            self.assertEqual(900, journal["metric_min"]["internal_largest_block"])
            self.assertEqual(1200, journal["metric_max"]["internal_largest_block"])
            self.assertEqual(250, journal["metric_min"]["main_stack_hwm_bytes"])
            self.assertEqual(300, journal["metric_max"]["main_stack_hwm_bytes"])
            self.assertEqual(base + 40_000, journal["uptime_last_ms"])
            self.assertEqual(1_800_000_040, journal["last_metric_unix_seconds"])
            self.assertEqual(1_800_000_000, journal["last_status_unix_seconds"])
            self.assertEqual(30, journal["elapsed_seconds"])

    def test_full_real_duration_contract_with_controlled_time_and_49_day_uptime(self) -> None:
        clock = Clock()
        port = FakeSerial(clock, interval=20)
        with tempfile.TemporaryDirectory() as tmp:
            evidence = Path(tmp) / "evidence"
            result = call(port, evidence, duration=172800)
            journal = summary(evidence)
            self.assertEqual("complete", result["state"])
            self.assertEqual("complete", journal["state"])
            self.assertGreaterEqual(journal["elapsed_seconds"], 172800)
            self.assertGreaterEqual(journal["uptime_elapsed_ms"], 172800000)
            self.assertEqual(0o700, evidence.stat().st_mode & 0o777)
            for item in evidence.iterdir():
                self.assertTrue(item.is_file())
                self.assertEqual(0o600, item.stat().st_mode & 0o777)
                self.assertLess(item.stat().st_size, 1024 * 1024)
            self.assertLessEqual(sum(item.stat().st_size for item in evidence.iterdir()), 2 * 1024 * 1024)
            self.assertTrue(port.closed)
            self.assertTrue(port.writes)
            self.assertTrue(all(write == b"KM1 STATUS\n" for write in port.writes))
            self.assertTrue(all(size is not None and size <= usb.MAX_DIAG_CHARS + 2
                                for size in port.read_sizes))

    def test_two_second_cumulative_device_clock_drift_still_allows_full_run(self) -> None:
        clock = Clock()
        port = FakeSerial(clock, interval=10, drift_over_48h_seconds=2.0)
        with tempfile.TemporaryDirectory() as tmp:
            evidence = Path(tmp) / "evidence"
            result = call(port, evidence, duration=172800)
            journal = summary(evidence)
            self.assertEqual("complete", result["state"])
            self.assertEqual("complete", journal["state"])
            self.assertGreaterEqual(journal["elapsed_seconds"], 172800)
            self.assertGreaterEqual(journal["uptime_elapsed_ms"], 172800000)
            self.assertLessEqual(journal["elapsed_seconds"], 172830)
            self.assertTrue(port.closed)

    def test_implausibly_fast_device_uptime_is_not_continuous_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            clock = Clock()
            port = FakeSerial(clock, uptime_rate=10.0)
            evidence = Path(tmp) / "evidence"
            with self.assertRaises(Exception):
                call(port, evidence, duration=120)
            self.assertTrue(port.closed)
            self.assertNotEqual("complete", summary(evidence)["state"])

    def test_identity_and_public_build_gates_fail_closed(self) -> None:
        cases = [
            (status(device="0" * 12), DEVICE, FIRMWARE, BUILD),
            (status(firmware="deadbee"), DEVICE, FIRMWARE, BUILD),
            (status(build="feed12"), DEVICE, FIRMWARE, BUILD),
        ]
        for initial, device, firmware, build in cases:
            with self.subTest(initial=initial[-55:]), tempfile.TemporaryDirectory() as tmp:
                clock = Clock()
                port = FakeSerial(clock, statuses=[initial])
                with self.assertRaises(Exception):
                    call(port, Path(tmp) / "evidence", expected_device=device,
                         expected_firmware=firmware, expected_build=build)
                self.assertTrue(port.closed)
                self.assertEqual([b"KM1 STATUS\n"], port.writes)

    def test_uptime_rollback_boot_marker_and_eof_never_complete(self) -> None:
        cases = [
            {2: metric(1000)},
            {2: b"ESP-ROM:esp32c3-api1-20210207\n"},
            {2: EOFError(POISON)},
        ]
        for events in cases:
            with self.subTest(events=repr(events)[:100]), tempfile.TemporaryDirectory() as tmp:
                clock = Clock()
                port = FakeSerial(clock, events=events)
                with self.assertRaises(Exception) as caught:
                    call(port, Path(tmp) / "evidence")
                self.assertNotIn(POISON, str(caught.exception))
                self.assertTrue(port.closed)
                evidence = Path(tmp) / "evidence"
                if (evidence / "summary.json").exists():
                    self.assertNotEqual("complete", summary(evidence)["state"])

    def test_bad_diagnostics_and_raw_unknown_lines_cannot_become_success(self) -> None:
        cases = [
            lambda _: b"I (123) katmed_metrics: uptime_ms=not-a-number\n",
            lambda _: (POISON.encode() + b"\n"),
            lambda _: (POISON.encode() * 200 + b"\n"),
        ]
        for make_event in cases:
            with self.subTest(kind=make_event(0)[:40]), tempfile.TemporaryDirectory() as tmp:
                clock = Clock()
                poison = make_event(0)
                port = FakeSerial(clock, events={i: poison for i in range(1, 20)})
                output: list[str] = []
                with self.assertRaises(Exception) as caught:
                    call(port, Path(tmp) / "evidence", output=output)
                self.assertNotIn(POISON, str(caught.exception))
                self.assertNotIn(POISON, "\n".join(output))
                self.assertTrue(port.closed)
                files = list((Path(tmp) / "evidence").glob("*"))
                for item in files:
                    if item.is_file():
                        self.assertNotIn(POISON.encode(), item.read_bytes())

    def test_diagnostic_loss_after_valid_sample_is_not_hidden_by_unknown_logs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            clock = Clock()
            unknown = f"I (123) unrelated: {POISON}\n".encode()
            port = FakeSerial(clock, events={i: unknown for i in range(2, 20)})
            evidence = Path(tmp) / "evidence"
            output: list[str] = []
            with self.assertRaises(Exception):
                call(port, evidence, duration=120, output=output)
            journal = summary(evidence)
            self.assertNotEqual("complete", journal["state"])
            self.assertEqual(1, journal["metric_count"])
            self.assertGreater(journal["unknown_line_count"], 0)
            self.assertTrue(port.closed)
            self.assertNotIn(POISON, "\n".join(output))
            self.assertNotIn(POISON.encode(), (evidence / "summary.json").read_bytes())

    def test_periodic_status_rejects_changed_identity_and_catalog(self) -> None:
        changed = [
            status(device="0" * 12),
            status(firmware="deadbee"),
            status(build="feed12"),
            status().replace(b"catalog=" + b"a" * 64,
                             b"catalog=" + b"b" * 64),
        ]
        for second in changed:
            with self.subTest(second=second[-85:]), tempfile.TemporaryDirectory() as tmp:
                clock = Clock()
                port = FakeSerial(clock, statuses=[status(), second])
                with self.assertRaises(Exception):
                    call(port, Path(tmp) / "evidence", duration=120)
                self.assertTrue(port.closed)
                self.assertGreaterEqual(port.writes.count(b"KM1 STATUS\n"), 2)
                self.assertNotEqual("complete", summary(Path(tmp) / "evidence")["state"])

    def test_host_wall_clock_jump_is_failure_not_elapsed_time_credit(self) -> None:
        class JumpClock(Clock):
            def wall(self) -> float:
                jump = 600.0 if self.now >= 20 else 0.0
                return 1_800_000_000.0 + self.now + jump
        with tempfile.TemporaryDirectory() as tmp:
            clock = JumpClock()
            port = FakeSerial(clock)
            with self.assertRaises(Exception):
                call(port, Path(tmp) / "evidence", duration=120)
            self.assertTrue(port.closed)
            self.assertNotEqual("complete", summary(Path(tmp) / "evidence")["state"])

    def test_host_abort_and_same_directory_retry_never_hide_first_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            evidence = Path(tmp) / "evidence"
            first_clock = Clock()
            first_port = FakeSerial(first_clock, events={3: KeyboardInterrupt()})
            with self.assertRaises(Exception):
                call(first_port, evidence, duration=120)
            first = summary(evidence)
            self.assertNotEqual("complete", first["state"])
            self.assertIsNotNone(first["start_unix_seconds"])
            first_bytes = (evidence / "summary.json").read_bytes()
            second_clock = Clock(3600)
            second_port = FakeSerial(second_clock)
            with self.assertRaises(Exception):
                call(second_port, evidence, duration=120)
            self.assertEqual(first_bytes, (evidence / "summary.json").read_bytes())
            self.assertEqual([], second_port.writes)
            self.assertFalse(second_port.closed)  # No second port was opened.

    def test_stale_running_journal_after_process_loss_cannot_be_resumed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            evidence = Path(tmp) / "evidence"
            evidence.mkdir(mode=0o700)
            previous = {
                "state": "running", "start_unix_seconds": 1_800_000_000,
                "last_unix_seconds": 1_800_003_600, "metric_count": 360,
            }
            target = evidence / "summary.json"
            target.write_text(json.dumps(previous))
            target.chmod(0o600)
            original = target.read_bytes()
            clock = Clock(7200)
            port = FakeSerial(clock)
            with self.assertRaises(Exception):
                call(port, evidence, duration=172800)
            self.assertEqual(original, target.read_bytes())
            self.assertEqual([], port.writes)
            self.assertEqual(1_800_000_000, summary(evidence)["start_unix_seconds"])

    def test_atomic_journal_replace_failure_cannot_leave_complete_record(self) -> None:
        original_replace = recorder.os.replace
        calls = 0
        def fail_once(source, target):
            nonlocal calls
            calls += 1
            if calls == 4:
                raise OSError(POISON)
            return original_replace(source, target)
        with tempfile.TemporaryDirectory() as tmp:
            clock = Clock()
            port = FakeSerial(clock)
            evidence = Path(tmp) / "evidence"
            with mock.patch.object(recorder.os, "replace", side_effect=fail_once):
                with self.assertRaises(Exception) as caught:
                    call(port, evidence, duration=120)
            self.assertNotIn(POISON, str(caught.exception))
            self.assertTrue(port.closed)
            self.assertNotEqual("complete", summary(evidence)["state"])
            self.assertFalse((evidence / ".summary.tmp").exists())
            for item in evidence.iterdir():
                self.assertNotIn(POISON.encode(), item.read_bytes())

    def test_fixed_input_budget_fails_at_boundary_without_raw_log(self) -> None:
        for field, limit in (("input_lines", recorder.MAX_INPUT_LINES),
                             ("input_bytes", recorder.MAX_INPUT_BYTES)):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as tmp:
                clock = Clock()
                port = FakeSerial(clock, events={1: POISON.encode() + b"\n"})
                evidence = Path(tmp) / "evidence"
                evidence.mkdir(mode=0o700)
                args = argparse.Namespace(
                    expect_device=DEVICE, expect_firmware=FIRMWARE,
                    expect_build=BUILD, duration_seconds=172800,
                )
                session = recorder.Recorder(args, port, evidence,
                                            clock.monotonic, clock.wall)
                session.summary[field] = limit
                with self.assertRaises(recorder.LongrunError):
                    session._line()
                self.assertLess((evidence / "summary.json").stat().st_size,
                                recorder.MAX_SUMMARY_BYTES)
                self.assertNotIn(POISON.encode(), (evidence / "summary.json").read_bytes())

    def test_evidence_parent_must_be_private_and_existing_run_cannot_be_reused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            parent = Path(tmp) / "public"
            parent.mkdir(mode=0o755)
            evidence = parent / "evidence"
            clock = Clock()
            port = FakeSerial(clock)
            with self.assertRaises(Exception):
                call(port, evidence)
            self.assertFalse(evidence.exists())
            self.assertEqual([], port.writes)
            private = Path(tmp) / "private"
            private.mkdir(mode=0o700)
            existing = private / "evidence"
            existing.mkdir(mode=0o700)
            with self.assertRaises(Exception):
                call(port, existing)
            self.assertEqual([], port.writes)

    def test_cli_fixes_real_run_to_48_hours_even_though_api_accepts_test_duration(self) -> None:
        observed: list[int] = []
        def capture(args, **_kwargs):
            observed.append(args.duration_seconds)
            return {"state": "complete"}
        with mock.patch.object(recorder, "run_longrun", side_effect=capture):
            result = recorder.main([
                "--port", "/dev/serial/by-id/not-opened",
                "--expect-device", DEVICE,
                "--expect-firmware", FIRMWARE,
                "--expect-build", BUILD,
                "--evidence-dir", "/never/opened",
            ])
        self.assertEqual(0, result)
        self.assertEqual([172800], observed)


if __name__ == "__main__":
    unittest.main()
