"""Bounded recorder checks through the real USB numeric parser."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests" / "host_longrun"))
import katmed_usb_setup as usb
import katmed_longrun as longrun
from test_longrun import Clock, FakeSerial, DEVICE, FIRMWARE, BUILD, metric


def state(uptime: int, seq: int, *, domain=7, round_sequence=2, due=200_000,
          effective_due=None) -> bytes:
    values = dict.fromkeys(usb.STATE_KEYS, 0)
    values.update(schema=1, sample_seq=seq, uptime_ms=uptime, boot_domain=domain,
                  round_d=11, round_e=1, round_s=round_sequence, round_o=1,
                  origin_d=11, origin_e=1, origin_s=2, start_d=domain,
                  start_ms=100_000, clock_d=domain, due_r=due,
                  due_e=due if effective_due is None else effective_due,
                  due_local=1, now_q=0, image_id=3,
                  image_rem=50_000)
    return ("I (123) katmed_state: " +
            " ".join(f"{k}={values[k]}" for k in usb.STATE_KEYS) + "\n").encode()


def health(uptime: int, seq: int, *, domain=7, image_fail=0) -> bytes:
    values = dict.fromkeys(usb.HEALTH_KEYS, 0)
    values.update(schema=1, sample_seq=seq, uptime_ms=uptime, boot_domain=domain,
                  image_v=1, image_fail=image_fail, store_v=1, mesh_v=1)
    return ("I (123) katmed_health: " +
            " ".join(f"{k}={values[k]}" for k in usb.HEALTH_KEYS) + "\n").encode()


def schedule(*, mismatched_health=False, missing_health=False, rollback=False):
    result = []
    for i in range(4):
        uptime = 100_000 + i * 10_000
        result.extend([(0 if i == 0 else 10, metric(uptime)),
                       (0, state(uptime, i + 1,
                                 round_sequence=2 if i < 2 else 3,
                                 due=200_000 if i < 2 else 250_000))])
        if not (missing_health and i == 1):
            result.append((0, health(uptime + (1 if mismatched_health and i == 1 else 0),
                                     i + 1, image_fail=(0 if i == 0 else
                                                       (0 if rollback and i == 2 else 1)))))
    return result


class EvidenceTests(unittest.TestCase):
    def invoke(self, schedule_lines, directory: Path, *, duration=30):
        clock = Clock()
        port = FakeSerial(clock, schedule=schedule_lines)
        args = argparse.Namespace(port="/dev/serial/by-id/fake", expect_device=DEVICE,
                                  expect_firmware=FIRMWARE, expect_build=BUILD,
                                  evidence_dir=str(directory), duration_seconds=duration)
        result = longrun.run_longrun(args, opener=lambda _: port,
                                     startup=lambda _: None, monotonic=clock.mono,
                                     wall=clock.wall, emit=lambda _: None)
        self.assertTrue(port.closed)
        return result

    def test_strict_fixed_numeric_parser(self):
        parsed = usb._evidence_line(state(100_000, 1).rstrip(b"\n"))
        self.assertIsNotNone(parsed)
        self.assertEqual(("state", 7), (parsed[0], parsed[1]["boot_domain"]))
        self.assertIsNone(usb._evidence_line(state(100_000, 1).replace(
            b"round_d=11", b"round_d=11 secret=hidden").rstrip(b"\n")))
        self.assertIsNone(usb._evidence_line(state(100_000, 1).replace(
            b"schema=1", b"schema=2").rstrip(b"\n")))
        self.assertIsNone(usb._evidence_line(state(100_000, 1).replace(
            b"due_local=1", b"due_local=2").rstrip(b"\n")))
        self.assertIsNone(usb._evidence_line(b"I (123) katmed_state: " + b"x" * 768))
        foreign_start = state(100_000, 1).replace(b"start_d=7", b"start_d=99")
        self.assertEqual(99, usb._evidence_line(foreign_start.rstrip(b"\n"))[1]["start_d"])
        foreign_deadline = (foreign_start.replace(b"clock_d=7", b"clock_d=99")
                            .replace(b"due_local=1", b"due_local=0"))
        self.assertEqual(0, usb._evidence_line(foreign_deadline.rstrip(b"\n"))[1]["due_local"])
        self.assertIsNone(usb._evidence_line(foreign_start.replace(
            b"clock_d=7", b"clock_d=99").rstrip(b"\n")))

    def test_pair_journal_and_no_effect_on_legacy_line(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root) / "run"
            result = self.invoke(schedule(), directory)
            self.assertEqual("complete", result["state"])
            self.assertEqual("complete", result["evidence_coverage"])
            self.assertEqual("unknown", result["ack_evidence"])
            self.assertEqual(4, result["evidence_samples"])
            self.assertEqual(0, result["evidence_missing_pairs"])
            self.assertEqual(4, result["metric_count"])
            self.assertEqual(3, result["evidence_last_state"]["round_s"])
            journal = (directory / "changes.jsonl").read_bytes()
            self.assertEqual(hashlib.sha256(journal).hexdigest(), result["change_sha256"])
            self.assertEqual(result["change_entries"], len(journal.splitlines()))
            self.assertTrue(all(len(line) + 1 <= 256 for line in journal.splitlines()))
            self.assertEqual(0o600, (directory / "changes.jsonl").stat().st_mode & 0o777)
            self.assertLessEqual((directory / "summary.json").stat().st_size, 8192)
            self.assertEqual(result, json.loads((directory / "summary.json").read_text()))

    def test_regular_deadline_changes_survive_constant_night_deadline(self):
        lines = []
        for i in range(4):
            uptime = 100_000 + i * 10_000
            lines.extend([(0 if i == 0 else 10, metric(uptime)),
                          (0, state(uptime, i + 1, due=200_000 + i * 10_000,
                                    effective_due=250_000)),
                          (0, health(uptime, i + 1))])
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root) / "night"
            result = self.invoke(lines, directory)
            self.assertEqual("complete", result["evidence_coverage"])
            time_entries = [json.loads(line) for line in
                            (directory / "changes.jsonl").read_text().splitlines()
                            if json.loads(line)[0] == "t"]
            self.assertEqual([200_000, 210_000, 220_000, 230_000],
                             [entry[-3] for entry in time_entries])
            self.assertTrue(all(entry[-4] == 7 and entry[-2] == 250_000 and
                                entry[-1] == 1 for entry in time_entries))
            self.assertTrue(all(len(json.dumps(entry, separators=(",", ":"))) + 1 <= 256
                                for entry in time_entries))

    def test_legacy_and_missing_pair_are_incomplete(self):
        with tempfile.TemporaryDirectory() as root:
            legacy = self.invoke([(0, metric(100_000))] +
                                 [(10, metric(100_000 + i * 10_000)) for i in range(1, 4)],
                                 Path(root) / "legacy")
            self.assertEqual("complete", legacy["state"])
            self.assertEqual("legacy_missing", legacy["evidence_coverage"])
            self.assertEqual("unknown", legacy["ack_evidence"])
            self.assertEqual(0, legacy["evidence_samples"])
            missing = self.invoke(schedule(missing_health=True), Path(root) / "missing")
            self.assertEqual("incomplete", missing["evidence_coverage"])
            self.assertEqual(1, missing["evidence_missing_pairs"])
            unknown = schedule()
            unknown[2] = (0, unknown[2][1].replace(b"image_v=1", b"image_v=0"))
            result = self.invoke(unknown, Path(root) / "unknown")
            self.assertEqual("incomplete", result["evidence_coverage"])
            self.assertEqual(1, result["evidence_unavailable_samples"])

    def test_wrong_pair_or_counter_roll_back_fails_closed(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(longrun.LongrunError, "verschiedenen Samples"):
                self.invoke(schedule(mismatched_health=True), Path(root) / "wrong")
            with self.assertRaisesRegex(longrun.LongrunError, "Fehlerzähler sprang zurück"):
                self.invoke(schedule(rollback=True), Path(root) / "rollback")

    def test_journal_hard_cap_without_silent_discard(self):
        u64 = (1 << 64) - 1
        i64 = -(1 << 63)
        maximal_time_entry = ["t", u64, u64, u64, u64, 1, i64, 1, i64,
                              1, i64, 2, 2, u64, u64, u64, 1]
        self.assertLessEqual(len((json.dumps(maximal_time_entry,
                                             separators=(",", ":")) + "\n").encode()), 256)
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root) / "run"
            directory.mkdir(mode=0o700)
            args = argparse.Namespace(expect_device=DEVICE, expect_firmware=FIRMWARE,
                                      expect_build=BUILD, duration_seconds=30)
            recorder = longrun.Recorder(args, None, directory, lambda: 0.0, lambda: 0.0)
            for i in range(256):
                recorder._append_change(["s", i, i])
            with self.assertRaisesRegex(longrun.LongrunError, "feste Grenze"):
                recorder._append_change(["s", 256, 256])
            self.assertEqual(256, recorder.summary["change_entries"])
            self.assertEqual(256, len((directory / "changes.jsonl").read_bytes().splitlines()))
            with self.assertRaisesRegex(longrun.LongrunError, "feste Grenze"):
                recorder._append_change(["s", "x" * 256])

    def test_saturated_error_count_and_boot_domain_change_fail(self):
        first = schedule()[:3]
        second_uptime = 110_000
        with tempfile.TemporaryDirectory() as root:
            saturated = health(second_uptime, 2, image_fail=1).replace(
                b"image_fail=1", b"image_fail=4294967295")
            lines = first + [(10, metric(second_uptime)),
                             (0, state(second_uptime, 2)), (0, saturated)]
            with self.assertRaisesRegex(longrun.LongrunError, "erschöpft"):
                self.invoke(lines, Path(root) / "saturated")
            lines = first + [(10, metric(second_uptime)),
                             (0, state(second_uptime, 2, domain=99)),
                             (0, health(second_uptime, 2, domain=99))]
            with self.assertRaisesRegex(longrun.LongrunError, "Bootdomäne"):
                self.invoke(lines, Path(root) / "domain")


if __name__ == "__main__":
    unittest.main()
