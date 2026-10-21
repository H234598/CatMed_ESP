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
RESOURCE_PHASES = (
    "idle", "key", "connection", "challenge", "issuance", "renewal",
    "certificate_switch", "cleanup", "certificate_download", "released",
)
_RESOURCE_RE = re.compile(
    rb"I \([0-9]{1,10}\) katmed_acme_resource: schema=2 "
    rb"variant=(baseline|client) cycle=([1-3]) seq=([1-9][0-9]{0,3}) "
    rb"phase=(idle|key|connection|challenge|issuance|renewal|certificate_switch|cleanup|certificate_download|released) "
    rb"status=(running|ok|error) uptime_ms=([0-9]{1,20}) "
    rb"internal_free=([0-9]{1,10}) internal_min=([0-9]{1,10}) "
    rb"internal_largest=([0-9]{1,10}) dma_free=([0-9]{1,10}) "
    rb"dma_min=([0-9]{1,10}) dma_largest=([0-9]{1,10}) monitor_valid=([01])"
)

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


def evaluate_resource(raw: bytes, variant: str, cleanup_tolerance: int = 0) -> dict:
    """Qualify a bounded numeric source capture, never a hardware RAM pass."""
    if variant not in ("baseline", "client") or not 0 <= cleanup_tolerance <= 65536:
        raise RamBenchError("Ungültiger Varianten- oder Cleanup-Vertrag")
    if len(raw) > MAX_INPUT_BYTES or len(raw.splitlines()) > MAX_INPUT_LINES or not raw:
        raise RamBenchError("ACME-Ressourceneingabe ist leer oder überschreitet die Grenze")
    cycles: dict[int, list[dict]] = {1: [], 2: [], 3: []}
    previous_uptime = -1
    previous_cycle = 1
    for line in raw.splitlines():
        if b"\r" in line:
            raise RamBenchError("Ungültiges Zeilenende")
        if b"katmed_acme_resource:" not in line:
            continue
        match = _RESOURCE_RE.fullmatch(line)
        if match is None:
            raise RamBenchError("ACME-Ressourcenbeleg ist beschädigt")
        fields = match.groups()
        item = {
            "variant": fields[0].decode("ascii"),
            "cycle": int(fields[1]), "seq": int(fields[2]),
            "phase": fields[3].decode("ascii"),
            "status": fields[4].decode("ascii"),
            "uptime_ms": int(fields[5]),
            "internal_free": int(fields[6]), "internal_min": int(fields[7]),
            "internal_largest": int(fields[8]), "dma_free": int(fields[9]),
            "dma_min": int(fields[10]), "dma_largest": int(fields[11]),
            "monitor_valid": int(fields[12]),
        }
        if item["variant"] != variant:
            raise RamBenchError("Variantenbeleg passt nicht zum gewählten Vertrag")
        if item["uptime_ms"] > 0xFFFFFFFFFFFFFFFF or item["uptime_ms"] < previous_uptime:
            raise RamBenchError("Ressourcenzeit ist ungültig oder rückläufig")
        previous_uptime = item["uptime_ms"]
        if item["cycle"] < previous_cycle or item["cycle"] > previous_cycle + 1:
            raise RamBenchError("Ressourcenzyklen sind nicht fortlaufend")
        previous_cycle = item["cycle"]
        for key in ("internal_free", "internal_min", "internal_largest",
                    "dma_free", "dma_min", "dma_largest"):
            if item[key] > UINT32_MAX:
                raise RamBenchError("Ressourcenwert überschreitet die Byte-Grenze")
        if (item["internal_min"] > item["internal_free"] or
            item["dma_min"] > item["dma_free"] or
            item["internal_largest"] > item["internal_free"] or
            item["dma_largest"] > item["dma_free"] or
            (item["internal_free"] > 0 and item["internal_largest"] == 0) or
            (item["dma_free"] > 0 and item["dma_largest"] == 0) or
            item["monitor_valid"] != 1):
            raise RamBenchError("Heap-Messung ist unvollständig oder widersprüchlich")
        samples = cycles[item["cycle"]]
        if item["seq"] != len(samples) + 1 or len(samples) >= 1024:
            raise RamBenchError("Ressourcenfolge ist lückenhaft oder überlang")
        samples.append(item)
    if not all(cycles.values()):
        raise RamBenchError("Mehrere vollständige Zyklen fehlen")
    observed: set[str] = set()
    missing_by_cycle: dict[str, list[str]] = {}
    minima = {key: UINT32_MAX for key in (
        "internal_free", "internal_min", "internal_largest",
        "dma_free", "dma_min", "dma_largest")}
    first_idle = cycles[1][0]
    for cycle, samples in cycles.items():
        if samples[0]["phase"] != "idle" or samples[-1]["phase"] != "released":
            raise RamBenchError(f"Zyklus {cycle} ohne Idle oder bestätigte Workerfreigabe")
        if any(s["status"] != "running" for s in samples[:-1]):
            raise RamBenchError(f"Zyklus {cycle} enthält Fehler oder vorzeitiges Ende")
        if samples[-1]["status"] != "ok":
            raise RamBenchError(f"Zyklus {cycle} wurde abgebrochen")
        phases = {s["phase"] for s in samples}
        if variant == "baseline" and [s["phase"] for s in samples] != ["idle", "idle", "cleanup", "released"]:
            raise RamBenchError("Baseline enthält unerwartete Clientphasen")
        if variant == "client":
            required_cycle = {"idle", "connection", "challenge", "issuance",
                              "certificate_switch", "certificate_download", "cleanup", "released"}
            required_cycle.add("key" if cycle == 1 else "renewal")
            missing_by_cycle[str(cycle)] = sorted(required_cycle - phases)
            # Connection hooks may nest in challenge/issuance, and pending
            # challenge or issuance may retry. They cannot reopen a completed
            # download/switch/cleanup. Check every observation, not just firsts.
            rank = {"idle": 0, "key": 1, "renewal": 1, "challenge": 2,
                    "issuance": 3, "certificate_download": 4,
                    "certificate_switch": 5, "cleanup": 6, "released": 7}
            stage = 0
            connected = False
            retry_connection = False
            for sample in samples:
                phase = sample["phase"]
                if phase == "connection":
                    if stage >= 4:
                        raise RamBenchError("Verbindung nach Downloadfenster")
                    connected = True
                    retry_connection = True
                    continue
                if phase == "challenge" and not connected:
                    raise RamBenchError("Challenge ohne vorherige Verbindung")
                next_stage = rank[phase]
                # An invalid order can create a new order via a connection,
                # then retry its challenge/finalization before any download.
                if stage == 3 and next_stage == 2 and retry_connection:
                    stage = 2
                if next_stage < stage:
                    raise RamBenchError(f"Spätere widersprüchliche Phase in Zyklus {cycle}")
                stage = next_stage
                retry_connection = False
        idle, cleanup = samples[0], samples[-1]
        for key in ("internal_free", "internal_largest", "dma_free", "dma_largest"):
            if (cleanup[key] + cleanup_tolerance < idle[key] or
                idle[key] + cleanup_tolerance < first_idle[key] or
                cleanup[key] + cleanup_tolerance < first_idle[key]):
                raise RamBenchError(f"Cleanup-Verlust in Zyklus {cycle}: {key}")
        observed.update(s["phase"] for s in samples)
        for sample in samples:
            for key in minima:
                minima[key] = min(minima[key], sample[key])
    expected = set(RESOURCE_PHASES if variant == "client" else ("idle", "cleanup", "released"))
    missing = sorted(expected - observed)
    complete = not missing and not any(missing_by_cycle.values())
    return {
        "schema": 2, "qualification": "source_capture_valid" if complete else "incomplete",
        "variant": variant, "cycles": 3, "samples": sum(map(len, cycles.values())),
        "observed_phases": sorted(observed), "missing_phases": missing,
        "missing_by_cycle": missing_by_cycle,
        "cleanup_tolerance_bytes": cleanup_tolerance,
        "minima_bytes": minima, "hardware_pass": False,
        "certificate_file_boundary": "DownloadCertificate entry through return; certificate_switch remains separate successful commit hook",
        "cleanup_boundary": "released snapshot after monitor stop and synchronous deletion of suspended worker",
        "largest_block_boundary": "discrete snapshots including download entry, network return, write, commit and parse; not a continuous block minimum",
    }


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
    parser.add_argument("--mode", choices=("legacy", "resource"), default="legacy")
    parser.add_argument("--variant", choices=("baseline", "client"))
    parser.add_argument("--cleanup-tolerance-bytes", type=int, default=0)
    args = parser.parse_args(argv)
    try:
        raw = args.input.read_bytes()
        if args.mode == "resource":
            if args.variant is None:
                raise RamBenchError("Ressourcenvariante fehlt")
            summary = evaluate_resource(raw, args.variant, args.cleanup_tolerance_bytes)
        else:
            if args.variant is not None or args.cleanup_tolerance_bytes != 0:
                raise RamBenchError("Legacy-Vertrag akzeptiert keine Ressourcenoptionen")
            summary = evaluate(raw)
        _write_summary(args.output, summary)
    except (OSError, RamBenchError) as exc:
        print(f"ACME-RAM-Prüfstand abgebrochen: {exc}", file=sys.stderr)
        return 1
    if summary["qualification"] == "incomplete":
        print("ACME-RAM-Prüfstand: fehlende Phasen: " +
              json.dumps(summary["missing_by_cycle"], sort_keys=True), file=sys.stderr)
        return 1
    print("ACME-RAM-Prüfstand: dateibasierter Quellenvertrag qualifiziert; kein Gerätepass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
