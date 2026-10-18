#!/usr/bin/env python3
"""Single-port, fail-closed, bounded 48-hour KM1 metadata recorder.

Numerical evidence coverage describes the fixed diagnostic samples only.
Group ACK and global confirmation uniqueness remain unknown here; they need
separate private Store-v5 identity and physical group evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import time
from typing import Callable

import katmed_usb_setup as usb


RUN_SECONDS = 48 * 60 * 60
DIAG_GAP_SECONDS = 30
STATUS_GAP_SECONDS = 90
STATUS_PERIOD_SECONDS = 60
FINISH_GRACE_SECONDS = 30
MAX_INPUT_BYTES = 64 * 1024 * 1024
MAX_INPUT_LINES = 300_000
MAX_SUMMARY_BYTES = 8192
MAX_CHANGE_ENTRIES = 256
MAX_CHANGE_LINE_BYTES = 256
MAX_CHANGE_BYTES = MAX_CHANGE_ENTRIES * MAX_CHANGE_LINE_BYTES
MAX_WALL_MONO_SKEW_SECONDS = 120
MAX_UPTIME_HOST_SKEW_SECONDS = 2
MIN_START_UPTIME_MS = (MAX_UPTIME_HOST_SKEW_SECONDS + 1) * 1000
MAX_PRIMING_SECONDS = 30


class LongrunError(usb.SetupError):
    """Public, sanitized failure reason."""


def _private_new_directory(path: Path) -> None:
    if path.exists() or path.is_symlink():
        raise LongrunError("Evidenzverzeichnis existiert bereits; kein Wiederaufnehmen")
    parent = path.parent
    try:
        info = parent.stat(follow_symlinks=False)
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or
                info.st_mode & 0o077):
            raise LongrunError("Evidenz-Elternverzeichnis benötigt Eigentum und höchstens 0700")
        path.mkdir(mode=0o700)
    except LongrunError:
        raise
    except OSError:
        raise LongrunError("Privates Evidenzverzeichnis konnte nicht erstellt werden") from None


def _save(directory: Path, summary: dict) -> None:
    encoded = (json.dumps(summary, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
    if len(encoded) > MAX_SUMMARY_BYTES:
        raise LongrunError("Evidenzdatei überschreitet die feste Grenze")
    temp = directory / ".summary.tmp"
    target = directory / "summary.json"
    fd = None
    try:
        fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "wb", closefd=True) as stream:
            fd = None
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, target)
        dir_fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(dir_fd)
        finally:
            os.close(dir_fd)
    except OSError:
        raise LongrunError("Private Evidenz konnte nicht dauerhaft geschrieben werden") from None
    finally:
        if fd is not None:
            os.close(fd)
        try:
            temp.unlink(missing_ok=True)
        except OSError:
            pass


class Recorder:
    def __init__(self, args: argparse.Namespace, port: usb.SerialPort, directory: Path,
                 monotonic: Callable[[], float], wall: Callable[[], float]):
        self.args = args
        self.port = port
        self.directory = directory
        self.monotonic = monotonic
        self.wall = wall
        self.started_mono: float | None = None
        self.started_wall: float | None = None
        self.first_metric_mono: float | None = None
        self.last_metric_mono: float | None = None
        self.last_status_mono: float | None = None
        self.first_uptime: int | None = None
        self.last_uptime: int | None = None
        self.priming_seen = False
        self.rebind_pending = False
        self.rebound_after_priming = False
        self.catalog: str | None = None
        self.next_save: float | None = None
        self.pending_metric_uptime: int | None = None
        self.pending_state: dict[str, int] | None = None
        self.pending_health: dict[str, int] | None = None
        self.phase_pending: dict[str, int] | None = None
        self.phase_highest: dict[str, int] | None = None
        self.phase_rejection_class: str | None = None
        self.last_evidence_seq: int | None = None
        self.evidence_domain: int | None = None
        self.last_state_key: tuple | None = None
        self.last_health_key: tuple | None = None
        self.change_hash = hashlib.sha256()
        try:
            fd = os.open(directory / "changes.jsonl",
                         os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            os.close(fd)
        except OSError:
            raise LongrunError("Privates Ereignisjournal konnte nicht angelegt werden") from None
        self.summary = {
            "schema": 1, "state": "opening", "reason": "opening",
            "device": args.expect_device.lower(), "firmware": args.expect_firmware.lower(),
            "build": args.expect_build.lower(), "catalog": None,
            "start_unix_seconds": None, "last_unix_seconds": None,
            "last_status_unix_seconds": None, "last_metric_unix_seconds": None,
            "elapsed_seconds": 0, "uptime_start_ms": None,
            "uptime_last_ms": None, "uptime_elapsed_ms": 0,
            "status_count": 0, "metric_count": 0, "priming_metric_count": 0,
            "unknown_line_count": 0,
            "input_bytes": 0, "input_lines": 0,
            "limits": {"duration_seconds": args.duration_seconds,
                       "diag_gap_seconds": DIAG_GAP_SECONDS,
                       "status_gap_seconds": STATUS_GAP_SECONDS,
                       "max_input_bytes": MAX_INPUT_BYTES,
                       "max_input_lines": MAX_INPUT_LINES},
            "last_status": None, "metric_min": {}, "metric_max": {},
            "evidence_coverage": "legacy_missing", "evidence_samples": 0,
            "ack_evidence": "unknown",
            "evidence_missing_pairs": 0, "evidence_last_state": None,
            "evidence_last_health": None, "evidence_boot_domain": None,
            "evidence_unavailable_samples": 0,
            "phase_coverage": "legacy_missing",
            "phase_prestatus_unverified": None,
            "phase_poststatus_unverified": None,
            "phase_last_verified": None,
            "completion_qualification": "not_complete",
            "change_entries": 0, "change_bytes": 0,
            "change_sha256": self.change_hash.hexdigest(),
        }
        _save(directory, self.summary)

    def _elapsed(self) -> float:
        assert self.started_mono is not None and self.started_wall is not None
        now_mono = self.monotonic()
        now_wall = self.wall()
        elapsed = now_mono - self.started_mono
        if elapsed < 0 or now_wall < self.started_wall or abs((now_wall - self.started_wall) - elapsed) > MAX_WALL_MONO_SKEW_SECONDS:
            raise LongrunError("Hostzeit ist rückläufig oder weicht von der Laufzeit ab")
        self.summary["elapsed_seconds"] = int(elapsed)
        self.summary["last_unix_seconds"] = int(now_wall)
        return elapsed

    def _health(self) -> None:
        now = self.monotonic()
        if self.last_metric_mono is not None and now - self.last_metric_mono > DIAG_GAP_SECONDS:
            raise LongrunError("Gültige Diagnose blieb länger als 30 Sekunden aus")
        if self.last_status_mono is not None and now - self.last_status_mono > STATUS_GAP_SECONDS:
            raise LongrunError("Frischer KM1-Status blieb länger als 90 Sekunden aus")
        if self.started_mono is not None:
            self._elapsed()
            if now - self.started_mono > self.args.duration_seconds + FINISH_GRACE_SECONDS:
                raise LongrunError("Gerätelaufzeit erreichte das Ziel nicht innerhalb der Abschlussfrist")

    def _line(self) -> bytes | None:
        try:
            raw = self.port.read_until(b"\n", usb.MAX_DIAG_CHARS + 2)
        except EOFError:
            raise LongrunError("USB-Verbindung ist beendet") from None
        except Exception:
            raise LongrunError("USB-Lesen fehlgeschlagen") from None
        if not isinstance(raw, bytes):
            raise LongrunError("Ungültige USB-Ausgabe")
        self._health()
        if not raw:
            return None
        self.summary["input_bytes"] += len(raw)
        self.summary["input_lines"] += 1
        if (self.summary["input_bytes"] > MAX_INPUT_BYTES or
                self.summary["input_lines"] > MAX_INPUT_LINES):
            raise LongrunError("USB-Eingabe überschreitet die feste Ressourcengrenze")
        if len(raw) > usb.MAX_DIAG_CHARS + 2 or not raw.endswith(b"\n"):
            raise LongrunError("USB-Zeile ist zu lang oder unvollständig")
        body = raw[:-1]
        if body.endswith(b"\r"):
            body = body[:-1]
        if b"\r" in body or len(body) > usb.MAX_DIAG_CHARS:
            raise LongrunError("USB-Zeilenende ist ungültig")
        if body.startswith(usb.BOOT_MARKERS):
            raise LongrunError("Geräteneustart erkannt")
        if body.startswith(b"KM1 ") and len(raw) > usb.MAX_RESPONSE_BYTES:
            raise LongrunError("KM1-Antwort überschreitet 256 Bytes")
        return body

    def _append_change(self, event: list) -> None:
        encoded = (json.dumps(event, separators=(",", ":"), ensure_ascii=True) + "\n").encode("ascii")
        if (len(encoded) > MAX_CHANGE_LINE_BYTES or
                self.summary["change_entries"] >= MAX_CHANGE_ENTRIES or
                self.summary["change_bytes"] + len(encoded) > MAX_CHANGE_BYTES):
            raise LongrunError("Privates Ereignisjournal überschreitet die feste Grenze")
        fd = None
        try:
            fd = os.open(self.directory / "changes.jsonl",
                         os.O_WRONLY | os.O_APPEND | os.O_NOFOLLOW)
            if os.write(fd, encoded) != len(encoded):
                raise OSError("short private write")
            os.fsync(fd)
        except OSError:
            raise LongrunError("Privates Ereignisjournal konnte nicht dauerhaft geschrieben werden") from None
        finally:
            if fd is not None:
                os.close(fd)
        self.change_hash.update(encoded)
        self.summary["change_entries"] += 1
        self.summary["change_bytes"] += len(encoded)
        self.summary["change_sha256"] = self.change_hash.hexdigest()

    def _evidence(self, parsed: tuple[str, dict[str, int]]) -> None:
        if self.last_status_mono is None:
            return
        kind, fields = parsed
        if self.pending_metric_uptime is None:
            if self.summary["metric_count"]:
                raise LongrunError("Messbeleg ohne zugehörige Diagnose")
            return  # A pre-baseline fragment is not part of the valued run.
        if fields["uptime_ms"] != self.pending_metric_uptime:
            raise LongrunError("Messbeleg und Diagnose stammen aus verschiedenen Samples")
        if kind == "state":
            if self.pending_state is not None or self.pending_health is not None:
                raise LongrunError("Doppelter oder falsch geordneter Zustandsbeleg")
            if self.last_evidence_seq is not None and fields["sample_seq"] <= self.last_evidence_seq:
                raise LongrunError("Messbelegfolge sprang zurück")
            if self.evidence_domain is not None and fields["boot_domain"] != self.evidence_domain:
                raise LongrunError("Messbeleg wechselte ohne Geräteabgleich die Bootdomäne")
            self.pending_state = fields
        else:
            if self.pending_state is None or self.pending_health is not None:
                raise LongrunError("Gesundheitsbeleg ohne eindeutigen Zustand")
            state = self.pending_state
            if any(fields[key] != state[key] for key in ("sample_seq", "uptime_ms", "boot_domain")):
                raise LongrunError("Zustand und Gesundheitsbeleg haben verschiedene Sample-IDs")
            self.pending_health = fields
            self._finish_evidence_pair()

    def _finish_evidence_pair(self) -> None:
        if self.pending_metric_uptime is None:
            return
        state, health = self.pending_state, self.pending_health
        self.pending_metric_uptime = None
        self.pending_state = None
        self.pending_health = None
        if state is None or health is None:
            first_missing = self.summary["evidence_missing_pairs"] == 0
            self.summary["evidence_missing_pairs"] += 1
            if self.summary["evidence_samples"]:
                self.summary["evidence_coverage"] = "incomplete"
            if first_missing:
                _save(self.directory, self.summary)
            return
        seq, uptime, domain = state["sample_seq"], state["uptime_ms"], state["boot_domain"]
        if self.evidence_domain is None:
            self.evidence_domain = domain
        if self.phase_pending is not None:
            if self.phase_pending["boot_domain"] != domain:
                raise LongrunError("KM1-Phasenbeleg und Zustandsbeleg haben verschiedene Bootdomänen")
            self.summary["phase_last_verified"] = self.phase_pending
            self.summary["phase_poststatus_unverified"] = None
            self.summary["phase_coverage"] = "verified_sample"
            self.phase_pending = None
        self.last_evidence_seq = seq
        previous = self.summary["evidence_last_health"]
        counters = ("image_fail", "store_read", "store_load", "store_commit",
                    "mesh_send", "mesh_queue", "mesh_level", "mesh_ip_got",
                    "mesh_ip_lost", "mesh_up", "mesh_down")
        if previous is not None:
            for key in counters:
                if health[key] < previous[key]:
                    raise LongrunError("Beobachteter Fehlerzähler sprang zurück")
        if any(health[key] == 0xffffffff for key in counters):
            raise LongrunError("Beobachteter Fehlerzähler ist erschöpft")
        state_key = tuple(state[key] for key in (
            "round_d", "round_e", "round_s", "round_o", "alarm", "reason",
            "origin_d", "origin_e", "origin_s", "start_d", "start_ms", "start_v",
            "start_utc", "clock_d", "due_r", "due_e", "due_local", "due_rv",
            "due_ru", "due_ev", "due_eu", "now_v", "now_q", "utc_source",
            "image_id", "image_pending"))
        if state_key != self.last_state_key:
            self._append_change(["s", seq, uptime,
                [state[key] for key in ("round_d", "round_e", "round_s", "round_o")],
                state["alarm"],
                [state[key] for key in ("origin_d", "origin_e", "origin_s")],
                state["due_e"], state["due_local"], state["image_id"]])
            self._append_change(["t", seq, uptime, state["start_d"], state["start_ms"],
                state["start_v"], state["start_utc"], state["due_rv"], state["due_ru"],
                state["due_ev"], state["due_eu"], state["now_q"], state["utc_source"],
                state["clock_d"], state["due_r"], state["due_e"], state["due_local"]])
            self.last_state_key = state_key
        health_key = tuple(health[key] for key in (
            "image_v", "image_fail", "store_v", "store_read", "store_load",
            "store_commit", "mesh_v", "mesh_send", "mesh_queue", "mesh_level",
            "mesh_ip_got", "mesh_ip_lost", "mesh_up", "mesh_down", "mesh_link",
            "mesh_root", "mesh_root_ip"))
        if health_key != self.last_health_key:
            self._append_change(["h", seq, uptime, list(health_key)])
            self.last_health_key = health_key
        self.summary["evidence_samples"] += 1
        if not (health["image_v"] and health["store_v"] and health["mesh_v"]):
            self.summary["evidence_unavailable_samples"] += 1
        self.summary["evidence_coverage"] = (
            "complete" if self.summary["evidence_missing_pairs"] == 0 and
                          self.summary["evidence_unavailable_samples"] == 0
            else "incomplete")
        self.summary["evidence_boot_domain"] = domain
        self.summary["evidence_last_state"] = state
        self.summary["evidence_last_health"] = health
        _save(self.directory, self.summary)

    def _usb_phase(self, fields: dict[str, int]) -> None:
        if (fields["phase_seq"] == 0xffffffffffffffff or
                any(fields[key] == 0xffffffff for key in usb.USB_PHASE_COUNTERS)):
            self.summary["phase_coverage"] = "exhausted"
            _save(self.directory, self.summary)
            raise LongrunError("KM1-Phasenbeobachtung ist erschöpft")
        highest = self.phase_highest
        if highest is not None:
            if fields["boot_domain"] != highest["boot_domain"]:
                raise LongrunError("KM1-Phasenbeleg wechselte die Bootdomäne")
            if fields["phase_seq"] <= highest["phase_seq"]:
                return  # A valid older task/main snapshot can be emitted late.
            if (fields["read"] < highest["read"] or
                    any(fields[key] < highest[key] for key in usb.USB_PHASE_COUNTERS)):
                raise LongrunError("KM1-Phasenzähler sprang innerhalb der Bootdomäne zurück")
        self.phase_highest = fields
        if self.last_status_mono is None:
            self.summary["phase_prestatus_unverified"] = fields
            self.summary["phase_coverage"] = "prestatus_unverified"
            _save(self.directory, self.summary)
            return
        if self.evidence_domain is not None and fields["boot_domain"] != self.evidence_domain:
            raise LongrunError("KM1-Phasenbeleg wechselte ohne Geräteabgleich die Bootdomäne")
        if self.evidence_domain is None:
            self.phase_pending = fields
            self.summary["phase_poststatus_unverified"] = fields
            self.summary["phase_coverage"] = "poststatus_unverified"
        else:
            self.summary["phase_last_verified"] = fields
            self.summary["phase_coverage"] = "verified_sample"
        _save(self.directory, self.summary)

    def _phase_rejection(self, kind: str) -> None:
        if self.phase_rejection_class is None and kind in (
                "envelope", "field_count", "key_order", "number_syntax",
                "number_range", "schema_or_domain", "task_or_read_range",
                "counter_relation", "drain_line_overlong", "drain_internal_cr",
                "drain_incomplete"):
            self.phase_rejection_class = kind

    def _metric(self, body: bytes) -> None:
        phase = usb._usb_phase_line(body, self._phase_rejection)
        if phase is not None:
            self._usb_phase(phase)
            return
        if b"katmed_usb_phase:" in body:
            raise LongrunError("Numerischer KM1-Phasenbeleg ist beschädigt")
        evidence = usb._evidence_line(body)
        if evidence is not None:
            self._evidence(evidence)
            return
        if b"katmed_state:" in body or b"katmed_health:" in body:
            raise LongrunError("Numerischer Messbeleg ist beschädigt")
        parsed = usb._metric_line(body)
        if parsed is None:
            if b"katmed_metrics:" in body:
                raise LongrunError("Numerische Diagnose ist beschädigt")
            self.summary["unknown_line_count"] += 1
            return
        if self.last_status_mono is None:
            return  # A metric before verified STATUS has no trusted device identity.
        line, uptime = parsed
        self._finish_evidence_pair()
        if self.last_uptime is not None and uptime < self.last_uptime:
            raise LongrunError("Gerätelaufzeit sprang zurück; Lauf beendet")
        now = self.monotonic()
        if (self.last_uptime is not None and self.last_metric_mono is not None and
                abs((uptime - self.last_uptime) / 1000 -
                    (now - self.last_metric_mono)) > MAX_UPTIME_HOST_SKEW_SECONDS):
            raise LongrunError("Gerätelaufzeit und Hostzeit sind unstimmig")
        if self.first_metric_mono is None:
            self.first_metric_mono = now
        self.last_uptime = uptime
        self.last_metric_mono = now
        self.summary["uptime_last_ms"] = uptime
        self.summary["last_metric_unix_seconds"] = int(self.wall())
        if (self.first_uptime is None and self.first_metric_mono is not None and
                now - self.first_metric_mono > MAX_PRIMING_SECONDS):
            raise LongrunError("Sichere Gerätelaufzeit-Basis blieb in der Vorphase aus")
        if self.first_uptime is None and uptime < MIN_START_UPTIME_MS:
            self.priming_seen = True
            self.rebind_pending = False
            self.rebound_after_priming = False
            self.summary["state"] = "priming"
            self.summary["reason"] = "awaiting_safe_uptime_basis"
            self.summary["priming_metric_count"] += 1
            _save(self.directory, self.summary)
            return
        if self.first_uptime is None and self.priming_seen and not self.rebound_after_priming:
            self.rebind_pending = True
            self.summary["state"] = "priming"
            self.summary["reason"] = "awaiting_fresh_status_after_priming"
            self.summary["priming_metric_count"] += 1
            _save(self.directory, self.summary)
            return
        if self.first_uptime is None:
            self.first_uptime = uptime
            self.started_mono = now
            self.started_wall = self.wall()
            self.summary["start_unix_seconds"] = int(self.started_wall)
            self.summary["uptime_start_ms"] = uptime
            self.summary["state"] = "running"
            self.summary["reason"] = "running"
        self.summary["uptime_elapsed_ms"] = uptime - self.first_uptime
        self.summary["metric_count"] += 1
        self.pending_metric_uptime = uptime
        fields = dict(token.split("=", 1) for token in line.removeprefix("Messung ").split(" "))
        for key in usb.METRIC_KEYS:
            if key == "uptime_ms":
                continue
            value = int(fields[key])
            minimum = self.summary["metric_min"]
            maximum = self.summary["metric_max"]
            minimum[key] = min(minimum.get(key, value), value)
            maximum[key] = max(maximum.get(key, value), value)
        self._elapsed()
        if self.next_save is None or now >= self.next_save:
            _save(self.directory, self.summary)
            self.next_save = now + STATUS_PERIOD_SECONDS

    def _status_reply(self, _port: usb.SerialPort) -> str:
        deadline = self.monotonic() + usb.SESSION_REPLY_SECONDS
        while self.monotonic() < deadline:
            body = self._line()
            if body is None:
                continue
            if body.startswith(b"KM1 "):
                try:
                    return body.decode("ascii")
                except UnicodeError:
                    raise LongrunError("KM1-Status ist kein ASCII") from None
            self._metric(body)
        raise LongrunError("KM1-Statusantwort fehlt innerhalb von acht Sekunden")

    def status(self) -> None:
        fields = usb.read_status(self.port, self._status_reply)
        if (fields["device"].lower() != self.args.expect_device.lower() or
                fields["firmware"].lower() != self.args.expect_firmware.lower() or
                fields["build"].lower() != self.args.expect_build.lower()):
            raise LongrunError("Geräte-, Firmware- oder Buildkennung weicht ab")
        if self.catalog is not None and fields["catalog"].lower() != self.catalog:
            raise LongrunError("Katalogkennung hat sich geändert")
        self.catalog = fields["catalog"].lower()
        self.summary["catalog"] = self.catalog
        self.summary["last_status"] = {
            key: fields[key] for key in ("configured", "durable", "pending", "clock", "error")
        }
        self.last_status_mono = self.monotonic()
        self.summary["last_status_unix_seconds"] = int(self.wall())
        self.summary["status_count"] += 1
        _save(self.directory, self.summary)

    def run(self) -> dict:
        self.status()
        first_deadline = self.monotonic() + DIAG_GAP_SECONDS
        while True:
            self._health()
            if self.first_metric_mono is None and self.monotonic() > first_deadline:
                raise LongrunError("Erste gültige Diagnose blieb aus")
            if (self.started_mono is None and self.first_metric_mono is not None and
                    self.monotonic() - self.first_metric_mono > MAX_PRIMING_SECONDS):
                raise LongrunError("Sichere Gerätelaufzeit-Basis blieb in der Vorphase aus")
            if self.rebind_pending:
                self.status()
                self.rebind_pending = False
                self.rebound_after_priming = True
                continue
            if (self.started_mono is not None and
                    self._elapsed() >= self.args.duration_seconds and
                    self.summary["uptime_elapsed_ms"] >= self.args.duration_seconds * 1000):
                # The legacy line is logged immediately before the two new
                # records. Give this already observed sample a bounded chance
                # to finish; an older firmware remains readable but incomplete.
                pair_deadline = min(self.monotonic() + 2.0,
                                    self.started_mono + self.args.duration_seconds +
                                    FINISH_GRACE_SECONDS)
                while (self.pending_metric_uptime is not None and
                       self.summary["evidence_samples"] > 0 and
                       self.monotonic() < pair_deadline):
                    body = self._line()
                    if body is None:
                        continue
                    if body.startswith(b"KM1 "):
                        raise LongrunError("Unerwartete KM1-Antwort außerhalb einer Statusabfrage")
                    self._metric(body)
                self._finish_evidence_pair()
                phase = self.summary["phase_last_verified"]
                if (phase is not None and phase["task"] == 1 and phase["read"] == 1 and
                        all(phase[key] > 0 for key in
                            ("status", "queue_ok", "main_take", "reply_ready", "reply_call"))):
                    self.summary["completion_qualification"] = "time_uptime_plus_internal_status_path"
                elif phase is not None:
                    self.summary["completion_qualification"] = "time_uptime_phase_partial"
                elif self.summary["phase_coverage"] == "legacy_missing":
                    self.summary["completion_qualification"] = "time_uptime_phase_missing"
                else:
                    self.summary["completion_qualification"] = "time_uptime_phase_unverified"
                self.summary["state"] = "complete"
                self.summary["reason"] = "continuous_time_and_uptime_reached"
                _save(self.directory, self.summary)
                return self.summary
            if self.monotonic() - self.last_status_mono >= STATUS_PERIOD_SECONDS:
                self.status()
                continue
            body = self._line()
            if body is None:
                continue
            if body.startswith(b"KM1 "):
                raise LongrunError("Unerwartete KM1-Antwort außerhalb einer Statusabfrage")
            self._metric(body)

    def fail(self, reason: str) -> None:
        self.summary["state"] = "failed"
        self.summary["reason"] = reason
        self.summary["last_unix_seconds"] = int(self.wall())
        if self.started_mono is not None:
            self.summary["elapsed_seconds"] = max(0, int(self.monotonic() - self.started_mono))
        _save(self.directory, self.summary)


def _startup_with_early_phases(recorder: Recorder, port: usb.SerialPort,
                               monotonic: Callable[[], float]) -> None:
    """Retain only valid early phases in the existing private recorder."""
    usb._drain_startup(port, monotonic, phase_observer=recorder._usb_phase,
                       rejection_observer=recorder._phase_rejection)


def run_longrun(args: argparse.Namespace, *, opener=None, startup=usb._drain_startup,
                monotonic: Callable[[], float] = time.monotonic,
                wall: Callable[[], float] = time.time,
                emit: Callable[[str], None] = print) -> dict:
    if not usb.DEVICE_RE.fullmatch(args.expect_device or ""):
        raise LongrunError("--expect-device benötigt zwölf Hexzeichen")
    if re.fullmatch(r"[0-9a-fA-F]{7,12}", args.expect_firmware or "") is None:
        raise LongrunError("--expect-firmware benötigt 7 bis 12 reine Hexzeichen")
    if re.fullmatch(r"[0-9a-fA-F]{6,24}", args.expect_build or "") is None:
        raise LongrunError("--expect-build benötigt 6 bis 24 reine Hexzeichen")
    if not isinstance(args.duration_seconds, int) or args.duration_seconds < 30 or args.duration_seconds > RUN_SECONDS:
        raise LongrunError("Ungültige Dauerlaufzeit")
    directory = Path(args.evidence_dir).absolute()
    _private_new_directory(directory)
    if opener is None:
        opener = lambda path: usb._open_serial(path, timeout=0.25)
    port = None
    recorder = None
    first_reason: str | None = None
    try:
        recorder = Recorder(args, None, directory, monotonic, wall)
        port = opener(args.port)
        recorder.port = port
        if startup is usb._drain_startup:
            _startup_with_early_phases(recorder, port, monotonic)
        else:
            startup(port)
        result = recorder.run()
        emit("Dauerlauf vollständig: private Metadaten in summary.json")
        return result
    except BaseException as exc:
        reason = str(exc) if isinstance(exc, (LongrunError, usb.SetupError)) else "Abbruch oder unerwarteter Laufzeitfehler"
        first_reason = reason
        if recorder is not None:
            recorder.fail(reason)
        raise LongrunError(reason) from None
    finally:
        if port is not None:
            try:
                port.close()
            except Exception:
                if first_reason is None:
                    if recorder is not None:
                        recorder.fail("USB-Port konnte nicht sicher geschlossen werden")
                    raise LongrunError("USB-Port konnte nicht sicher geschlossen werden") from None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Private 48-Stunden-KM1-Metadatenaufnahme")
    parser.add_argument("--port", required=True)
    parser.add_argument("--expect-device", required=True)
    parser.add_argument("--expect-firmware", required=True)
    parser.add_argument("--expect-build", required=True)
    parser.add_argument("--evidence-dir", required=True,
                        help="Noch nicht existierendes Unterverzeichnis eines privaten 0700-Verzeichnisses")
    args = parser.parse_args(argv)
    args.duration_seconds = RUN_SECONDS
    try:
        run_longrun(args)
        return 0
    except LongrunError as exc:
        print(f"Fehler: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
