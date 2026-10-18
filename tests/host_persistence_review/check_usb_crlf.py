#!/usr/bin/env python3
"""Independent KM1 line-ending checks against the actual host parser."""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import katmed_usb_setup as setup


DEVICE = "ac276e46bc2c"
LF = (
    f"KM1 OK STATUS configured=0 durable=1 pending=0 clock=0 "
    f"device={DEVICE} catalog={'a' * 64} firmware=test build=abcdef123 error=OK\n"
).encode("ascii")


class Port:
    def __init__(self, line: bytes):
        self.line = line
        self.writes: list[bytes] = []

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        return len(data)

    def flush(self) -> None:
        pass

    def read_until(self, expected: bytes = b"\n", size: int | None = None) -> bytes:
        assert expected == b"\n" and size == setup.MAX_RESPONSE_BYTES + 1
        return self.line


def main() -> None:
    crlf = Port(LF[:-1] + b"\r\n")
    assert setup.read_status(crlf)["device"] == DEVICE
    assert crlf.writes == [b"KM1 STATUS\n"]

    for malformed in (
        LF.replace(b"configured=0", b"configured=\r0"),
        LF[:-1] + b"\r\r\n",
        LF.replace(b"catalog=", b"catalog=\r"),
    ):
        port = Port(malformed)
        try:
            setup.read_status(port)
        except setup.SetupError:
            pass
        else:
            raise AssertionError("malformed CR accepted")
        assert port.writes == [b"KM1 STATUS\n"]
    print("terminal CRLF accepted; embedded and doubled CR rejected")


if __name__ == "__main__":
    main()
