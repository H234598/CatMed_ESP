#!/usr/bin/env python3
"""Independent KM1 line-ending checks against the actual host parser."""

from pathlib import Path
import sys
from unittest import TestCase

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import katmed_usb_setup as setup


DEVICE = "ac276e46bc2c"
LF = (
    f"KM1 OK STATUS configured=0 durable=1 pending=0 clock=0 "
    f"device={DEVICE} catalog={'a' * 64} firmware=test build=abcdef123 error=OK\n"
).encode("ascii")
REQUEST = b"KM1 STATUS\n"
CHECK = TestCase()


class Port:
    def __init__(self, reply: bytes, *, buffer: bytes = b""):
        self.reply = reply
        self.buffer = buffer
        self.closed = False
        self.writes: list[bytes] = []
        self.read_limits: list[tuple[bytes, int | None]] = []
        self.flushes = 0

    def _require_open(self) -> None:
        if self.closed:
            raise OSError("fixture port is closed")

    def read(self, size: int = 1) -> bytes:
        self._require_open()
        size = max(0, size)
        data, self.buffer = self.buffer[:size], self.buffer[size:]
        return data

    def reset_input_buffer(self) -> None:
        self._require_open()
        self.buffer = b""

    def write(self, data: bytes) -> int:
        self._require_open()
        self.writes.append(data)
        if data != REQUEST:
            raise OSError("unexpected fixture request")
        self.buffer += self.reply
        return len(data)

    def flush(self) -> None:
        self._require_open()
        self.flushes += 1

    def read_until(self, expected: bytes = b"\n", size: int | None = None) -> bytes:
        self._require_open()
        self.read_limits.append((expected, size))
        end = self.buffer.find(expected)
        length = len(self.buffer) if end < 0 else end + len(expected)
        return self.read(length if size is None else min(length, size))

    def close(self) -> None:
        self.closed = True


def check_exchange(port: Port) -> None:
    CHECK.assertTrue(port.closed)
    CHECK.assertEqual(port.writes, [REQUEST])
    CHECK.assertEqual(port.flushes, 1)
    CHECK.assertEqual(port.read_limits, [(b"\n", setup.MAX_DIAG_CHARS + 2)])


def check_rejection(reply: bytes, reason: str) -> None:
    port = Port(reply)
    try:
        with CHECK.assertRaises(setup.SetupError) as failure:
            setup.read_status(port)
    finally:
        port.close()
    CHECK.assertEqual(str(failure.exception), reason)
    check_exchange(port)


def check_buffer() -> None:
    port = Port(LF, buffer=b"abc\ntail")
    try:
        CHECK.assertEqual(port.read_until(size=2), b"ab")
        CHECK.assertEqual(port.read_until(), b"c\n")
        CHECK.assertEqual(port.read(), b"t")
        port.reset_input_buffer()
        CHECK.assertEqual(port.read(10), b"")
        CHECK.assertEqual(port.write(REQUEST), len(REQUEST))
        CHECK.assertEqual(port.read_until(), LF)
    finally:
        port.close()
    for operation in (port.read, port.read_until, port.reset_input_buffer, port.flush,
                      lambda: port.write(REQUEST)):
        with CHECK.assertRaisesRegex(OSError, "fixture port is closed"):
            operation()


def main() -> None:
    crlf = Port(LF[:-1] + b"\r\n")
    try:
        CHECK.assertEqual(setup.read_status(crlf)["device"], DEVICE)
    finally:
        crlf.close()
    check_exchange(crlf)

    for malformed in (
        LF.replace(b"configured=0", b"configured=\r0"),
        LF[:-1] + b"\r\r\n",
        LF.replace(b"catalog=", b"catalog=\r"),
    ):
        check_rejection(malformed, setup.USB_LINE_END_ERROR)
    print("terminal CRLF accepted; embedded and doubled CR rejected")
    check_rejection(LF[:-1], setup.REPLY_FRAME_ERROR)
    oversized = LF[:-1].ljust(setup.MAX_RESPONSE_BYTES, b" ") + b"\n"
    check_rejection(oversized, setup.REPLY_LENGTH_ERROR)
    check_buffer()
    print("reply bounds and serial fixture lifecycle verified")


if __name__ == "__main__":
    main()
