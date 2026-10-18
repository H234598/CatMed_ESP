"""Independent, port-free guard for the existing human observation contract."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import katmed_usb_setup as setup


class NoPort:
    def __init__(self) -> None:
        self.opened = False

    def __call__(self, _path: str):
        self.opened = True
        raise AssertionError("A port must not be opened for an invalid duration")


class LegacyObservationContract(unittest.TestCase):
    def test_human_observation_still_has_its_original_fixed_limits(self) -> None:
        self.assertEqual(1800, setup.MAX_SESSION_SECONDS)
        self.assertEqual(512 * 1024, setup.MAX_SESSION_INPUT_BYTES)
        self.assertEqual(4096, setup.MAX_SESSION_LINES)
        self.assertEqual(1024, setup.MAX_SESSION_OUTPUT_LINES)
        self.assertEqual(128 * 1024, setup.MAX_SESSION_OUTPUT_BYTES)
        self.assertEqual(767, setup.MAX_DIAG_CHARS)
        self.assertEqual(256, setup.MAX_RESPONSE_BYTES)

    def test_longrun_cannot_sneak_through_human_observe(self) -> None:
        args = argparse.Namespace(
            port="/dev/serial/by-id/unused-fake",
            expect_device="ac276e46bc2c",
            expect_firmware="abcdef0",
            duration_seconds=172800,
        )
        opener = NoPort()
        with self.assertRaisesRegex(setup.SetupError, "30 und 1800"):
            setup.run_observation(args, opener=opener)
        self.assertFalse(opener.opened)


if __name__ == "__main__":
    unittest.main()
