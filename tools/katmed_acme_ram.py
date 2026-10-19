#!/usr/bin/env python3
"""Bounded host gate for the ACME RAM-prüfstand.

The input is a local artificial capture or a future passive long-run excerpt.
It is deliberately not a device-control command. Runtime metric syntax is
parsed by the existing katmed_usb_setup helper so the RAM result uses the same
numeric boundary as the real long-run recorder.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
import tempfile

import katmed_usb_setup as usb


PHASES = (
    "baseline", "initialization", "order", "challenge",
    "certificate", "error", "recovery", "retry",
)
MAX_INPUT_BYTES = 1024 * 1024
MAX_INPUT_LINES = 16_384
MAX_SUMMARY_BYTES = 4096
UINT32_MAX = 0xFFFFFFFF

_PHASE_RE = re.compile(
    rb"I \([0-9]{1,10}\) katmed_acme_phase: "
    rb"schema=1 phase_seq=([1-9][0-9]{0,19}) phase=(baseline|initialization|order|"
    rb"challenge|certificate|error|recovery|retry)"
)


class RamBenchError(ValueError):
    """Sanitized, fail-closed RAM bench input error."""


def _phase(body: bytes) -> tuple[int, str] | None:
    match = _PHASE_RE.fullmatch(body)
    if match is None:
        return None
    sequence = int(match.group(1))
    if sequence > UINT32_MAX:
        raise RamBenchError("ACME-Phasenfolge überschreitet die feste Grenze")
    return sequence, match.group(2).decode("ascii")


def _metric(body: bytes) -> dict[str, int] | None:
    parsed = usb._metric_line(body)
    if parsed is None:
        return None
    values = {}
    for token in parsed[0].removeprefix("Messung ").split(" "):
        name, value = token.split("=", 1)
        values[name] = int(value)
    required = (
        "uptime_ms", "internal_min_free", "internal_largest_block",
        "main_stack_hwm_bytes", "lvgl_stack_hwm_bytes", "lvgl_stack_valid",
    )
    if any(name not in values for name in required):
        raise RamBenchError("Runtime-Metrik enthält keine vollständige RAM-Messung")
    if any(values[name] > UINT32_MAX for name in required[1:]):
        raise RamBenchError("Runtime-Metrik überschreitet die feste Byte-Grenze")
    if values["uptime_ms"] > 0xFFFFFFFFFFFFFFFF:
        raise RamBenchError("Runtime-Metrik enthält eine ungültige Laufzeit")
    if values["lvgl_stack_valid"] != 1:
        raise RamBenchError("LVGL-Stack-HWM ist für den RAM-Prüfstand nicht verfügbar")
    return values


def evaluate(raw: bytes) -> dict:
    if len(raw) > MAX_INPUT_BYTES:
        raise RamBenchError("RAM-Prüfstandseingabe überschreitet die feste Grenze")
    lines = raw.splitlines()
    if len(lines) > MAX_INPUT_LINES:
        raise RamBenchError("RAM-Prüfstandseingabe enthält zu viele Zeilen")
    if not lines:
        raise RamBenchError("RAM-Prüfstandseingabe ist leer")

    samples: list[dict[str, int | str]] = []
    expected_sequence = 1
    pending: tuple[int, str] | None = None
    previous_uptime = -1
    for raw_line in lines:
        if b"\r" in raw_line:
            raise RamBenchError("RAM-Prüfstandseingabe enthält ein ungültiges Zeilenende")
        body = raw_line
        phase = _phase(body)
        if b"katmed_acme_phase:" in body:
            if phase is None:
                raise RamBenchError("ACME-Phasenbeleg ist beschädigt")
            sequence, name = phase
            if sequence != expected_sequence or name != PHASES[(sequence - 1) % len(PHASES)]:
                raise RamBenchError("ACME-Phasenfolge ist nicht baseline-init-order-challenge-cert-error-recovery-retry")
            if pending is not None:
                raise RamBenchError("ACME-Phase erhielt keinen zugehörigen Runtime-Snapshot")
            pending = phase
            expected_sequence += 1
            continue
        metric = _metric(body)
        if b"katmed_metrics:" in body:
            if metric is None:
                raise RamBenchError("Runtime-Metrik ist beschädigt")
            if pending is None:
                raise RamBenchError("Runtime-Snapshot steht ohne ACME-Phase")
            if metric["uptime_ms"] <= previous_uptime:
                raise RamBenchError("Runtime-Laufzeit ist nicht streng monoton")
            previous_uptime = metric["uptime_ms"]
            samples.append({"phase": pending[1], **metric})
            pending = None
            continue
        if metric is not None:
            raise RamBenchError("Nicht zuordenbarer Runtime-Snapshot")
    if pending is not None:
        raise RamBenchError("Letzte ACME-Phase hat keinen Runtime-Snapshot")
    if len(samples) < len(PHASES) or len(samples) % len(PHASES):
        raise RamBenchError("RAM-Prüfstand deckt nicht alle definierten ACME-Phasen ab")
    if [sample["phase"] for sample in samples[:len(PHASES)]] != list(PHASES):
        raise RamBenchError("RAM-Prüfstand beginnt nicht mit der definierten Baseline")

    summary = {
        "schema": 1,
        "qualification": "qualified_host_synthetic",
        "required_phases": list(PHASES),
        "repetitions": len(samples) // len(PHASES),
        "samples": len(samples),
        "internal_min_free_min": min(int(sample["internal_min_free"]) for sample in samples),
        "internal_largest_block_min": min(int(sample["internal_largest_block"]) for sample in samples),
        "main_stack_hwm_min": min(int(sample["main_stack_hwm_bytes"]) for sample in samples),
        "lvgl_stack_hwm_min": min(int(sample["lvgl_stack_hwm_bytes"]) for sample in samples),
        "lvgl_stack_valid_all": True,
        "measurement_source": "existing katmed_metrics parser; local artificial input",
    }
    encoded = (json.dumps(summary, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
    if len(encoded) > MAX_SUMMARY_BYTES:
        raise RamBenchError("RAM-Prüfstandsergebnis überschreitet die feste Grenze")
    return summary


def _write_summary(path: Path, summary: dict) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    encoded = (json.dumps(summary, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
    fd, temp_name = tempfile.mkstemp(prefix=".acme-ram-", dir=path.parent)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_name, path)
        directory_fd = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except OSError as exc:
        raise RamBenchError("RAM-Prüfstandsergebnis konnte nicht atomar geschrieben werden") from exc
    finally:
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        raw = args.input.read_bytes()
        summary = evaluate(raw)
        _write_summary(args.output, summary)
    except (OSError, RamBenchError) as exc:
        print(f"ACME-RAM-Prüfstand abgebrochen: {exc}", file=sys.stderr)
        return 1
    print("ACME-RAM-Prüfstand: host-synthetische Phasenfolge qualifiziert")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
