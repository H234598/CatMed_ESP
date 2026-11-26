"""Port-free positive and malformed identity checks for the actual KM1 client."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch


root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("katmed_usb_p7", root / "tools" / "katmed_usb_setup.py")
if spec is None or spec.loader is None:
    raise AssertionError("KM1 client test module could not be loaded")
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)


class Port:
    def __init__(self, firmware: str = "a1b2c3d") -> None:
        self.firmware = firmware
        self.requests: list[bytes] = []
        self.replies: list[bytes] = []
        self.closed = False

    def write(self, data: bytes) -> int:
        self.requests.append(data)
        if data == b"KM1 STATUS\n":
            self.replies.append((
                "KM1 OK STATUS configured=0 durable=1 pending=0 clock=0 "
                "device=001122334455 catalog=" + "a" * 64 +
                f" firmware={self.firmware} build=abcdef012345678901234567 error=OK\n"
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
        if size is not None and len(reply) > size:
            raise AssertionError("Synthetic reply exceeds the read bound")
        return reply

    def flush(self) -> None:
        # The fake makes each write immediately available.
        return None

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
                if command == "provision":
                    tokens += ["--expect-build-prefix", "abcdef0123456789"]
                port = Port()
                with patch.object(client, "_private_config_bytes", return_value=b"{}"), \
                        patch.object(client, "build_provision_request",
                                     return_value=b"KM1 PROVISION synthetic=1\n"):
                    result = client.run_command(client.parser().parse_args(tokens),
                                                opener=lambda _path, port=port: port,
                                                startup=lambda p: None)
                self.assertTrue(result)
                self.assertTrue(port.closed)
                self.assertEqual(1 if command == "status" else 2, len(port.requests))

    def test_malformed_explicit_version_never_opens_port(self) -> None:
        for version in ("123456", "123456789abcd", "nothex", "tree-012345678",
                        "tree-0123456789a", "tree-012345678A", "123456-dirty",
                        "123456789abcd-dirty", "abcdef0-DIRTY", "abcdef0-dirty ",
                        "abcdeｆ0-dirty", "abcdef0-dirty\n", "tree-0123456789\r"):
            with self.subTest(version=version):
                args = client.parser().parse_args(["--port", "/synthetic/by-id",
                                                    "--expect-firmware", version, "status"])
                with self.assertRaises(client.SetupError):
                    client.run_command(args, opener=lambda path: self.fail("port opened"),
                                       startup=lambda p: None)

    def test_native_versions_require_full_exact_identity_before_write(self) -> None:
        for expected, actual, matches in (
            ("tree-0123456789", "tree-0123456789", True),
            ("511293c-dirty", "511293c-dirty", True),
            ("511293c-dirty", "511293C-dirty", False),
            ("511293c-dirty", "511293c", False),
            ("511293c", "511293ca", False),
            ("tree-0123456789", "tree-012345678a", False),
        ):
            with self.subTest(expected=expected, actual=actual):
                args = client.parser().parse_args([
                    "--port", "/synthetic/by-id", "--expect-device", "001122334455",
                    "--expect-firmware", expected, "reset-config", "--confirm-reset", "RESET CONFIG",
                ])
                port = Port(actual)

                def opener(_path: str, port: Port = port) -> Port:
                    return port

                if matches:
                    client.run_command(args, opener=opener, startup=lambda _: None)
                    self.assertEqual([b"KM1 STATUS\n", b"KM1 RESET CONFIG\n"], port.requests)
                else:
                    with self.assertRaisesRegex(client.SetupError, "stimmt nicht"):
                        client.run_command(args, opener=opener, startup=lambda _: None)
                    self.assertEqual([b"KM1 STATUS\n"], port.requests)
                self.assertTrue(port.closed)


if __name__ == "__main__":
    unittest.main()
