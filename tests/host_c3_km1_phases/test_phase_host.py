"""Check the real C formatter output through the production host parser."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import katmed_usb_setup as usb
import katmed_longrun as longrun
sys.path.insert(0, str(ROOT / "tests" / "host_longrun"))
from test_longrun import Clock, FakeSerial, continuous, DEVICE, FIRMWARE, BUILD


def recorder(directory: Path) -> longrun.Recorder:
    args = argparse.Namespace(expect_device="ac276e46bc2c",
                              expect_firmware="5042056",
                              expect_build="6c9d77e5b", duration_seconds=30)
    return longrun.Recorder(args, None, directory, lambda: 1.0, lambda: 1.0)


def main() -> None:
    product = Path(sys.argv[1]).read_text(encoding="ascii")
    status_line = re.search(r"^PRODUCT_STATUS=(.+)$", product, re.MULTILINE)
    assert status_line is not None
    class StatusPort:
        def write(self, data: bytes) -> int:
            assert data == b"KM1 STATUS\n"
            return len(data)
        def flush(self) -> None:
            pass
    status = usb.read_status(StatusPort(), lambda _port: status_line.group(1))
    assert len(status) == 9 and status["firmware"] == "5042056"
    assert status["build"] == "6c9d77e5b"
    match = re.search(r"^PRODUCT_PHASE=(.+)$", product, re.MULTILINE)
    assert match is not None, product
    line = match.group(1).encode("ascii")
    phase = usb._usb_phase_line(line)
    assert phase is not None and phase["schema"] == 1
    assert phase["boot_domain"] == 391 and phase["task"] == 1
    assert phase["read"] == 1 and phase["status"] == 1
    assert phase["queue_ok"] == phase["main_take"] == 1
    assert phase["reply_ready"] == phase["reply_call"] == 1
    assert phase["queue_drop"] == 0
    assert len(line) <= usb.MAX_DIAG_CHARS
    resources = re.search(r"MAX_PHASE_CHARS=([0-9]+) SNAPSHOT_BYTES=([0-9]+)", product)
    assert resources is not None
    max_chars, snapshot_bytes = map(int, resources.groups())
    assert max_chars <= 767 and 0 < snapshot_bytes <= 128

    with tempfile.TemporaryDirectory(prefix="katmed-km1-phase-host-") as path:
        private = Path(path)
        first = private / "pre"
        first.mkdir(mode=0o700)
        rec = recorder(first)
        rec._metric(line)  # No STATUS: only an expressly unverified private value.
        assert rec.summary["phase_coverage"] == "prestatus_unverified"
        assert rec.summary["phase_prestatus_unverified"] == phase
        assert rec.summary["phase_last_verified"] is None
        rec.fail("status_timeout")
        stored = json.loads((first / "summary.json").read_text("ascii"))
        assert stored["state"] == "failed" and stored["phase_last_verified"] is None
        assert "KM1 STATUS" not in json.dumps(stored)

        second = private / "post"
        second.mkdir(mode=0o700)
        rec2 = recorder(second)
        rec2.last_status_mono = 1.0
        rec2._metric(line)
        assert rec2.summary["phase_coverage"] == "poststatus_unverified"
        assert rec2.summary["phase_last_verified"] is None
        metric_values = dict.fromkeys(usb.METRIC_KEYS, 0)
        metric_values["uptime_ms"] = 100_000
        metric = ("I (123) katmed_metrics: " +
                  " ".join(f"{key}={metric_values[key]}" for key in usb.METRIC_KEYS)).encode()
        state_values = dict.fromkeys(usb.STATE_KEYS, 0)
        state_values.update(schema=1, sample_seq=1, uptime_ms=100_000,
                            boot_domain=phase["boot_domain"])
        state = ("I (123) katmed_state: " +
                 " ".join(f"{key}={state_values[key]}" for key in usb.STATE_KEYS)).encode()
        health_values = dict.fromkeys(usb.HEALTH_KEYS, 0)
        health_values.update(schema=1, sample_seq=1, uptime_ms=100_000,
                             boot_domain=phase["boot_domain"],
                             image_v=1, store_v=1, mesh_v=1)
        health = ("I (123) katmed_health: " +
                  " ".join(f"{key}={health_values[key]}" for key in usb.HEALTH_KEYS)).encode()
        for body in (metric, state, health):
            rec2._metric(body)
        assert rec2.summary["phase_coverage"] == "verified_sample"
        newer = re.sub(rb"phase_seq=[0-9]+", b"phase_seq=1000", line)
        rec2._metric(newer)
        assert rec2.summary["phase_coverage"] == "verified_sample"

        third = private / "full"
        third.mkdir(mode=0o700)
        rec3 = recorder(third)
        exhausted = re.sub(rb"phase_seq=[0-9]+", b"phase_seq=18446744073709551615", line)
        try:
            rec3._metric(exhausted)
        except longrun.LongrunError as error:
            assert "erschöpft" in str(error)
        else:
            raise AssertionError("saturated sequence was accepted")
        assert rec3.summary["phase_coverage"] == "exhausted"

        fourth = private / "rollback"
        fourth.mkdir(mode=0o700)
        rec4 = recorder(fourth)
        rec4.last_status_mono = 1.0
        rec4.evidence_domain = phase["boot_domain"]
        first_values = dict(phase, phase_seq=8, status=2)
        def phase_line(values: dict[str, int]) -> bytes:
            return ("I (4294967295) katmed_usb_phase: " +
                    " ".join(f"{key}={values[key]}" for key in usb.USB_PHASE_KEYS)).encode()
        rec4._metric(phase_line(first_values))
        assert rec4.summary["phase_last_verified"]["status"] == 2
        rec4._metric(phase_line(dict(first_values, phase_seq=7, status=1)))
        assert rec4.summary["phase_last_verified"]["phase_seq"] == 8
        try:
            rec4._metric(phase_line(dict(first_values, phase_seq=9, status=1)))
        except longrun.LongrunError as error:
            assert "sprang" in str(error)
        else:
            raise AssertionError("newer cumulative rollback was accepted")

    clock = Clock()
    with tempfile.TemporaryDirectory(prefix="katmed-km1-phase-legacy-") as path:
        old_port = FakeSerial(clock, schedule=continuous(seconds=30))
        args = argparse.Namespace(port="/dev/serial/by-id/fake", expect_device=DEVICE,
                                  expect_firmware=FIRMWARE, expect_build=BUILD,
                                  evidence_dir=str(Path(path) / "run"), duration_seconds=30)
        old = longrun.run_longrun(args, opener=lambda _: old_port,
                                  startup=lambda _: None, monotonic=clock.mono,
                                  wall=clock.wall, emit=lambda _: None)
        assert old["state"] == "complete"  # Existing time/uptime meaning.
        assert old["phase_coverage"] == "legacy_missing"
        assert old["completion_qualification"] == "time_uptime_phase_missing"
        assert old["evidence_coverage"] == "legacy_missing"

    assert usb._usb_phase_line(line.replace(b"queue_ok=1", b"queue_ok=2")) is None
    assert usb._usb_phase_line(line.replace(b"task=1", b"task=3")) is None
    assert usb._usb_phase_line(line.replace(b"status=1", b"status=PRIVATE")) is None
    assert usb._usb_phase_line(line + b" raw_payload") is None
    assert usb._usb_phase_line(line + b"x" * 768) is None
    assert usb._usb_phase_line(b"KM1 OK STATUS " + line) is None
    print(f"km1_phase_product_path_and_host=PASS "
          f"max_phase_chars={max_chars} snapshot_bytes={snapshot_bytes}")


if __name__ == "__main__":
    main()
