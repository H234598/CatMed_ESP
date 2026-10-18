"""Bounded FakeSerial reproductions of the real observation client."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import katmed_usb_setup as setup


DEVICE = "ac276e46bc2c"
STATUS = (
    "KM1 OK STATUS configured=0 durable=0 pending=0 clock=0 "
    f"device={DEVICE} catalog={'a' * 64} firmware=f4575aa build=abc123 error=OK\n"
).encode("ascii")


def metrics(*, crlf: bool = False, long: bool = False) -> bytes:
    values = ["0"] * len(setup.METRIC_KEYS)
    if long:
        # The longest fixed-schema numeric diagnostic still below 768 chars.
        for i in range(len(values)):
            values[i] = "1" * 19
        while True:
            candidate = b"I (123) katmed_metrics: " + b" ".join(
                f"{key}={value}".encode() for key, value in zip(setup.METRIC_KEYS, values))
            if len(candidate) >= setup.MAX_DIAG_CHARS:
                break
            values[len(candidate) % len(values)] += "1"
        if len(candidate) > setup.MAX_DIAG_CHARS:
            values[len(candidate) % len(values)] = values[len(candidate) % len(values)][:-1]
    body = b"I (123) katmed_metrics: " + b" ".join(
        f"{key}={value}".encode() for key, value in zip(setup.METRIC_KEYS, values))
    return body + (b"\r\n" if crlf else b"\n")


class Clock:
    def __init__(self, step: float = 0.01):
        self.now = 0.0
        self.step = step

    def __call__(self) -> float:
        self.now += self.step
        return self.now


class FakeSerial:
    def __init__(self, *, statuses: list[bytes] | None = None,
                 idle: list[bytes | Exception] | None = None,
                 after_status: list[bytes | Exception] | None = None):
        self.statuses = list(statuses if statuses is not None else [STATUS, STATUS])
        self.lines: list[bytes | Exception] = list(idle or [])
        self.after_status = list(after_status or [])
        self.writes: list[bytes] = []
        self.closed = False
        self.read_sizes: list[int] = []

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        if data == b"KM1 STATUS\n" and self.statuses:
            self.lines.append(self.statuses.pop(0))
            self.lines.extend(self.after_status)
            self.after_status.clear()
        return len(data)

    def flush(self) -> None:
        pass

    def read_until(self, expected: bytes = b"\n", size: int | None = None) -> bytes:
        self.read_sizes.append(size or 0)
        if not self.lines:
            return b""
        item = self.lines.pop(0)
        if isinstance(item, Exception):
            raise item
        return item[:size]

    def close(self) -> None:
        self.closed = True


def args(device: str = DEVICE, firmware: str = "f4575aa", duration: int = 30) -> argparse.Namespace:
    return argparse.Namespace(port="/dev/serial/by-id/fake", expect_device=device,
                              expect_firmware=firmware, duration_seconds=duration)


def commands(*sequence: str | None):
    pending = list(sequence)
    return lambda: pending.pop(0) if pending else "quit"


class ObservationTests(unittest.TestCase):
    def run_client(self, port: FakeSerial, sequence: tuple[str | None, ...] = ("quit",),
                   *, device: str = DEVICE, clock: Clock | None = None,
                   duration: int = 30) -> list[str]:
        output: list[str] = []
        openings: list[str] = []
        def opener(path: str) -> FakeSerial:
            openings.append(path)
            return port
        setup.run_observation(args(device, duration=duration), opener=opener, startup=lambda _: None,
                              command=commands(*sequence), clock=clock or Clock(), emit=output.append)
        self.assertEqual(["/dev/serial/by-id/fake"], openings)
        self.assertTrue(port.closed)
        return output

    def test_one_open_multiple_statuses_with_long_numeric_logs_and_crlf(self) -> None:
        long_metric = metrics(long=True, crlf=True)
        self.assertLessEqual(len(long_metric) - 2, setup.MAX_DIAG_CHARS)
        port = FakeSerial(statuses=[STATUS[:-1] + b"\r\n", STATUS],
                          after_status=[long_metric, b"SYNTHETIC_TEST_SECRET\n"])
        output = self.run_client(port, (None, None, "status", "quit"))
        self.assertEqual([b"KM1 STATUS\n", b"KM1 STATUS\n"], port.writes)
        self.assertEqual(2, sum(line.startswith("Status ") for line in output))
        self.assertEqual(1, sum(line.startswith("Messung ") for line in output))
        self.assertNotIn("SYNTHETIC_TEST_SECRET", "\n".join(output))
        self.assertTrue(all(size == setup.MAX_DIAG_CHARS + 2 for size in port.read_sizes))

    def test_diagnostic_between_later_request_and_reply_cannot_displace_status(self) -> None:
        port = FakeSerial(statuses=[STATUS, STATUS], after_status=[metrics()])
        output = self.run_client(port, ("status", "quit"))
        self.assertEqual(2, sum(line.startswith("Status ") for line in output))
        self.assertEqual(1, sum(line.startswith("Messung ") for line in output))

    def test_pre_identity_metric_is_never_attributed_or_printed(self) -> None:
        for bad_status in [STATUS.replace(f"device={DEVICE}".encode(), b"device=000000000000"),
                           STATUS.replace(b"firmware=f4575aa", b"firmware=deadbee0")]:
            port = FakeSerial(statuses=[bad_status], idle=[metrics()])
            output: list[str] = []
            with self.subTest(bad_status=bad_status[-60:]), self.assertRaises(setup.SetupError):
                setup.run_observation(args(), opener=lambda _: port, startup=lambda _: None,
                                      command=commands("quit"), clock=Clock(), emit=output.append)
            self.assertEqual([], output)
            self.assertEqual([b"KM1 STATUS\n"], port.writes)
            self.assertTrue(port.closed)

    def test_malformed_missing_timeout_eof_and_secret_suppression(self) -> None:
        cases = [
            FakeSerial(statuses=[STATUS[:-1]]),
            FakeSerial(statuses=[]),
            FakeSerial(statuses=[], idle=[EOFError("SYNTHETIC_TEST_SECRET")]),
            FakeSerial(statuses=[b"KM1 OK STATUS " + b"SYNTHETIC_TEST_SECRET" * 20 + b"\n"]),
            FakeSerial(statuses=[b"KM1 ERR SYNTHETIC_TEST_SECRET\n"]),
        ]
        for port in cases:
            with self.subTest(case=len(cases)), self.assertRaises(setup.SetupError) as caught:
                self.run_client(port, clock=Clock(step=0.1))
            self.assertTrue(port.closed)
            self.assertNotIn("SYNTHETIC_TEST_SECRET", str(caught.exception))

    def test_idle_overlong_line_and_transport_error_close_port_without_raw_output(self) -> None:
        for bad in [b"SYNTHETIC_TEST_SECRET" * 50 + b"\n",
                    OSError("SYNTHETIC_TEST_SECRET"), EOFError("SYNTHETIC_TEST_SECRET"),
                    b"ESP-ROM:esp32c3-api1-20210207\n"]:
            port = FakeSerial(statuses=[STATUS], idle=[bad])
            with self.subTest(type=type(bad)), self.assertRaises(setup.SetupError) as caught:
                self.run_client(port, (None, "quit"))
            self.assertTrue(port.closed)
            self.assertNotIn("SYNTHETIC_TEST_SECRET", str(caught.exception))

    def test_km1_limit_remains_256_even_when_diagnostics_allow_767(self) -> None:
        for response in [b"KM1 OK STATUS " + b"x" * 242 + b"\n",
                         b"KM1 OK STATUS " + b"x" * 241 + b"\r\n"]:
            self.assertEqual(257, len(response))
            port = FakeSerial(statuses=[response])
            with self.subTest(response=response[-3:]), self.assertRaisesRegex(
                    setup.SetupError, "256 Bytes"):
                self.run_client(port)
            self.assertTrue(port.closed)

    def test_public_status_fields_cannot_echo_secret_like_tokens(self) -> None:
        for changed in [STATUS.replace(b"firmware=f4575aa", b"firmware=SYNTHETIC_TEST_SECRET"),
                        STATUS.replace(b"build=abc123", b"build=SYNTHETIC_TEST_SECRET")]:
            port = FakeSerial(statuses=[changed])
            output: list[str] = []
            with self.assertRaises(setup.SetupError) as caught:
                setup.run_observation(args(), opener=lambda _: port, startup=lambda _: None,
                                      command=commands("quit"), clock=Clock(), emit=output.append)
            self.assertEqual([], output)
            self.assertNotIn("SYNTHETIC_TEST_SECRET", str(caught.exception))
            self.assertTrue(port.closed)

    def test_bounded_input_output_and_idle_timeout(self) -> None:
        for name, attribute, limit in [
            ("input", "input_bytes", setup.MAX_SESSION_INPUT_BYTES),
            ("lines", "lines", setup.MAX_SESSION_LINES),
            ("output", "output_lines", setup.MAX_SESSION_OUTPUT_LINES),
            ("output bytes", "output_bytes", setup.MAX_SESSION_OUTPUT_BYTES),
        ]:
            port = FakeSerial(statuses=[STATUS])
            session = setup.ObservationSession(port, clock=Clock(), emit=lambda _: None,
                                               duration_seconds=30)
            setattr(session, attribute, limit)
            with self.subTest(name=name), self.assertRaises(setup.SetupError):
                if attribute.startswith("output"):
                    session._emit("Messung uptime_ms=0")
                else:
                    port.lines.append(metrics())
                    session.idle_read()
        silent = FakeSerial(statuses=[STATUS])
        session = setup.ObservationSession(silent, clock=Clock(step=0.1),
                                           emit=lambda _: None, duration_seconds=1800)
        session.last_measure_time = session.clock()
        with self.assertRaisesRegex(setup.SetupError, "30 Sekunden"):
            for _ in range(1000):
                session.idle_read()

    def test_long_numeric_metric_cannot_exceed_output_byte_budget(self) -> None:
        port = FakeSerial(statuses=[STATUS], idle=[metrics(long=True)])
        emitted: list[str] = []
        session = setup.ObservationSession(port, clock=Clock(), emit=emitted.append,
                                           duration_seconds=30)
        session.identity_checked = True
        session.output_bytes = setup.MAX_SESSION_OUTPUT_BYTES - 10
        with self.assertRaisesRegex(setup.SetupError, "Sitzungsausgabe"):
            session.idle_read()
        self.assertEqual([], emitted)

    def test_reopen_requires_new_status_device_gate(self) -> None:
        first = FakeSerial(statuses=[STATUS])
        self.run_client(first)
        wrong = FakeSerial(statuses=[STATUS])
        with self.assertRaisesRegex(setup.SetupError, "Gerätekennung"):
            self.run_client(wrong, device="0" * 12)
        self.assertEqual([b"KM1 STATUS\n"], wrong.writes)
        self.assertTrue(wrong.closed)

    def test_explicit_firmware_gate_checks_initial_and_later_status(self) -> None:
        wrong_first = FakeSerial(statuses=[STATUS.replace(b"f4575aa", b"deadbee0")])
        with self.assertRaisesRegex(setup.SetupError, "Firmwarekennung"):
            self.run_client(wrong_first)
        self.assertEqual([b"KM1 STATUS\n"], wrong_first.writes)
        changed = FakeSerial(statuses=[STATUS, STATUS.replace(b"f4575aa", b"deadbee0")])
        with self.assertRaisesRegex(setup.SetupError, "Firmwarekennung"):
            self.run_client(changed, ("status", "quit"))
        self.assertEqual([b"KM1 STATUS\n", b"KM1 STATUS\n"], changed.writes)
        missing = args()
        del missing.expect_firmware
        with self.assertRaisesRegex(setup.SetupError, "expect-firmware"):
            setup.run_observation(missing, opener=lambda _: wrong_first,
                                  startup=lambda _: None, command=commands("quit"),
                                  clock=Clock(), emit=lambda _: None)

    def test_unknown_logs_cannot_mask_diagnostic_silence(self) -> None:
        port = FakeSerial(statuses=[STATUS],
                          after_status=[b"I (123) irrelevant: 1\n"] * 400)
        with self.assertRaisesRegex(setup.SetupError, "Diagnose"):
            self.run_client(port, tuple([None] * 400), clock=Clock(step=0.1), duration=60)

    def test_uptime_regression_and_uint64_overflow(self) -> None:
        def with_uptime(uptime: int) -> bytes:
            return metrics().replace(b"uptime_ms=0", f"uptime_ms={uptime}".encode(), 1)
        port = FakeSerial(statuses=[STATUS], after_status=[with_uptime(2000), with_uptime(1000)])
        with self.assertRaisesRegex(setup.SetupError, "Neustart|Monoton"):
            self.run_client(port, (None, None, "quit"))
        overflow = with_uptime(1 << 64)
        self.assertIsNone(setup._metric_line(overflow[:-1]))


class NetEvidenceTests(unittest.TestCase):
    def test_direct_network_schema_and_rejections(self):
        raw = (b"I (123) katmed_net: schema=1 sample_seq=2 uptime_ms=100 "
               b"boot_domain=77 sta_v=1 sta_up=1 sta_ip=0 sta_dhcp=1")
        kind, fields = setup._evidence_line(raw)
        self.assertEqual(kind, "net")
        self.assertEqual(fields["sta_dhcp"], 1)
        self.assertLess(len(raw), setup.MAX_DIAG_CHARS)
        for old, new in ((b"sta_dhcp=1", b"sta_dhcp=3"),
                         (b"sta_up=1", b"sta_up=2"),
                         (b"sta_v=1", b"sta_v=0"),
                         (b"boot_domain=77", b"boot_domain=0"),
                         (b"schema=1", b"schema=2"),
                         (b"sta_ip=0", b"sta_ip=0 ip=private")):
            self.assertIsNone(setup._evidence_line(raw.replace(old, new)))
        invalid = raw.replace(b"sta_v=1", b"sta_v=0").replace(
            b"sta_up=1", b"sta_up=0").replace(b"sta_dhcp=1", b"sta_dhcp=0")
        self.assertIsNotNone(setup._evidence_line(invalid))


class DhcpEvidenceTests(unittest.TestCase):
    def test_actual_main_formatter_maximum_and_rejections(self):
        import re
        text = (Path(__file__).resolve().parents[2] / "firmware/main/main.c").read_text()
        part = text.split('ESP_LOGI("katmed_dhcp",', 1)[1].split(
            "sequence,now.monotonic_ms,runtime.boot_domain,", 1)[0]
        tokens = re.findall(r'"([^"\n]*)"|(PRIu64|PRIu32|PRId32)', part)
        fmt = "".join(literal if not macro else ("d" if macro == "PRId32" else "u")
                      for literal, macro in tokens)
        fields = {key: 0xffffffff for key in setup.DHCP_KEYS}
        fields.update(schema=1, sample_seq=(1 << 64)-1, uptime_ms=(1 << 64)-1,
                      boot_domain=(1 << 64)-1, core_v=1, dhcp_v=1, link_up=1,
                      state=12, tries=255, fine=1, udp_error=-(1 << 31), tx_error=-(1 << 31))
        raw = ("I (123) katmed_dhcp: " + fmt % tuple(
            fields[k] for k in setup.DHCP_KEYS[1:])).encode()
        self.assertLessEqual(len(raw), setup.MAX_DIAG_CHARS)
        self.assertEqual(setup._evidence_line(raw), ("dhcp", fields))
        for old, new in ((b"state=12", b"state=13"),
                         (b"tries=255", b"tries=256"),
                         (b"core_v=1", b"core_v=0"),
                         (b"dhcp_v=1", b"dhcp_v=0"),
                         (b"fine=1", b"fine=2")):
            self.assertIsNone(setup._evidence_line(raw.replace(old, new)))
        self.assertIsNone(setup._evidence_line(raw + b" unknown=1"))


if __name__ == "__main__":
    unittest.main()
