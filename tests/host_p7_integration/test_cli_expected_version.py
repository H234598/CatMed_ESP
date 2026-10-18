"""Port-free positive and malformed identity checks for the actual KM1 client."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch


root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("katmed_usb_p7", root / "tools" / "katmed_usb_setup.py")
assert spec and spec.loader
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)


class Port:
    def __init__(self) -> None:
        self.requests: list[bytes] = []
        self.replies: list[bytes] = []
        self.closed = False

    def write(self, data: bytes) -> int:
        self.requests.append(data)
        if data == b"KM1 STATUS\n":
            self.replies.append((
                "KM1 OK STATUS configured=0 durable=1 pending=0 clock=0 "
                "device=001122334455 catalog=" + "a" * 64 +
                " firmware=a1b2c3d build=abcdef123 error=OK\n"
            ).encode("ascii"))
        elif data.startswith(b"KM1 PROVISION "):
            self.replies.append(b"KM1 OK PROVISION\n")
        elif data == b"KM1 RESET CONFIG\n":
            self.replies.append(b"KM1 OK RESET_CONFIG\n")
        else:
            raise AssertionError("unexpected synthetic request")
        return len(data)

    def read_until(self, expected: bytes = b"\n", size: int | None = None) -> bytes:
        reply = self.replies.pop(0)
        assert size is None or len(reply) <= size
        return reply

    def flush(self) -> None:
        pass

    def close(self) -> None:
        self.closed = True


class ExpectedVersion(unittest.TestCase):
    def test_matching_version_all_command_paths(self) -> None:
        for command in ("status", "provision", "reset-config"):
            with self.subTest(command=command):
                tokens = ["--port", "/synthetic/by-id", "--expect-device", "001122334455",
                          "--expect-firmware", "A1B2C3D", command]
                if command == "reset-config":
                    tokens += ["--confirm-reset", "RESET CONFIG"]
                port = Port()
                with patch.object(client, "_private_config_bytes", return_value=b"{}"), \
                        patch.object(client, "build_provision_request",
                                     return_value=b"KM1 PROVISION synthetic=1\n"):
                    result = client.run_command(client.parser().parse_args(tokens),
                                                opener=lambda path: port,
                                                startup=lambda p: None)
                self.assertTrue(result)
                self.assertTrue(port.closed)
                self.assertEqual(1 if command == "status" else 2, len(port.requests))

    def test_malformed_explicit_version_never_opens_port(self) -> None:
        for version in ("d941fae-dirty", "123456", "123456789abcd", "nothex"):
            with self.subTest(version=version):
                args = client.parser().parse_args(["--port", "/synthetic/by-id",
                                                    "--expect-firmware", version, "status"])
                with self.assertRaises(client.SetupError):
                    client.run_command(args, opener=lambda path: self.fail("port opened"),
                                       startup=lambda p: None)


if __name__ == "__main__":
    unittest.main()
