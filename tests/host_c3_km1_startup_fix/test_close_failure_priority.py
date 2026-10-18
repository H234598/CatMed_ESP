"""A later close error must not replace the first safe recorder failure."""

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
from tests.host_c3_km1_startup_review.test_early_phase_during_drain import (
    Clock, safe_numeric_phase,
)
from tests.host_c3_km1_startup_fix.test_startup_phase_limits import ScheduledPort, invoke
from tests.host_longrun.test_longrun import (
    Clock as ValuedClock, FakeSerial as ValuedPort, continuous,
    DEVICE, FIRMWARE, BUILD,
)


class BadClosePort(ScheduledPort):
    def close(self) -> None:
        self.closed = True
        raise OSError("synthetic-private-close")


class CloseFailurePriority(unittest.TestCase):
    def test_status_timeout_survives_later_close_failure(self) -> None:
        clock = Clock()
        port = BadClosePort(clock, [safe_numeric_phase()])
        with tempfile.TemporaryDirectory() as root:
            error, saved = invoke(port, clock, Path(root) / "run")
            self.assertEqual("KM1-Statusantwort fehlt innerhalb von acht Sekunden", error)
            self.assertEqual(error, saved["reason"])
            self.assertEqual("failed", saved["state"])
            self.assertEqual("prestatus_unverified", saved["phase_coverage"])
            self.assertEqual(0, saved["status_count"])
            self.assertEqual([b"KM1 STATUS\n"], port.writes)
            self.assertTrue(port.closed)
            self.assertNotIn("synthetic-private-close", json.dumps(saved))

    def test_startup_error_survives_later_close_failure(self) -> None:
        clock = Clock()
        port = BadClosePort(clock, [safe_numeric_phase()[:-1]])
        with tempfile.TemporaryDirectory() as root:
            error, saved = invoke(port, clock, Path(root) / "run")
            self.assertEqual("Numerischer KM1-Phasenbeleg ist unvollständig", error)
            self.assertEqual(error, saved["reason"])
            self.assertEqual("failed", saved["state"])
            self.assertEqual(0, saved["status_count"])
            self.assertEqual([], port.writes)
            self.assertTrue(port.closed)

    def test_close_failure_as_sole_error_still_fails_and_is_saved(self) -> None:
        clock = ValuedClock()

        class BadCloseValuedPort(ValuedPort):
            def close(self) -> None:
                self.closed = True
                raise OSError("synthetic-private-close")

        port = BadCloseValuedPort(clock, schedule=continuous(seconds=30))
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root) / "run"
            args = argparse.Namespace(
                port="/dev/serial/by-id/synthetic-not-opened",
                expect_device=DEVICE, expect_firmware=FIRMWARE,
                expect_build=BUILD, evidence_dir=str(directory), duration_seconds=30,
            )
            with self.assertRaisesRegex(longrun.LongrunError,
                                        "USB-Port konnte nicht sicher geschlossen werden"):
                longrun.run_longrun(
                    args, opener=lambda _: port, startup=lambda _: None,
                    monotonic=clock.mono, wall=clock.wall, emit=lambda _: None,
                )
            saved = json.loads((directory / "summary.json").read_text("ascii"))
            self.assertEqual("failed", saved["state"])
            self.assertEqual("USB-Port konnte nicht sicher geschlossen werden",
                             saved["reason"])
            self.assertTrue(port.closed)
            self.assertNotIn("synthetic-private-close", json.dumps(saved))


if __name__ == "__main__":
    unittest.main()
