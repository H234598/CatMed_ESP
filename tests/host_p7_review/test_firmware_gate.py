"""Port-free review of the real KM1 host command firmware identity gate."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("katmed_usb_setup_p7_review", ROOT / "tools" / "katmed_usb_setup.py")
assert SPEC and SPEC.loader
setup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(setup)

DEVICE = "001122334455"
ACTUAL = "a1b2c3d"
EXPECTED_WRONG = "deadbee"
STATUS = (
    "KM1 OK STATUS configured=0 durable=1 pending=0 clock=0 "
    f"device={DEVICE} catalog={'a' * 64} firmware={ACTUAL} "
    "build=12345678 error=OK\n"
).encode("ascii")


class FakeSerial:
    """An in-process KM1 responder; it never opens a device or stores data."""

    def __init__(self):
        self.lines: list[bytes] = []
        self.writes: list[bytes] = []
        self.mutations = 0
        self.closed = False

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        if data == b"KM1 STATUS\n":
            self.lines.append(STATUS)
        elif data.startswith(b"KM1 PROVISION "):
            self.mutations += 1
            self.lines.append(b"KM1 OK PROVISION\n")
        elif data == b"KM1 RESET CONFIG\n":
            self.mutations += 1
            self.lines.append(b"KM1 OK RESET_CONFIG\n")
        else:
            raise AssertionError("Unexpected synthetic KM1 request")
        return len(data)

    def flush(self) -> None:
        pass

    def read_until(self, expected: bytes = b"\n", size: int | None = None) -> bytes:
        if not self.lines:
            return b""
        answer = self.lines.pop(0)
        assert size is None or len(answer) <= size
        return answer

    def close(self) -> None:
        self.closed = True


def cli_args(command: str, firmware: str | None = EXPECTED_WRONG, device: str = DEVICE):
    tokens = ["--port", "/fake/by-id/review", "--expect-device", device]
    if firmware is not None:
        tokens += ["--expect-firmware", firmware]
    tokens.append(command)
    if command == "reset-config":
        tokens += ["--confirm-reset", "RESET CONFIG"]
    if command == "provision":
        tokens += ["--config", "/fake/private/review.json"]
    return setup.parser().parse_args(tokens)


def run_fake(command: str, firmware: str | None = EXPECTED_WRONG,
             device: str = DEVICE) -> tuple[FakeSerial, str]:
    fake = FakeSerial()
    args = cli_args(command, firmware, device)
    # No local configuration or real key is opened. The public identity path
    # still runs through the actual parser, run_command, read_status, exchange.
    with patch.object(setup, "_private_config_bytes", return_value=b"{}"), \
            patch.object(setup, "build_provision_request",
                         return_value=b"KM1 PROVISION synthetic=1\n"):
        result = setup.run_command(args, opener=lambda path: fake,
                                   startup=lambda port: None)
    return fake, result


class FirmwareGateReview(unittest.TestCase):
    def test_wrong_firmware_rejects_status(self) -> None:
        fake = FakeSerial()
        args = cli_args("status")
        caught = None
        try:
            setup.run_command(args, opener=lambda path: fake,
                              startup=lambda port: None)
        except setup.SetupError as error:
            caught = error
        self.assertEqual([b"KM1 STATUS\n"], fake.writes)
        self.assertEqual(0, fake.mutations)
        self.assertTrue(fake.closed)
        self.assertIsNotNone(caught, "Mismatched firmware was accepted")

    def test_wrong_firmware_rejects_provision_before_mutation(self) -> None:
        fake = FakeSerial()
        args = cli_args("provision")
        caught = None
        with patch.object(setup, "_private_config_bytes", return_value=b"{}"), \
                patch.object(setup, "build_provision_request",
                             return_value=b"KM1 PROVISION synthetic=1\n"):
            try:
                setup.run_command(args, opener=lambda path: fake,
                                  startup=lambda port: None)
            except setup.SetupError as error:
                caught = error
        self.assertEqual([b"KM1 STATUS\n"], fake.writes)
        self.assertEqual(0, fake.mutations)
        self.assertTrue(fake.closed)
        self.assertIsNotNone(caught, "Mismatched firmware was accepted")

    def test_wrong_firmware_rejects_reset_before_mutation(self) -> None:
        fake = FakeSerial()
        args = cli_args("reset-config")
        caught = None
        try:
            setup.run_command(args, opener=lambda path: fake,
                              startup=lambda port: None)
        except setup.SetupError as error:
            caught = error
        self.assertEqual([b"KM1 STATUS\n"], fake.writes)
        self.assertEqual(0, fake.mutations)
        self.assertTrue(fake.closed)
        self.assertIsNotNone(caught, "Mismatched firmware was accepted")

    def test_without_optional_flag_keeps_existing_contract(self) -> None:
        for command in ("status", "provision", "reset-config"):
            with self.subTest(command=command):
                fake, result = run_fake(command, firmware=None)
                self.assertTrue(result)
                if command == "status":
                    self.assertIn(f"firmware={ACTUAL}", result)
                self.assertEqual(0 if command == "status" else 1, fake.mutations)
                self.assertTrue(fake.closed)

    def test_wrong_device_still_rejects_before_mutation(self) -> None:
        for command in ("provision", "reset-config"):
            with self.subTest(command=command):
                fake = FakeSerial()
                args = cli_args(command, firmware=ACTUAL, device="ffeeddccbbaa")
                with patch.object(setup, "_private_config_bytes", return_value=b"{}"), \
                        patch.object(setup, "build_provision_request",
                                     return_value=b"KM1 PROVISION synthetic=1\n"):
                    with self.assertRaises(setup.SetupError):
                        setup.run_command(args, opener=lambda path: fake,
                                          startup=lambda port: None)
                self.assertEqual([b"KM1 STATUS\n"], fake.writes)
                self.assertEqual(0, fake.mutations)
                self.assertTrue(fake.closed)

    def test_observe_strict_hex_and_device_gate_remain(self) -> None:
        for firmware, device in (("d941fae-dirty", DEVICE), (ACTUAL, "bad")):
            with self.subTest(firmware=firmware, device=device):
                openings = 0

                def forbidden_opener(path: str):
                    nonlocal openings
                    openings += 1
                    raise AssertionError("Observe opened a port before validation")

                args = cli_args("observe", firmware=firmware, device=device)
                with self.assertRaises(setup.SetupError):
                    setup.run_observation(args, opener=forbidden_opener,
                                          startup=lambda port: None)
                self.assertEqual(0, openings)


if __name__ == "__main__":
    unittest.main()
