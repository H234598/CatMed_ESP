"""Fake-serial tests of the actual bounded KM1 host client."""

from __future__ import annotations

import argparse
import os
import re
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import katmed_usb_setup as setup


DEVICE = "ac276e46bc2c"
CATALOG = "a" * 64
STATUS = (
    f"KM1 OK STATUS configured=0 durable=0 pending=0 clock=0 "
    f"device={DEVICE} catalog={CATALOG} firmware=1.0 build=abc123 error=OK\n"
).encode("ascii")


class FakeSerial:
    def __init__(self, replies: list[bytes], *, partial_at: int | None = None, error_at: int | None = None):
        self.replies = list(replies)
        self.writes: list[bytes] = []
        self.partial_at = partial_at
        self.error_at = error_at
        self.closed = False

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        if self.error_at == len(self.writes):
            raise RuntimeError(data.decode("ascii"))
        return len(data) - (1 if self.partial_at == len(self.writes) else 0)

    def flush(self) -> None:
        pass

    def read(self, size: int = 1) -> bytes:
        return b""

    def reset_input_buffer(self) -> None:
        pass

    def read_until(self, expected: bytes = b"\n", size: int | None = None) -> bytes:
        if not self.replies:
            return b""
        return self.replies.pop(0)[:size]

    def close(self) -> None:
        self.closed = True


class BootSerial(FakeSerial):
    def __init__(self, startup: bytes, replies: list[bytes]):
        super().__init__(replies)
        self.startup = startup
        self.position = 0
        self.drained = False
        self.read_sizes: list[int] = []

    def read(self, size: int = 1) -> bytes:
        self.read_sizes.append(size)
        end = min(self.position + size, len(self.startup))
        chunk = self.startup[self.position:end]
        self.position = end
        return chunk

    def reset_input_buffer(self) -> None:
        self.drained = True
        self.position = len(self.startup)

    def write(self, data: bytes) -> int:
        if not self.drained:
            raise RuntimeError("request before boot drain")
        return super().write(data)


class FakeClock:
    def __init__(self, step: float = 0.1):
        self.value = 0.0
        self.step = step

    def __call__(self) -> float:
        self.value += self.step
        return self.value


def cfg(**overrides: object) -> bytes:
    values = {
        "group": "1" * 32,
        "secret": "2" * 64,
        "ssid": "ExampleSSID",
        "password": "example-only-password",
        "time_source": "pool.ntp.org",
    }
    values.update(overrides)
    import json

    return json.dumps(values).encode("utf-8")


def args(command: str, **overrides: object) -> argparse.Namespace:
    values = {
        "command": command,
        "port": "/fake/by-id/device",
        "expect_device": DEVICE,
        "config": "/fake/private/usb-setup.json",
        "confirm_reset": None,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


def firmware_metric_line() -> bytes:
    """Zero-valued fixture from the actual firmware's numeric log formatter."""
    source = (Path(__file__).resolve().parents[2] /
              "firmware/main/runtime_metrics.c").read_text()
    formatter = source.split('ESP_LOGI("katmed_metrics",', 1)[1].split(
        "input->uptime_ms", 1)[0]
    text = "".join(re.findall(r'"([^"\n]*)"', formatter))
    return ("I (0) katmed_metrics: " + re.sub(r"%[udi]?", "0", text) + "\n").encode("ascii")


class UsbSetupTests(unittest.TestCase):
    def test_firmware_diagnostic_before_status_and_commit_reply(self) -> None:
        metric = firmware_metric_line()
        self.assertIsNotNone(setup._metric_line(metric.rstrip(b"\n")))
        self.assertGreater(len(metric), setup.MAX_RESPONSE_BYTES)
        self.assertLessEqual(len(metric), setup.MAX_DIAG_CHARS + 2)
        fake = FakeSerial([metric, STATUS])
        self.assertEqual(DEVICE, setup.read_status(fake)["device"])
        self.assertEqual([b"KM1 STATUS\n"], fake.writes)
        self.assertEqual("KM1 OK PROVISION", setup.exchange(
            FakeSerial([metric, b"KM1 OK PROVISION\n"]),
            b"KM1 PROVISION\n", "KM1 OK PROVISION"))

    def test_mixed_stream_keeps_protocol_diagnostic_and_line_bounds(self) -> None:
        metric = firmware_metric_line()
        for replies in ([b"x" * setup.MAX_DIAG_CHARS + b"\r\n", STATUS],
                        [metric] * 15 + [STATUS]):
            self.assertEqual(DEVICE, setup.read_status(FakeSerial(replies))["device"])
        for replies in ([metric, b"KM1 OK STATUS " + b"x" * 300 + b"\n"],
                        [b"x" * (setup.MAX_DIAG_CHARS + 1) + b"\n", STATUS],
                        [metric] * 16 + [STATUS],
                        [metric, STATUS[:-1]],
                        [metric, STATUS.replace(b"durable=0", b"durable=2")],
                        [metric, STATUS.replace(b"clock=0", b"clock=3")]):
            with self.subTest(), self.assertRaises(setup.SetupError):
                setup.read_status(FakeSerial(replies))

    def test_mixed_stream_commit_witness_keeps_nonce_and_identity_guards(self) -> None:
        nonce = "0" * 32
        reply = (f"KM1 OK PROVEBOOT n={nonce} dev={DEVICE} cat={CATALOG} "
                 "fw=1234567 b=abcdef e=1 x=1 u=1\n").encode("ascii")
        for response in (reply, reply.replace(nonce.encode(), b"1" * 32),
                         reply.replace(DEVICE.encode(), b"0" * 12)):
            fake = FakeSerial([firmware_metric_line(), response])
            parsed = setup._bound_exchange(fake, b"KM1 PROVEBOOT synthetic\n")
            if response == reply:
                self.assertEqual({"epoch": 1, "domain": 1, "uptime_ms": 1},
                    setup._bound_fields(parsed, "PROVEBOOT", nonce,
                        expect_device=DEVICE, expect_firmware="1234567",
                        expect_build="abcdef", expect_catalog=CATALOG))
            else:
                with self.assertRaises(setup.SetupError):
                    setup._bound_fields(parsed, "PROVEBOOT", nonce,
                        expect_device=DEVICE, expect_firmware="1234567",
                        expect_build="abcdef", expect_catalog=CATALOG)

    def test_shared_opener_configures_idle_lines_before_exactly_one_open(self) -> None:
        from types import SimpleNamespace
        port_name = "/dev/serial/by-id/synthetic-device"
        for timeout in (3.0, 0.25):
            events = []
            class Spy:
                def __init__(self, **kwargs):
                    events.append(("construct", kwargs))
                def __setattr__(self, name, value):
                    events.append((name, value))
                    object.__setattr__(self, name, value)
                def open(self):
                    events.append(("open", self.port, self.dtr, self.rts))
            with patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=Spy)}), \
                 patch.object(setup.Path, "is_symlink", return_value=True):
                port = setup._open_serial(port_name, timeout=timeout)
            self.assertIsInstance(port, Spy)
            self.assertEqual([
                ("construct", dict(port=None, baudrate=115200, timeout=timeout, write_timeout=3)),
                ("dtr", False), ("rts", False), ("port", port_name),
                ("open", port_name, False, False),
            ], events)

    def test_shared_opener_keeps_path_gate_and_sanitizes_open_errors(self) -> None:
        from types import SimpleNamespace
        events = []
        class Spy:
            def __init__(self, **kwargs): events.append("construct")
            def open(self):
                events.append("open")
                raise RuntimeError("SYNTHETIC_PRIVATE_ECHO")
        with patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=Spy)}):
            with self.assertRaises(setup.SetupError):
                setup._open_serial("/dev/ttyACM0")
            self.assertEqual([], events)
            with patch.object(setup.Path, "is_symlink", return_value=True), \
                 self.assertRaises(setup.SetupError) as caught:
                setup._open_serial("/dev/serial/by-id/synthetic-device")
            self.assertEqual(["construct", "open"], events)
            self.assertNotIn("SYNTHETIC_PRIVATE_ECHO", str(caught.exception))

    def test_all_field_maxima_fit_wire_limit(self) -> None:
        request = setup.build_provision_request(cfg(ssid="a" * 32, password="b" * 63, time_source="c" * 63))
        self.assertEqual(463, len(request))
        self.assertLessEqual(len(request), setup.MAX_REQUEST_BYTES)

    def test_unicode_limits_are_bytes_and_ascii_time(self) -> None:
        self.assertLess(len(setup.build_provision_request(cfg(ssid="ä" * 16))), setup.MAX_REQUEST_BYTES)
        for invalid in [cfg(ssid="ä" * 17), cfg(password="x" * 7), cfg(time_source="ä"),
                        cfg(time_source="pool:123"), cfg(ssid="x\nY"), cfg(ssid="x\x00Y"),
                        cfg(ssid="x\x7fY"), cfg(password="abcdefgh\x00"),
                        cfg(password="abcdefgh\x1f")]:
            with self.subTest(invalid=invalid[:20]), self.assertRaises(setup.SetupError):
                setup.build_provision_request(invalid)

    def test_unknown_or_malformed_configuration_is_rejected(self) -> None:
        for invalid in [b"{}", b"[1]", b"not json", b'{"group":"a","group":"b"}', cfg(group="x" * 32), cfg(secret="x" * 64), cfg(group="0" * 32), cfg(secret="0" * 64), cfg(extra="x")]:
            with self.subTest(invalid=invalid[:20]), self.assertRaises(setup.SetupError):
                setup.build_provision_request(invalid)

    def test_private_file_owner_permissions_size_and_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            private = Path(d)
            private.chmod(0o700)
            data = private / "usb-setup.json"
            data.write_bytes(cfg())
            data.chmod(0o600)
            with patch.object(setup, "PRIVATE_DIR", private):
                self.assertEqual(cfg(), setup._private_config_bytes(data))
                data.chmod(0o644)
                with self.assertRaises(setup.SetupError):
                    setup._private_config_bytes(data)
                data.chmod(0o600)
                private.chmod(0o755)
                with self.assertRaises(setup.SetupError):
                    setup._private_config_bytes(data)
                private.chmod(0o700)
                link = private / "link.json"
                link.symlink_to(data)
                with self.assertRaises(setup.SetupError):
                    setup._private_config_bytes(link)
                data.write_bytes(b"x" * (setup.MAX_CONFIG_BYTES + 1))
                with self.assertRaises(setup.SetupError):
                    setup._private_config_bytes(data)

    def test_status_parses_and_rejects_corruption(self) -> None:
        fake = FakeSerial([b"boot output\n", STATUS])
        self.assertEqual(DEVICE, setup.read_status(fake)["device"])
        self.assertEqual([b"KM1 STATUS\n"], fake.writes)
        bad = [STATUS[:-1], b"KM1 OK STATUS configured=1 configured=0\n", b"KM1 ERR bad secret\n", STATUS.replace(b"error=OK", b"error=SYNTHETIC_TEST_SECRET"), b"KM1 OK STATUS " + b"x" * 300 + b"\n"]
        for response in bad:
            with self.subTest(response=response[:30]), self.assertRaises(setup.SetupError):
                setup.read_status(FakeSerial([response]))

    def test_exactly_one_terminal_cr_before_lf_is_accepted(self) -> None:
        # The real ESP-IDF console returned the otherwise valid status with
        # `error=OK\r\n`. Only this single line-ending CR is normalized.
        crlf = STATUS[:-1] + b"\r\n"
        self.assertEqual(DEVICE, setup.read_status(FakeSerial([crlf]))["device"])
        self.assertEqual("KM1 OK PROVISION", setup.exchange(
            FakeSerial([b"KM1 OK PROVISION\r\n"]), b"KM1 PROVISION\n", "KM1 OK PROVISION"))
        for invalid in [STATUS[:-1] + b"\r\r\n",
                        STATUS.replace(b"durable", b"dur\rable"),
                        b"KM1 OK PROVISION\r\r\n"]:
            with self.subTest(invalid=invalid[-25:]), self.assertRaises(setup.SetupError):
                if invalid.startswith(b"KM1 OK PROVISION"):
                    setup.exchange(FakeSerial([invalid]), b"KM1 PROVISION\n", "KM1 OK PROVISION")
                else:
                    setup.read_status(FakeSerial([invalid]))

    def test_identity_preflight_and_confirmed_reset(self) -> None:
        wrong = FakeSerial([STATUS])
        with self.assertRaisesRegex(setup.SetupError, "Gerätekennung"):
            setup.run_command(args("reset-config", expect_device="0" * 12, confirm_reset="RESET CONFIG"), lambda _: wrong, lambda _: None)
        self.assertEqual([b"KM1 STATUS\n"], wrong.writes)
        self.assertTrue(wrong.closed)
        no_confirm = FakeSerial([STATUS])
        with self.assertRaisesRegex(setup.SetupError, "confirm-reset"):
            setup.run_command(args("reset-config"), lambda _: no_confirm, lambda _: None)
        self.assertEqual([], no_confirm.writes)
        success = FakeSerial([STATUS, b"KM1 OK RESET_CONFIG\n"])
        result = setup.run_command(args("reset-config", confirm_reset="RESET CONFIG"), lambda _: success, lambda _: None)
        self.assertIn("Outbox", result)
        self.assertEqual([b"KM1 STATUS\n", b"KM1 RESET CONFIG\n"], success.writes)

    def test_provision_success_denial_partial_write_and_no_secret_echo(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            private = Path(d)
            private.chmod(0o700)
            data = private / "usb-setup.json"
            data.write_bytes(cfg())
            data.chmod(0o600)
            request_args = args("provision", config=str(data))
            with patch.object(setup, "PRIVATE_DIR", private):
                success = FakeSerial([STATUS, b"KM1 OK PROVISION\n"])
                result = setup.run_command(request_args, lambda _: success, lambda _: None)
                self.assertIn("dauerhaft", result)
                self.assertEqual(2, len(success.writes))
                self.assertNotIn("example-only-password", result)
                self.assertNotIn("2" * 64, result)
                for fake in [FakeSerial([STATUS, b"KM1 ERR STORAGE\n"]), FakeSerial([STATUS], partial_at=2), FakeSerial([STATUS], error_at=2)]:
                    with self.subTest(fake=fake), self.assertRaises(setup.SetupError) as caught:
                        setup.run_command(request_args, lambda _: fake, lambda _: None)
                    self.assertNotIn("example-only-password", str(caught.exception))
                    self.assertNotIn("2" * 64, str(caught.exception))
                    self.assertTrue(fake.closed)
                unknown = FakeSerial([STATUS, b"KM1 ERR SYNTHETIC_TEST_SECRET\n"])
                with self.assertRaises(setup.SetupError) as caught:
                    setup.run_command(request_args, lambda _: unknown, lambda _: None)
                self.assertNotIn("SYNTHETIC_TEST_SECRET", str(caught.exception))

    def test_boot_reset_output_is_bounded_and_drained_before_status(self) -> None:
        boot_prefix = b"KM1 OK STATUS configured=0 garbage=boot\nSYNTHETIC_TEST_SECRET\n"
        boot = boot_prefix + b"B" * (3204 - len(boot_prefix))
        fake = BootSerial(boot, [STATUS])
        clock = FakeClock()
        result = setup.run_command(args("status"), lambda _: fake,
                                   lambda port: setup._drain_startup(port, clock))
        self.assertEqual([b"KM1 STATUS\n"], fake.writes)
        self.assertTrue(fake.drained and fake.closed)
        self.assertEqual(len(boot), fake.position)
        self.assertTrue(all(0 < n <= setup.STARTUP_CHUNK_BYTES for n in fake.read_sizes))
        self.assertNotIn("SYNTHETIC_TEST_SECRET", result)
        self.assertIn(f"device={DEVICE}", result)

    def test_excessive_boot_output_is_capped_and_purged_before_request(self) -> None:
        fake = BootSerial(b"X" * (setup.MAX_STARTUP_BYTES + 1), [STATUS])
        clock = FakeClock()
        with patch.object(setup.time, "sleep"):
            setup.run_command(args("status"), lambda _: fake,
                              lambda port: setup._drain_startup(port, clock))
        self.assertTrue(fake.drained)
        self.assertEqual(setup.MAX_STARTUP_BYTES // setup.STARTUP_CHUNK_BYTES,
                         len(fake.read_sizes))
        self.assertEqual([b"KM1 STATUS\n"], fake.writes)
        self.assertTrue(fake.closed)

    def test_identity_gate_remains_after_boot_drain(self) -> None:
        fake = BootSerial(b"boot\n" * 500, [STATUS])
        clock = FakeClock()
        with self.assertRaisesRegex(setup.SetupError, "Gerätekennung"):
            setup.run_command(args("reset-config", expect_device="0" * 12,
                                   confirm_reset="RESET CONFIG"), lambda _: fake,
                              lambda port: setup._drain_startup(port, clock))
        self.assertEqual([b"KM1 STATUS\n"], fake.writes)
        self.assertTrue(fake.closed)


if __name__ == "__main__":
    unittest.main()
