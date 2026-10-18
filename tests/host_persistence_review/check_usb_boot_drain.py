#!/usr/bin/env python3
"""Independent, bounded reboot-output repro for the real KM1 host client."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import katmed_usb_setup as setup


DEVICE = "ac276e46bc2c"
CATALOG = "a" * 64
STATUS = (
    f"KM1 OK STATUS configured=0 durable=1 pending=0 clock=0 "
    f"device={DEVICE} catalog={CATALOG} firmware=test build=abcdef123 error=OK\n"
).encode("ascii")


class Clock:
    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        self.now += 0.05
        return self.now


class Port:
    def __init__(self, boot: bytes, fail_flush: bool = False) -> None:
        self.boot = boot
        self.position = 0
        self.drained = False
        self.fail_flush = fail_flush
        self.read_sizes: list[int] = []
        self.writes: list[bytes] = []
        self.closed = False

    def read(self, size: int = 1) -> bytes:
        assert 0 < size <= setup.STARTUP_CHUNK_BYTES
        self.read_sizes.append(size)
        end = min(self.position + size, len(self.boot))
        chunk = self.boot[self.position:end]
        self.position = end
        return chunk

    def reset_input_buffer(self) -> None:
        if self.fail_flush:
            raise RuntimeError("SYNTHETIC_TEST_SECRET")
        self.drained = True
        self.position = len(self.boot)

    def write(self, data: bytes) -> int:
        assert self.drained, "request sent before startup drain"
        self.writes.append(data)
        return len(data)

    def flush(self) -> None:
        pass

    def read_until(self, expected: bytes = b"\n", size: int | None = None) -> bytes:
        assert self.writes == [b"KM1 STATUS\n"]
        assert expected == b"\n" and size == setup.MAX_RESPONSE_BYTES + 1
        return STATUS

    def close(self) -> None:
        self.closed = True


def args() -> argparse.Namespace:
    return argparse.Namespace(command="status", port="/fake/by-id/C3",
                              expect_device=DEVICE, config=None, confirm_reset=None)


def startup(port: Port) -> None:
    setup._drain_startup(port, Clock())


def main() -> None:
    forged = STATUS + b"SYNTHETIC_TEST_SECRET\n"
    boot = forged + b"B" * (3204 - len(forged))
    port = Port(boot)
    result = setup.run_command(args(), lambda _: port, startup)
    assert port.closed and port.drained and port.position == 3204
    assert port.writes == [b"KM1 STATUS\n"]
    assert f"device={DEVICE}" in result
    assert "SYNTHETIC_TEST_SECRET" not in result

    huge = Port(b"X" * (setup.MAX_STARTUP_BYTES + 1))
    try:
        setup.run_command(args(), lambda _: huge, startup)
    except setup.SetupError as error:
        assert "Startausgabe" in str(error)
    else:
        raise AssertionError("oversize startup was accepted")
    assert huge.closed and not huge.writes

    broken = Port(b"boot\n", fail_flush=True)
    try:
        setup.run_command(args(), lambda _: broken, startup)
    except setup.SetupError as error:
        assert "SYNTHETIC_TEST_SECRET" not in str(error)
    else:
        raise AssertionError("failed drain was accepted")
    assert broken.closed and not broken.writes
    print("bounded startup drain and sanitized failure passed")


if __name__ == "__main__":
    main()
