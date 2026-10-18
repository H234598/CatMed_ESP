"""Independent, port-free checks through firmware formatter and real recorder.

The C fixture calls the production formatter.  Its serial bytes then enter the
unchanged KM1 status parser and the production 48-hour recorder.  Time is
controlled; this is no physical duration or device measurement.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import katmed_longrun as longrun  # noqa: E402
import katmed_usb_setup as usb  # noqa: E402

DEVICE = "ac276e46bc2c"
FIRMWARE = "abc1234"
BUILD = "abc123"
SECRET = "SYNTHETIC_SECRET_NOT_FOR_SERIAL"

C_SOURCE = r'''
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime_metrics.h"

void test_log_info(const char *tag, const char *format, ...) {
    printf("I (4294967295) %s: ", tag);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
}

int main(int argc, char **argv) {
    int count = argc > 1 ? atoi(argv[1]) : 4;
    int changing = argc > 2 ? atoi(argv[2]) : 0;
    for (int i = 0; i < count; ++i) {
        uint64_t uptime = UINT64_C(4300000000) + (uint64_t)i * 10000;
        KatmedRuntimeMetricInput m = {.uptime_ms=uptime};
        KatmedRuntimeEvidenceState s = {
            .seq=(uint64_t)i + 1,.uptime_ms=uptime,.boot_domain=7,
            .round_device=11,.round_epoch=1,.round_sequence=changing == 1 ? (uint64_t)i + 2 : (i < 2 ? 2 : 3),
            .round_ordinal=1,.origin_device=11,.origin_epoch=1,.origin_sequence=2,
            .start_domain=99,.start_ms=50000,.clock_domain=7,
            .regular_ms=changing == 2 ? 200000 + (uint64_t)i * 10000 : 200000,
            .effective_ms=changing == 1 ? 200000 + (uint64_t)i * 10000 : (changing == 2 ? 250000 : (i < 2 ? 200000 : 250000)),
            .deadline_local=1,.alarm=i < 2,.reason=1,
            .now_utc_valid=i >= 2,.now_utc_ms=i >= 2 ? INT64_C(1791973800000) + (int64_t)i * 10000 : 0,
            .now_quality=i >= 2 ? 1 : 0,.utc_source=i >= 2 ? 2 : 0,
            .image_id=3,.image_remaining_ms=50000
        };
        KatmedRuntimeEvidenceHealth h = {
            .seq=s.seq,.uptime_ms=uptime,.boot_domain=7,
            .image_valid=1,.image_failures=i >= 2 ? 1 : 0,
            .store_valid=1,.store_commit_failures=i >= 3 ? 1 : 0,
            .mesh_valid=1,.mesh_session_up=i >= 2 ? 1 : 0,
            .mesh_connected=i >= 2
        };
        katmed_runtime_metrics_log(&m);
        katmed_runtime_evidence_log(&s,&h);
    }
    return 0;
}
'''


class Clock:
    def __init__(self) -> None:
        self.now = 0.0

    def mono(self) -> float:
        return self.now

    def wall(self) -> float:
        return 1_800_000_000.0 + self.now


class FakePort:
    def __init__(self, clock: Clock, samples: list[list[bytes]]) -> None:
        self.clock = clock
        self.schedule = [((10 if index else 0) if part == 0 else 0, line)
                         for index, sample in enumerate(samples)
                         for part, line in enumerate(sample)]
        self.pending: list[bytes] = []
        self.writes: list[bytes] = []
        self.closed = False

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        if data == b"KM1 STATUS\n":
            self.pending.append((
                "KM1 OK STATUS configured=1 durable=1 pending=0 clock=2 "
                f"device={DEVICE} catalog={'a' * 64} firmware={FIRMWARE} "
                f"build={BUILD} error=OK\n"
            ).encode("ascii"))
        return len(data)

    def flush(self) -> None:
        pass

    def read_until(self, expected=b"\n", size=None) -> bytes:
        if self.pending:
            return self.pending.pop(0)
        if self.schedule:
            delay, line = self.schedule.pop(0)
            self.clock.now += delay
            return line
        self.clock.now += 1
        return b""

    def close(self) -> None:
        self.closed = True


class MetricsReview(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.build_dir = tempfile.TemporaryDirectory(prefix="katmed-metrics-review-")
        source = Path(cls.build_dir.name) / "fixture.c"
        source.write_text(C_SOURCE)
        cls.program = Path(cls.build_dir.name) / "fixture"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            f"-I{ROOT / 'tests/host_runtime_metrics/stubs'}",
            f"-I{ROOT / 'firmware/main'}",
            str(ROOT / "firmware/main/runtime_metrics.c"), str(source),
            "-o", str(cls.program),
        ], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.build_dir.cleanup()

    def samples(self, count=4, changing=0) -> list[list[bytes]]:
        raw = subprocess.check_output([str(self.program), str(count), str(changing)])
        lines = raw.splitlines(keepends=True)
        self.assertEqual(count * 3, len(lines))
        return [lines[index:index + 3] for index in range(0, len(lines), 3)]

    def run_recording(self, samples: list[list[bytes]], root: Path,
                      *, duration=30) -> tuple[dict, FakePort]:
        clock = Clock()
        port = FakePort(clock, samples)
        args = argparse.Namespace(port="/dev/serial/by-id/synthetic", expect_device=DEVICE,
                                  expect_firmware=FIRMWARE, expect_build=BUILD,
                                  evidence_dir=str(root), duration_seconds=duration)
        result = longrun.run_longrun(args, opener=lambda _: port, startup=lambda _: None,
                                     monotonic=clock.mono, wall=clock.wall,
                                     emit=lambda _: None)
        return result, port

    def test_product_formatter_parser_and_recorder_keep_round_and_error_changes(self) -> None:
        samples = self.samples()
        for sample in samples:
            for line in sample:
                self.assertLessEqual(len(line.rstrip(b"\r\n")), 767)
                self.assertNotIn(SECRET.encode(), line)
            self.assertIsNotNone(usb._metric_line(sample[0].rstrip(b"\n")))
            self.assertEqual("state", usb._evidence_line(sample[1].rstrip(b"\n"))[0])
            self.assertEqual("health", usb._evidence_line(sample[2].rstrip(b"\n"))[0])
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / "run"
            result, port = self.run_recording(samples, directory)
            self.assertTrue(port.closed)
            self.assertEqual([b"KM1 STATUS\n"], port.writes)
            self.assertEqual("complete", result["state"])
            self.assertEqual("complete", result["evidence_coverage"])
            self.assertEqual(4, result["evidence_samples"])
            state = result["evidence_last_state"]
            self.assertEqual((3, 250000, 99, 7, 1, 2),
                             (state["round_s"], state["due_e"], state["start_d"],
                              state["boot_domain"], state["now_v"], state["utc_source"]))
            health = result["evidence_last_health"]
            self.assertEqual((1, 1, 1),
                             (health["image_fail"], health["store_commit"], health["mesh_up"]))
            journal = (directory / "changes.jsonl").read_bytes()
            self.assertEqual(hashlib.sha256(journal).hexdigest(), result["change_sha256"])
            self.assertTrue(any(b'"s"' in line and b'250000' in line for line in journal.splitlines()))
            self.assertTrue(any(b'"h"' in line and b'1' in line for line in journal.splitlines()))
            self.assertLessEqual(len(journal), 64 * 1024)
            self.assertLessEqual((directory / "summary.json").stat().st_size, 8192)
            self.assertNotIn(SECRET.encode(), journal + (directory / "summary.json").read_bytes())

    def test_mismatched_or_missing_product_pair_cannot_be_complete(self) -> None:
        samples = self.samples()
        samples[1][2] = samples[1][2].replace(b"sample_seq=2", b"sample_seq=3")
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(longrun.LongrunError, "verschiedene Sample-IDs"):
                self.run_recording(samples, Path(tmp) / "mismatch")
        samples = self.samples()
        samples[1].pop()
        with tempfile.TemporaryDirectory() as tmp:
            result, _ = self.run_recording(samples, Path(tmp) / "missing")
            self.assertEqual("incomplete", result["evidence_coverage"])
            self.assertEqual(1, result["evidence_missing_pairs"])

    def test_historical_regular_deadline_survives_recording(self) -> None:
        # When the night rule keeps the effective deadline fixed, the regular
        # monotonic deadline can still change. The last-state summary cannot
        # explain an earlier round; its own change journal must retain it.
        samples = self.samples(4, changing=2)
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / "run"
            result, _ = self.run_recording(samples, directory)
            self.assertEqual("complete", result["evidence_coverage"])
            self.assertEqual(230000, result["evidence_last_state"]["due_r"])
            journal = (directory / "changes.jsonl").read_bytes()
            self.assertIn(b"210000", journal,
                          "the prior regular deadline vanished from the bounded timeline")
            self.assertIn(b"220000", journal)

    def test_legacy_and_missing_health_source_are_explicitly_incomplete(self) -> None:
        samples = self.samples()
        with tempfile.TemporaryDirectory() as tmp:
            result, _ = self.run_recording([[sample[0]] for sample in samples], Path(tmp) / "old")
            self.assertEqual("legacy_missing", result["evidence_coverage"])
        samples = self.samples()
        samples[1][2] = samples[1][2].replace(b"store_v=1", b"store_v=0")
        with tempfile.TemporaryDirectory() as tmp:
            result, _ = self.run_recording(samples, Path(tmp) / "unknown")
            self.assertEqual("incomplete", result["evidence_coverage"])
            self.assertEqual(1, result["evidence_unavailable_samples"])

    def test_257th_change_fails_closed_without_discarding_older_entries(self) -> None:
        samples = self.samples(130, changing=1)
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / "run"
            with self.assertRaisesRegex(longrun.LongrunError, "Ereignisjournal überschreitet"):
                self.run_recording(samples, directory, duration=172800)
            summary = json.loads((directory / "summary.json").read_text())
            journal = (directory / "changes.jsonl").read_bytes()
            self.assertEqual("failed", summary["state"])
            self.assertEqual(256, summary["change_entries"])
            self.assertEqual(256, len(journal.splitlines()))
            self.assertLessEqual(len(journal), 64 * 1024)
            self.assertEqual(hashlib.sha256(journal).hexdigest(), summary["change_sha256"])

    def test_secret_and_oversize_lines_fail_closed(self) -> None:
        samples = self.samples()
        samples[1][1] = samples[1][1].rstrip(b"\n") + b" " + SECRET.encode() + b"\n"
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(longrun.LongrunError, "beschädigt") as failure:
                self.run_recording(samples, Path(tmp) / "poison")
            self.assertNotIn(SECRET, str(failure.exception))
            self.assertNotIn(SECRET.encode(), (Path(tmp) / "poison/summary.json").read_bytes())
        samples = self.samples()
        samples[1][1] = b"I (123) katmed_state: " + b"0" * 746 + b"\n"
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(longrun.LongrunError, "ungültig|zu lang"):
                self.run_recording(samples, Path(tmp) / "oversize")


if __name__ == "__main__":
    unittest.main()
