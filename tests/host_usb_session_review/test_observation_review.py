"""Independent boundary reproductions against the real USB observation client."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import katmed_usb_setup as client


DEVICE = "ac276e46bc2c"
STATUS = (
    "KM1 OK STATUS configured=0 durable=0 pending=0 clock=0 "
    f"device={DEVICE} catalog={'a' * 64} firmware=f4575aa build=abc123 error=OK\n"
).encode("ascii")


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self) -> float:
        return self.now


class FakeSerial:
    def __init__(self, clock: Clock, *, statuses: list[bytes], logs: list[bytes] | None = None,
                 after_status: list[bytes] | None = None):
        self.clock = clock
        self.statuses = statuses.copy()
        self.lines = list(logs or [])
        self.after_status = list(after_status or [])
        self.writes: list[bytes] = []
        self.closed = False

    def write(self, request: bytes) -> int:
        self.writes.append(request)
        if request == b"KM1 STATUS\n" and self.statuses:
            self.lines.append(self.statuses.pop(0))
            self.lines.extend(self.after_status)
            self.after_status.clear()
        return len(request)

    def flush(self) -> None:
        pass

    def read_until(self, _end: bytes, size: int) -> bytes:
        self.clock.now += 1.0
        if not self.lines:
            return b""
        item = self.lines.pop(0)
        assert len(item) <= size
        return item

    def close(self) -> None:
        self.closed = True


def metric(uptime: int) -> bytes:
    fields = [f"{key}={uptime if key == 'uptime_ms' else 0}"
              for key in client.METRIC_KEYS]
    return ("I (123) katmed_metrics: " + " ".join(fields) + "\n").encode("ascii")


def observe(port: FakeSerial, clock: Clock, commands: list[str | None],
            duration: int = 60) -> list[str]:
    output: list[str] = []
    opened: list[str] = []

    def opener(path: str) -> FakeSerial:
        opened.append(path)
        return port

    pending = commands.copy()
    args = argparse.Namespace(port="/dev/serial/by-id/review", expect_device=DEVICE,
                              expect_firmware="f4575aa",
                              duration_seconds=duration)
    try:
        client.run_observation(args, opener=opener, startup=lambda _: None,
                               command=lambda: pending.pop(0) if pending else "quit",
                               clock=clock, emit=output.append)
    finally:
        assert opened == ["/dev/serial/by-id/review"]
        assert port.closed
    return output


class IndependentReviewTests(unittest.TestCase):
    def test_bound_firmware_version_is_required(self) -> None:
        clock = Clock()
        port = FakeSerial(clock, statuses=[STATUS.replace(b"firmware=f4575aa",
                                                          b"firmware=deadbee0")])
        with self.assertRaisesRegex(client.SetupError, "Firmware"):
            observe(port, clock, ["quit"])

    def test_unknown_logs_do_not_postpone_missing_diagnostic_timeout(self) -> None:
        clock = Clock()
        port = FakeSerial(clock, statuses=[STATUS],
                          after_status=[b"I (123) irrelevant: allowed but no metric\n"] * 40)
        with self.assertRaisesRegex(client.SetupError, "Diagnose|Messung"):
            observe(port, clock, [None] * 40 + ["quit"])

    def test_uptime_regression_is_new_boot_not_continuity(self) -> None:
        clock = Clock()
        port = FakeSerial(clock, statuses=[STATUS],
                          after_status=[metric(2000), metric(1000)])
        with self.assertRaisesRegex(client.SetupError, "Neustart|Monoton"):
            observe(port, clock, [None, None, "quit"])

    def test_no_metric_output_before_initial_identity_gate(self) -> None:
        clock = Clock()
        wrong = STATUS.replace(f"device={DEVICE}".encode(), b"device=000000000000")
        port = FakeSerial(clock, statuses=[wrong], logs=[metric(100)])
        output: list[str] = []
        args = argparse.Namespace(port="/dev/serial/by-id/review", expect_device=DEVICE,
                                  expect_firmware="f4575aa", duration_seconds=60)
        with self.assertRaisesRegex(client.SetupError, "Gerätekennung"):
            client.run_observation(args, opener=lambda _: port, startup=lambda _: None,
                                   command=lambda: "quit", clock=clock, emit=output.append)
        self.assertEqual([], output)
        self.assertTrue(port.closed)


if __name__ == "__main__":
    unittest.main()
