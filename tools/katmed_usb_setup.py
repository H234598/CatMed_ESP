#!/usr/bin/env python3
"""Bounded KM1 USB provisioning client. Never prints configuration contents."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import secrets
import select
import stat
import sys
import time
from typing import Callable, Protocol


ROOT = Path(__file__).resolve().parents[1]
PRIVATE_DIR = ROOT / ".local"
DEFAULT_CONFIG = PRIVATE_DIR / "usb-setup.json"
MAX_CONFIG_BYTES = 4096
MAX_REQUEST_BYTES = 512
MAX_RESPONSE_BYTES = 256
MAX_DIAG_CHARS = 767
MAX_SESSION_SECONDS = 1800
MAX_SESSION_INPUT_BYTES = 512 * 1024
MAX_SESSION_LINES = 4096
MAX_SESSION_OUTPUT_LINES = 1024
MAX_SESSION_OUTPUT_BYTES = 128 * 1024
SESSION_REPLY_SECONDS = 8.0
SESSION_IDLE_SILENCE_SECONDS = 30.0
STARTUP_DRAIN_SECONDS = 5.0
MAX_STARTUP_BYTES = 8192
STARTUP_CHUNK_BYTES = 256
DEVICE_RE = re.compile(r"[0-9a-fA-F]{12}\Z")
HEX32_RE = re.compile(r"[0-9a-fA-F]{32}\Z")
HEX64_RE = re.compile(r"[0-9a-fA-F]{64}\Z")
TOKEN_RE = re.compile(r"[A-Za-z0-9_.+-]+\Z")
WIRE_ERRORS = frozenset({"BUSY", "COMMAND", "COMMIT", "CONFIG", "FORMAT", "LENGTH", "OUTPUT", "STATUS", "UNAVAILABLE", "VERSION"})
STORE_ERRORS = frozenset({"OK", "NOT_FOUND", "INVALID", "CORRUPT", "UNSUPPORTED", "FULL", "IO", "COMMIT_UNKNOWN", "DUPLICATE", "STALE", "UNAVAILABLE", "UNKNOWN"})
METRIC_KEYS = (
    "uptime_ms", "pack_hash_us", "pack_valid", "variant_hash_us", "variant_valid",
    "jpeg_decode_us", "jpeg_valid", "blink_cycle_us", "blink_valid",
    "blink_pixels", "blink_segments", "blink_cycles", "blink_changes",
    "pack_status", "variant_status", "jpeg_status", "internal_min_free",
    "internal_largest_block", "main_stack_hwm_bytes", "lvgl_stack_hwm_bytes",
    "lvgl_stack_valid",
)
SIGNED_METRICS = frozenset({"pack_status", "variant_status", "jpeg_status"})
STATE_KEYS = (
    "schema", "sample_seq", "uptime_ms", "boot_domain", "round_d", "round_e",
    "round_s", "round_o", "alarm", "reason", "origin_d", "origin_e",
    "origin_s", "start_d", "start_ms", "start_v", "start_utc",
    "clock_d", "due_r", "due_e", "due_local", "due_rv", "due_ru",
    "due_ev", "due_eu", "now_v", "now_utc", "now_q", "utc_source",
    "image_id", "image_rem", "image_pending",
)
HEALTH_KEYS = (
    "schema", "sample_seq", "uptime_ms", "boot_domain", "image_v", "image_fail",
    "store_v", "store_read", "store_load", "store_commit", "mesh_v",
    "mesh_send", "mesh_queue", "mesh_level", "mesh_ip_got", "mesh_ip_lost",
    "mesh_up", "mesh_down", "mesh_link", "mesh_root", "mesh_root_ip",
)
NET_KEYS = (
    "schema", "sample_seq", "uptime_ms", "boot_domain",
    "sta_v", "sta_up", "sta_ip", "sta_dhcp",
)
DHCP_KEYS = (
    "schema", "sample_seq", "uptime_ms", "boot_domain", "core_v", "dhcp_v",
    "link_up", "state", "tries", "timeout", "fine",
    "udp_discover", "udp_request", "udp_other", "udp_invalid", "udp_ok", "udp_fail", "udp_error",
    "tx_discover", "tx_request", "tx_other", "tx_invalid", "tx_ok", "tx_fail", "tx_error",
)
TXRES_KEYS = (
    "schema", "sample_seq", "uptime_ms", "boot_domain", "task_v", "v", "count",
    "dhcp_type", "calls", "ok", "nulls", "resource_v", "size", "caps",
    "dma_free", "dma_largest", "result",
)
USB_PHASE_KEYS = (
    "schema", "phase_seq", "uptime_ms", "boot_domain", "task", "read",
    "status", "queue_ok", "queue_drop", "main_take", "reply_ready",
    "reply_call",
)
USB_PHASE_COUNTERS = USB_PHASE_KEYS[6:]
SIGNED_EVIDENCE = frozenset({"start_utc", "due_ru", "due_eu", "now_utc", "udp_error", "tx_error", "result"})
BOOLEAN_EVIDENCE = frozenset({
    "alarm", "start_v", "due_local", "due_rv", "due_ev", "now_v",
    "task_v", "v", "resource_v", "image_pending", "sta_v", "sta_up", "sta_ip", "core_v", "dhcp_v", "link_up", "fine",
    "image_v", "store_v", "mesh_v", "mesh_link", "mesh_root", "mesh_root_ip",
})
BOOT_MARKERS = (b"ESP-ROM:", b"rst:", b"ets ", b"boot:0x")


class SetupError(Exception):
    """Sanitized error suitable for display without provisioning secrets."""


class SerialPort(Protocol):
    def read(self, size: int = 1) -> bytes: ...
    def reset_input_buffer(self) -> None: ...
    def write(self, data: bytes) -> int: ...
    def flush(self) -> None: ...
    def read_until(self, expected: bytes = b"\n", size: int | None = None) -> bytes: ...
    def close(self) -> None: ...


def _private_config_bytes(path: Path) -> bytes:
    # A fixed ignored directory also keeps an accidentally supplied repository
    # path from becoming the source of a secret-bearing provisioning request.
    if path.parent.absolute() != PRIVATE_DIR or path.name in ("", ".", ".."):
        raise SetupError("Konfigurationsdatei muss direkt unter .local/ liegen")
    try:
        dir_fd = os.open(PRIVATE_DIR, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            directory = os.fstat(dir_fd)
            if not stat.S_ISDIR(directory.st_mode) or directory.st_uid != os.getuid():
                raise SetupError("Privates Verzeichnis ist nicht sicher")
            if directory.st_mode & 0o077:
                raise SetupError("Privates Verzeichnis benötigt höchstens Rechte 0700")
            fd = os.open(path.name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW, dir_fd=dir_fd)
            try:
                info = os.fstat(fd)
                if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
                    raise SetupError("Konfigurationsdatei ist nicht sicher")
                if info.st_mode & 0o077:
                    raise SetupError("Konfigurationsdatei benötigt höchstens Rechte 0600")
                if info.st_size > MAX_CONFIG_BYTES:
                    raise SetupError("Konfigurationsdatei ist zu groß")
                data = os.read(fd, MAX_CONFIG_BYTES + 1)
                if len(data) > MAX_CONFIG_BYTES:
                    raise SetupError("Konfigurationsdatei ist zu groß")
                return data
            finally:
                os.close(fd)
        finally:
            os.close(dir_fd)
    except SetupError:
        raise
    except OSError:
        raise SetupError("Private Konfigurationsdatei ist nicht lesbar") from None


def _hex_bytes(value: object, label: str, minimum: int, maximum: int, ascii_only: bool = False) -> str:
    if not isinstance(value, str):
        raise SetupError(f"{label}: Textwert erwartet")
    try:
        raw = value.encode("ascii" if ascii_only else "utf-8")
    except UnicodeError:
        raise SetupError(f"{label}: ungültige Zeichen") from None
    if not minimum <= len(raw) <= maximum:
        raise SetupError(f"{label}: ungültige Bytelänge")
    if ascii_only and any(c < 0x21 or c > 0x7e for c in raw):
        raise SetupError(f"{label}: nur druckbare ASCII-Zeichen ohne Leerzeichen")
    if ascii_only and not re.fullmatch(r"[A-Za-z0-9.-]+", value):
        raise SetupError(f"{label}: nur Buchstaben, Ziffern, Punkt und Bindestrich")
    if not ascii_only and any(c < 0x20 or c == 0x7f for c in raw):
        raise SetupError(f"{label}: Steuerzeichen sind nicht erlaubt")
    return raw.hex()


def build_provision_request(config_bytes: bytes) -> bytes:
    def unique_fields(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                raise SetupError("Konfiguration enthält doppelte Felder")
            result[key] = value
        return result

    try:
        cfg = json.loads(config_bytes.decode("utf-8"), object_pairs_hook=unique_fields)
    except SetupError:
        raise
    except (UnicodeError, ValueError, RecursionError):
        raise SetupError("Konfigurationsdatei enthält kein gültiges UTF-8-JSON") from None
    if not isinstance(cfg, dict) or set(cfg) != {"group", "secret", "ssid", "password", "time_source"}:
        raise SetupError("Konfiguration benötigt genau group, secret, ssid, password und time_source")
    group = cfg["group"]
    secret = cfg["secret"]
    if not isinstance(group, str) or not HEX32_RE.fullmatch(group):
        raise SetupError("group benötigt genau 32 Hexzeichen")
    if not isinstance(secret, str) or not HEX64_RE.fullmatch(secret):
        raise SetupError("secret benötigt genau 64 Hexzeichen")
    if int(group, 16) == 0 or int(secret, 16) == 0:
        raise SetupError("group und secret dürfen nicht nur aus Nullen bestehen")
    if cfg["ssid"] == "" and cfg["password"] == "":
        # An empty pair asks the firmware's normal PROVISION transaction to
        # commit its private build-time WLAN defaults.
        ssid = password = ""
    elif cfg["ssid"] == "" or cfg["password"] == "":
        raise SetupError("ssid und password müssen beide leer oder beide gesetzt sein")
    else:
        ssid = _hex_bytes(cfg["ssid"], "ssid", 1, 32)
        password = _hex_bytes(cfg["password"], "password", 8, 63)
    source = _hex_bytes(cfg["time_source"], "time_source", 1, 63, ascii_only=True)
    line = (
        f"KM1 PROVISION group={group.lower()} secret={secret.lower()} "
        f"ssid={ssid} password={password} time={source}\n"
    ).encode("ascii")
    if len(line) > MAX_REQUEST_BYTES:
        raise SetupError("Einrichtungsanforderung überschreitet 512 Bytes")
    return line


def _read_protocol_line(port: SerialPort) -> str:
    # Use the existing diagnostic framing bound for the shared console stream;
    # only KM1 replies have the smaller protocol bound. Keep the 16-line budget.
    # Never print raw lines: a faulty target could echo a provisioning request.
    for _ in range(16):
        raw = port.read_until(b"\n", MAX_DIAG_CHARS + 2)
        if not raw or len(raw) > MAX_DIAG_CHARS + 2 or not raw.endswith(b"\n"):
            raise SetupError("USB-Antwort fehlt, ist zu lang oder unvollständig")
        payload = raw[:-1]
        if payload.endswith(b"\r"):
            payload = payload[:-1]
        if b"\r" in payload or len(payload) > MAX_DIAG_CHARS:
            raise SetupError("USB-Zeile enthält ein ungültiges Ende oder ist zu lang")
        if not (raw.startswith(b"KM1 OK ") or raw.startswith(b"KM1 ERR ")):
            continue
        if len(raw) > MAX_RESPONSE_BYTES:
            raise SetupError("KM1-Antwort überschreitet 256 Bytes")
        try:
            return payload.decode("ascii")
        except UnicodeError:
            raise SetupError("USB-Antwort ist kein ASCII") from None
    raise SetupError("Keine KM1-Antwort innerhalb der begrenzten Ausgabe")


def _drain_startup(port: SerialPort, clock: Callable[[], float] = time.monotonic,
                   phase_observer: Callable[[dict[str, int]], None] | None = None,
                   rejection_observer: Callable[[str], None] | None = None) -> None:
    """Discard bounded boot output, optionally retaining valid numeric phases."""
    deadline = clock() + STARTUP_DRAIN_SECONDS
    consumed = 0
    pending = bytearray()
    overlong = False
    phase_tag = b"katmed_usb_phase:"
    tag_tail = bytearray()
    known_phase = False
    while clock() < deadline:
        remaining = MAX_STARTUP_BYTES - consumed
        if remaining == 0:
            if phase_observer is not None:
                raise SetupError("USB-Startausgabe überschreitet die sichere Grenze")
            # Discard-only mode stops reading at the unchanged byte cap.
            # Keep the original pre-command deadline, then use the purge below.
            time.sleep(max(0.0, deadline - clock()))
            break
        request_size = min(STARTUP_CHUNK_BYTES, remaining)
        try:
            chunk = port.read(request_size)
        except SetupError:
            raise
        except Exception:
            raise SetupError("USB-Startausgabe konnte nicht sicher verworfen werden") from None
        if not isinstance(chunk, bytes) or len(chunk) > request_size:
            raise SetupError("Ungültige USB-Startausgabe")
        consumed += len(chunk)
        if phase_observer is not None:
            for byte in chunk:
                if byte == 10:  # LF ends one bounded diagnostic line.
                    body = bytes(pending)
                    if body.endswith(b"\r"):
                        body = body[:-1]
                    if known_phase:
                        if overlong:
                            if rejection_observer is not None:
                                rejection_observer("drain_line_overlong")
                            raise SetupError("Numerischer KM1-Phasenbeleg ist beschädigt")
                        if b"\r" in body:
                            if rejection_observer is not None:
                                rejection_observer("drain_internal_cr")
                            raise SetupError("Numerischer KM1-Phasenbeleg ist beschädigt")
                        phase = _usb_phase_line(body, rejection_observer)
                        if phase is None:
                            raise SetupError("Numerischer KM1-Phasenbeleg ist beschädigt")
                        phase_observer(phase)
                    pending.clear()
                    overlong = False
                    tag_tail.clear()
                    known_phase = False
                else:
                    tag_tail.append(byte)
                    if len(tag_tail) > len(phase_tag):
                        del tag_tail[0]
                    if tag_tail == phase_tag:
                        known_phase = True
                    if not overlong:
                        if len(pending) < MAX_DIAG_CHARS + 1:
                            pending.append(byte)
                        else:
                            overlong = True
    if phase_observer is not None and known_phase:
        if rejection_observer is not None:
            rejection_observer("drain_incomplete")
        raise SetupError("Numerischer KM1-Phasenbeleg ist unvollständig")
    # No request has been sent yet. Discard a possible partial boot line so
    # it cannot merge with the first KM1 response.
    try:
        port.reset_input_buffer()
    except SetupError:
        raise
    except Exception:
        raise SetupError("USB-Startausgabe konnte nicht sicher verworfen werden") from None


def exchange(port: SerialPort, request: bytes, expected: str,
             reader: Callable[[SerialPort], str] = _read_protocol_line) -> str:
    if not request.endswith(b"\n") or len(request) > MAX_REQUEST_BYTES or not request.isascii():
        raise SetupError("Ungültige Anforderungsgrenze")
    try:
        written = port.write(request)
        if written != len(request):
            raise SetupError("USB-Anforderung wurde nur teilweise gesendet")
        port.flush()
        reply = reader(port)
    except SetupError:
        raise
    except Exception:
        # Serial exceptions can contain the transmitted byte string. Do not
        # surface their arguments on any provisioning/error path.
        raise SetupError("USB-Übertragung fehlgeschlagen") from None
    if reply.startswith("KM1 ERR "):
        reason = reply.removeprefix("KM1 ERR ")
        if reason not in WIRE_ERRORS:
            raise SetupError("Gerät meldet einen ungültigen Fehlercode")
        raise SetupError(f"Gerät lehnt die Anforderung ab: {reason}")
    if reply == expected or (expected == "KM1 OK STATUS " and reply.startswith(expected)):
        return reply
    raise SetupError("Unerwartete USB-Antwort")


def read_status(port: SerialPort,
                reader: Callable[[SerialPort], str] = _read_protocol_line) -> dict[str, str]:
    reply = exchange(port, b"KM1 STATUS\n", "KM1 OK STATUS ", reader)
    fields: dict[str, str] = {}
    for token in reply.removeprefix("KM1 OK STATUS ").split(" "):
        if token.count("=") != 1:
            raise SetupError("Status enthält ein ungültiges Feld")
        key, value = token.split("=", 1)
        if key in fields or not TOKEN_RE.fullmatch(value):
            raise SetupError("Status enthält ein doppeltes oder ungültiges Feld")
        fields[key] = value
    required = {"configured", "durable", "pending", "clock", "device", "catalog", "firmware", "build", "error"}
    if set(fields) != required:
        raise SetupError("Status enthält nicht die vereinbarten Felder")
    if fields["configured"] not in {"0", "1"} or fields["durable"] not in {"0", "1"}:
        raise SetupError("Status enthält ungültige Zustandswerte")
    if fields["clock"] not in {"0", "1", "2"}:
        raise SetupError("Status enthält einen ungültigen Uhrwert")
    if not fields["pending"].isdigit() or int(fields["pending"]) > 255:
        raise SetupError("Status enthält eine ungültige Warteschlangenlänge")
    if not DEVICE_RE.fullmatch(fields["device"]) or not HEX64_RE.fullmatch(fields["catalog"]):
        raise SetupError("Status enthält eine ungültige Geräte- oder Katalogkennung")
    if fields["error"] not in STORE_ERRORS:
        raise SetupError("Status enthält einen unbekannten Fehlercode")
    return fields


def _metric_line(raw: bytes) -> tuple[str, int] | None:
    """Return only the fixed numeric SDK diagnostic, never arbitrary log text."""
    match = re.fullmatch(rb"I \([0-9]{1,10}\) katmed_metrics: (.+)", raw)
    if match is None:
        return None
    tokens = match.group(1).split(b" ")
    if len(tokens) != len(METRIC_KEYS):
        return None
    values: list[str] = []
    for key, token in zip(METRIC_KEYS, tokens):
        expected = key.encode("ascii") + b"="
        if not token.startswith(expected):
            return None
        number = token[len(expected):]
        pattern = rb"-?[0-9]{1,20}" if key in SIGNED_METRICS else rb"[0-9]{1,20}"
        if re.fullmatch(pattern, number) is None:
            return None
        values.append(f"{key}={number.decode('ascii')}")
    uptime = int(values[0].removeprefix("uptime_ms="))
    if uptime > 0xffffffffffffffff:
        return None
    return "Messung " + " ".join(values), uptime


def _evidence_line(raw: bytes) -> tuple[str, dict[str, int]] | None:
    """Parse only the fixed, bounded numeric firmware records."""
    if len(raw) > MAX_DIAG_CHARS:
        return None
    match = re.fullmatch(rb"I \([0-9]{1,10}\) katmed_(state|health|net|dhcp|txres): (.+)", raw)
    if match is None:
        return None
    kind = match.group(1).decode("ascii")
    keys = {"state": STATE_KEYS, "health": HEALTH_KEYS, "net": NET_KEYS, "dhcp": DHCP_KEYS, "txres": TXRES_KEYS}[kind]
    tokens = match.group(2).split(b" ")
    if len(tokens) != len(keys):
        return None
    fields: dict[str, int] = {}
    for key, token in zip(keys, tokens):
        expected = key.encode("ascii") + b"="
        if not token.startswith(expected):
            return None
        value = token[len(expected):]
        signed = key in SIGNED_EVIDENCE
        if re.fullmatch(rb"-?[0-9]{1,20}" if signed else rb"[0-9]{1,20}", value) is None:
            return None
        number = int(value)
        if (number < (-1 << 63) if signed else number < 0) or number > ((1 << 63) - 1 if signed else (1 << 64) - 1):
            return None
        if key in BOOLEAN_EVIDENCE and number not in (0, 1):
            return None
        fields[key] = number
    if (fields["schema"] != 1 or fields["sample_seq"] == 0 or
            fields["boot_domain"] == 0):
        return None
    if kind == "state" and (fields["reason"] > 4 or fields["now_q"] > 2 or
                             fields["utc_source"] > 2 or
                             (fields["due_local"] and fields["clock_d"] != fields["boot_domain"])):
        return None
    if kind == "net" and (fields["sta_dhcp"] > 2 or
                            (not fields["sta_v"] and any(fields[k] for k in
                             ("sta_up", "sta_ip", "sta_dhcp")))):
        return None
    if kind == "txres":
        if (any(fields[k] > 0xffffffff for k in TXRES_KEYS[4:] if k != "result") or
                not -(1 << 31) <= fields["result"] < (1 << 31)):
            return None
        if not fields["v"]:
            if any(fields[k] for k in TXRES_KEYS[6:]):
                return None
        elif (not fields["task_v"] or fields["count"] == 0 or
              fields["dhcp_type"] not in (1, 3) or
              fields["ok"] + fields["nulls"] != fields["calls"]):
            return None
        if fields["calls"] != 1 and any(fields[k] for k in
                ("resource_v", "size", "caps", "dma_free", "dma_largest")):
            return None
        if fields["resource_v"] != int(fields["calls"] == 1 and fields["caps"] == 0x80c):
            return None
        if not fields["resource_v"] and (fields["dma_free"] or fields["dma_largest"]):
            return None
    if kind == "dhcp":
        if (fields["state"] > 12 or fields["tries"] > 255 or
                fields["timeout"] > 0xffffffff or
                any(fields[k] > 0xffffffff for k in DHCP_KEYS[11:] if k not in SIGNED_EVIDENCE) or
                any(not -(1 << 31) <= fields[k] < (1 << 31) for k in ("udp_error", "tx_error"))):
            return None
        if not fields["core_v"] and any(fields[k] for k in DHCP_KEYS[5:11]):
            return None
        if not fields["dhcp_v"] and any(fields[k] for k in ("state", "tries", "timeout", "fine")):
            return None
    for key in ("image_fail", "store_read", "store_load", "store_commit",
                "image_id", "mesh_send", "mesh_queue", "mesh_level", "mesh_ip_got",
                "mesh_ip_lost", "mesh_up", "mesh_down"):
        if key in fields and fields[key] > 0xffffffff:
            return None
    return kind, fields


def _usb_phase_line(raw: bytes, rejection_observer: Callable[[str], None] | None = None) -> dict[str, int] | None:
    """Accept only the bounded, fixed numeric product formatter output."""
    def reject(kind: str) -> None:
        if rejection_observer is not None and b"katmed_usb_phase:" in raw:
            rejection_observer(kind)

    if len(raw) > MAX_DIAG_CHARS:
        reject("envelope")
        return None
    match = re.fullmatch(rb"I \([0-9]{1,10}\) katmed_usb_phase: (.+)", raw)
    if match is None:
        reject("envelope")
        return None
    tokens = match.group(1).split(b" ")
    if len(tokens) != len(USB_PHASE_KEYS):
        reject("field_count")
        return None
    fields: dict[str, int] = {}
    for key, token in zip(USB_PHASE_KEYS, tokens):
        expected = key.encode("ascii") + b"="
        if not token.startswith(expected):
            reject("key_order")
            return None
        value = token[len(expected):]
        if re.fullmatch(rb"[0-9]{1,20}", value) is None:
            reject("number_syntax")
            return None
        number = int(value)
        limit = 0xffffffff if key in ("schema", "task", "read", *USB_PHASE_COUNTERS) else 0xffffffffffffffff
        if number > limit:
            reject("number_range")
            return None
        fields[key] = number
    if (fields["schema"] != 1 or fields["phase_seq"] == 0 or
            fields["boot_domain"] == 0):
        reject("schema_or_domain")
        return None
    if fields["task"] > 2 or fields["read"] > 1:
        reject("task_or_read_range")
        return None
    if (fields["queue_ok"] + fields["queue_drop"] > fields["status"] or
            fields["main_take"] > fields["queue_ok"] or
            fields["reply_ready"] > fields["main_take"] or
            fields["reply_call"] > fields["reply_ready"] or
            (fields["status"] and not fields["read"])):
        reject("counter_relation")
        return None
    return fields


class ObservationSession:
    """Single-port, bounded, read-only KM1 and diagnostic observation."""

    def __init__(self, port: SerialPort, *, clock: Callable[[], float],
                 emit: Callable[[str], None], duration_seconds: int):
        if not 30 <= duration_seconds <= MAX_SESSION_SECONDS:
            raise SetupError("Sitzungsdauer muss zwischen 30 und 1800 Sekunden liegen")
        self.port = port
        self.clock = clock
        self.emit = emit
        self.deadline = clock() + duration_seconds
        self.input_bytes = 0
        self.lines = 0
        self.output_lines = 0
        self.output_bytes = 0
        self.last_measure_time: float | None = None
        self.last_uptime_ms: int | None = None
        self.identity_checked = False
        self._line_buffer = bytearray()

    def _emit(self, line: str) -> None:
        encoded = line.encode("utf-8")
        if (self.output_lines >= MAX_SESSION_OUTPUT_LINES or
                self.output_bytes + len(encoded) + 1 > MAX_SESSION_OUTPUT_BYTES):
            raise SetupError("Begrenzte Sitzungsausgabe ist erschöpft")
        self.output_lines += 1
        self.output_bytes += len(encoded) + 1
        self.emit(line)

    def ensure_complete_line(self) -> None:
        if self._line_buffer:
            raise SetupError("USB-Zeile ist zu lang oder unvollständig")

    def _line(self) -> bytes | None:
        if self.clock() >= self.deadline:
            self.ensure_complete_line()
            return None
        self._check_measure_silence()
        try:
            remaining = MAX_DIAG_CHARS + 2 - len(self._line_buffer)
            raw = self.port.read_until(b"\n", remaining)
        except EOFError:
            raise SetupError("USB-Verbindung ist beendet") from None
        except Exception:
            raise SetupError("USB-Lesen fehlgeschlagen") from None
        if not isinstance(raw, bytes):
            raise SetupError("Ungültige USB-Ausgabe")
        self._check_measure_silence()
        if not raw:
            if self.clock() >= self.deadline:
                self.ensure_complete_line()
            return None  # A bounded serial timeout is not a device EOF.
        self.input_bytes += len(raw)
        if not self._line_buffer:
            self.lines += 1
        if self.input_bytes > MAX_SESSION_INPUT_BYTES or self.lines > MAX_SESSION_LINES:
            raise SetupError("USB-Ausgabe überschreitet die Sitzungsgrenze")
        if len(raw) > remaining:
            raise SetupError("USB-Zeile ist zu lang oder unvollständig")
        self._line_buffer.extend(raw)
        if not self._line_buffer.endswith(b"\n"):
            if len(self._line_buffer) >= MAX_DIAG_CHARS + 2:
                raise SetupError("USB-Zeile ist zu lang oder unvollständig")
            if self.clock() >= self.deadline:
                self.ensure_complete_line()
            return None
        raw = bytes(self._line_buffer)
        self._line_buffer.clear()
        body = raw[:-1]
        if body.endswith(b"\r"):
            body = body[:-1]
        if b"\r" in body or len(body) > MAX_DIAG_CHARS:
            raise SetupError("USB-Zeile enthält ein ungültiges Ende oder ist zu lang")
        if body.startswith(BOOT_MARKERS):
            raise SetupError("Geräteneustart erkannt; neue Sitzung mit Geräteabgleich erforderlich")
        if body.startswith(b"KM1 ") and len(raw) > MAX_RESPONSE_BYTES:
            raise SetupError("KM1-Antwort überschreitet 256 Bytes")
        return body

    def _check_measure_silence(self) -> None:
        if (self.last_measure_time is not None and
                self.clock() - self.last_measure_time > SESSION_IDLE_SILENCE_SECONDS):
            raise SetupError("Gültige numerische Diagnose blieb länger als 30 Sekunden aus")

    def _accept_metric(self, body: bytes) -> None:
        # Before the first verified device AND firmware status, even a valid
        # numeric line cannot be attributed to the intended target.
        if not self.identity_checked:
            return
        metric = _metric_line(body)
        if metric is None:
            return
        line, uptime = metric
        if self.last_uptime_ms is not None and uptime < self.last_uptime_ms:
            raise SetupError("Neustart oder neue Monotoniedomäne ohne neue Geräteprüfung")
        self.last_uptime_ms = uptime
        self.last_measure_time = self.clock()
        self._emit(line)

    def _status_reply(self) -> str:
        deadline = min(self.deadline, self.clock() + SESSION_REPLY_SECONDS)
        while self.clock() < deadline:
            body = self._line()
            if body is None:
                continue
            if body.startswith(b"KM1 "):
                try:
                    return body.decode("ascii")
                except UnicodeError:
                    raise SetupError("KM1-Antwort ist kein ASCII") from None
            self._accept_metric(body)
        raise SetupError("KM1-Statusantwort fehlt innerhalb von acht Sekunden")

    def status(self) -> dict[str, str]:
        fields = read_status(self.port, lambda _port: self._status_reply())
        # The generic KM1 parser permits token-safe build/version strings.
        # Observation prints them, so accept only the expected public forms.
        if (re.fullmatch(r"[0-9a-fA-F]{7,12}", fields["firmware"]) is None or
                re.fullmatch(r"[0-9a-fA-F]{6,24}", fields["build"]) is None):
            raise SetupError("Status enthält keine gültige öffentliche Buildkennung")
        if self.last_measure_time is None:
            self.last_measure_time = self.clock()
        return fields

    def emit_status(self, fields: dict[str, str]) -> None:
        self._emit("Status " + " ".join(f"{key}={value}" for key, value in fields.items()))

    def idle_read(self) -> None:
        body = self._line()
        if body is None:
            return
        if body.startswith(b"KM1 "):
            raise SetupError("Unerwartete KM1-Antwort außerhalb einer Statusabfrage")
        self._accept_metric(body)


def _stdin_command() -> str | None:
    try:
        ready, _, _ = select.select([sys.stdin], [], [], 0)
        if not ready:
            return None
        line = sys.stdin.readline()
        return "quit" if line == "" else line.strip().lower()
    except Exception:
        raise SetupError("Sitzungseingabe ist nicht verfügbar") from None


def run_observation(args: argparse.Namespace,
                    opener: Callable[[str], SerialPort] | None = None,
                    startup: Callable[[SerialPort], None] = _drain_startup,
                    command: Callable[[], str | None] = _stdin_command,
                    clock: Callable[[], float] = time.monotonic,
                    emit: Callable[[str], None] = print) -> None:
    if not args.expect_device or not DEVICE_RE.fullmatch(args.expect_device):
        raise SetupError("Beobachtung erfordert --expect-device mit 12 Hexzeichen")
    expected_firmware = getattr(args, "expect_firmware", None)
    if not isinstance(expected_firmware, str) or re.fullmatch(
            r"[0-9a-fA-F]{7,12}", expected_firmware) is None:
        raise SetupError("Beobachtung erfordert --expect-firmware mit 7 bis 12 Hexzeichen")
    if not 30 <= args.duration_seconds <= MAX_SESSION_SECONDS:
        raise SetupError("Sitzungsdauer muss zwischen 30 und 1800 Sekunden liegen")
    if opener is None:
        opener = lambda path: _open_serial(path, timeout=0.25)
    port = opener(args.port)
    try:
        startup(port)
        session = ObservationSession(port, clock=clock, emit=emit,
                                     duration_seconds=args.duration_seconds)
        status = session.status()
        if status["device"].lower() != args.expect_device.lower():
            raise SetupError("Gerätekennung stimmt nicht mit --expect-device überein")
        if status["firmware"].lower() != expected_firmware.lower():
            raise SetupError("Firmwarekennung stimmt nicht mit --expect-firmware überein")
        session.identity_checked = True
        session.emit_status(status)
        session._emit("Eingabe: status oder quit; der Port bleibt bis zum Ende geöffnet.")
        while clock() < session.deadline:
            requested = command()
            if requested == "quit":
                return
            if requested == "status":
                status = session.status()
                if status["device"].lower() != args.expect_device.lower():
                    raise SetupError("Gerätekennung hat sich während der Sitzung geändert")
                if status["firmware"].lower() != expected_firmware.lower():
                    raise SetupError("Firmwarekennung hat sich während der Sitzung geändert")
                session.emit_status(status)
            elif requested is not None:
                session._emit("Nur status oder quit ist zulässig.")
            session.idle_read()
        session.ensure_complete_line()
        session._emit("Die begrenzte USB-Testsitzung ist beendet.")
    except KeyboardInterrupt:
        return
    finally:
        try:
            port.close()
        except Exception:
            raise SetupError("USB-Port konnte nicht sicher geschlossen werden") from None


def _open_serial(port_name: str, *, timeout: float = 3.0) -> SerialPort:
    # Only a stable Linux by-id name may be used for device operations.
    path = Path(port_name)
    if path.parent != Path("/dev/serial/by-id") or not path.is_symlink():
        raise SetupError("Ein stabiler /dev/serial/by-id/-Gerätepfad ist erforderlich")
    try:
        import serial

        # Configure the desired idle lines while closed. Native USB Serial/JTAG
        # interprets DTR/RTS as boot controls; do not request their defaults.
        port = serial.Serial(port=None, baudrate=115200, timeout=timeout, write_timeout=3)
        port.dtr = False
        port.rts = False
        port.port = port_name
        port.open()
        return port
    except Exception:
        raise SetupError("USB-Port konnte nicht geöffnet werden") from None


def run_command(args: argparse.Namespace, opener: Callable[[str], SerialPort] = _open_serial,
                startup: Callable[[SerialPort], None] = _drain_startup) -> str:
    request: bytes | None = None
    expected_firmware = getattr(args, "expect_firmware", None)
    if expected_firmware is not None and (
            not isinstance(expected_firmware, str) or
            re.fullmatch(r"[0-9a-fA-F]{7,12}", expected_firmware) is None):
        raise SetupError("--expect-firmware erfordert 7 bis 12 Hexzeichen")
    if args.command == "provision":
        request = build_provision_request(_private_config_bytes(Path(args.config)))
    if args.command == "reset-config" and args.confirm_reset != "RESET CONFIG":
        raise SetupError("Reset erfordert --confirm-reset 'RESET CONFIG'")
    if args.command in {"provision", "reset-config"}:
        if not args.expect_device or not DEVICE_RE.fullmatch(args.expect_device):
            raise SetupError("Für Änderungen ist --expect-device mit 12 Hexzeichen erforderlich")
    port = opener(args.port)
    try:
        startup(port)
        status = read_status(port)
        if args.expect_device and status["device"].lower() != args.expect_device.lower():
            raise SetupError("Gerätekennung stimmt nicht mit --expect-device überein")
        if (expected_firmware is not None and
                status["firmware"].lower() != expected_firmware.lower()):
            raise SetupError("Firmwarekennung stimmt nicht mit --expect-firmware überein")
        if args.command == "status":
            return " ".join(f"{key}={value}" for key, value in status.items())
        if args.command == "provision":
            assert request is not None
            exchange(port, request, "KM1 OK PROVISION")
            return "Einrichtung dauerhaft angenommen."
        exchange(port, b"KM1 RESET CONFIG\n", "KM1 OK RESET_CONFIG")
        return "Nur Gruppenschlüssel, WLAN und Zeitquelle wurden zurückgesetzt; Zustand und Outbox bleiben erhalten."
    finally:
        try:
            port.close()
        except Exception:
            # Do not expose a transport exception that might include sent bytes.
            raise SetupError("USB-Port konnte nicht sicher geschlossen werden") from None


def _bound_nonce(factory: Callable[[], str]) -> str:
    value = factory()
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{32}", value) is None:
        raise SetupError("Interne Anfragekennung ist ungültig")
    return value


def _bound_fields(reply: str, kind: str, nonce: str,
                  *, expect_device: str, expect_firmware: str,
                  expect_build: str, expect_catalog: str) -> dict[str, int]:
    prefix = f"KM1 OK {kind} "
    if not reply.startswith(prefix):
        raise SetupError("Gebundene USB-Antwort fehlt")
    parts = reply[len(prefix):].split(" ")
    keys = (["n", "c", "d", "dev", "cat", "fw", "b", "e", "x", "u"]
            if kind == "BOOT" else ["n", "dev", "cat", "fw", "b", "e", "x", "u"])
    if len(parts) != len(keys):
        raise SetupError("Gebundene USB-Antwort ist unvollständig")
    values: dict[str, str] = {}
    for part, key in zip(parts, keys):
        if not part.startswith(key + "=") or part.count("=") != 1:
            raise SetupError("Gebundene USB-Antwort enthält ungültige Felder")
        values[key] = part[len(key) + 1:]
    if values["n"] != nonce:
        raise SetupError("Gebundene USB-Antwort gehört zu einer älteren Anfrage")
    if (values["dev"].lower() != expect_device.lower() or
            values["cat"].lower() != expect_catalog.lower() or
            values["fw"].lower() != expect_firmware.lower() or
            values["b"].lower() != expect_build.lower()):
        raise SetupError("Gebundene USB-Kennung weicht ab")
    if (not DEVICE_RE.fullmatch(values["dev"]) or
            not HEX64_RE.fullmatch(values["cat"]) or
            re.fullmatch(r"[0-9a-fA-F]{7,12}", values["fw"]) is None or
            re.fullmatch(r"[0-9a-fA-F]{6,24}", values["b"]) is None):
        raise SetupError("Gebundene USB-Kennung hat ein ungültiges Format")
    result: dict[str, int] = {}
    for key, name in (("e", "epoch"), ("x", "domain"), ("u", "uptime_ms")):
        raw = values[key]
        if re.fullmatch(r"[0-9]{1,20}", raw) is None or int(raw) > (1 << 64) - 1:
            raise SetupError("Gebundene USB-Zeitkennung ist ungültig")
        result[name] = int(raw)
    if result["epoch"] == 0 or result["domain"] == 0:
        raise SetupError("Gebundene USB-Bootkennung fehlt")
    if kind == "BOOT":
        if values["c"] not in ("0", "1") or values["d"] not in ("0", "1"):
            raise SetupError("Gebundener Speicherstatus ist ungültig")
        result["configured"] = int(values["c"])
        result["durable"] = int(values["d"])
    return result


def _bound_exchange(port: SerialPort, request: bytes) -> str:
    if not request.endswith(b"\n") or len(request) > MAX_REQUEST_BYTES or not request.isascii():
        raise SetupError("Ungültige interne Anforderungsgrenze")
    try:
        if port.write(request) != len(request):
            raise SetupError("Interne USB-Anforderung wurde nur teilweise gesendet")
        port.flush()
        reply = _read_protocol_line(port)
    except SetupError:
        raise
    except Exception:
        raise SetupError("Interne USB-Übertragung fehlgeschlagen") from None
    if reply.startswith("KM1 ERR "):
        reason = reply.removeprefix("KM1 ERR ")
        if reason not in WIRE_ERRORS:
            raise SetupError("Gerät meldet einen ungültigen Fehlercode")
        raise SetupError(f"Gerät lehnt die interne Anfrage ab: {reason}")
    return reply


def run_bound_provision(args: argparse.Namespace,
                        opener: Callable[[str], SerialPort] = _open_serial,
                        startup: Callable[[SerialPort], None] = _drain_startup,
                        nonce_factory: Callable[[], str] = lambda: secrets.token_hex(16)) -> dict[str, int]:
    """Private, one-shot commit witness; the public provision command is unchanged."""
    request = build_provision_request(_private_config_bytes(Path(args.config)))
    expected = (args.expect_device, args.expect_firmware, args.expect_build,
                args.expect_catalog)
    if (not DEVICE_RE.fullmatch(expected[0] or "") or
            re.fullmatch(r"[0-9a-fA-F]{7,12}", expected[1] or "") is None or
            re.fullmatch(r"[0-9a-fA-F]{6,24}", expected[2] or "") is None or
            not HEX64_RE.fullmatch(expected[3] or "")):
        raise SetupError("Interne erwartete Kennungen sind ungültig")
    nonce = _bound_nonce(nonce_factory)
    port = opener(args.port)
    try:
        startup(port)
        status = read_status(port)
        if tuple(status[key].lower() for key in ("device", "firmware", "build", "catalog")) != tuple(
                value.lower() for value in expected):
            raise SetupError("USB-Status weicht von der gebundenen Kennung ab")
        arm = _bound_exchange(port, f"KM1 ARM n={nonce}\n".encode("ascii"))
        if arm != f"KM1 OK ARM n={nonce}":
            raise SetupError("ARM-Antwort gehört nicht zur aktuellen Anfrage")
        internal = b"KM1 PROVEBOOT" + request[len(b"KM1 PROVISION"):]
        reply = _bound_exchange(port, internal)
        return _bound_fields(reply, "PROVEBOOT", nonce,
                             expect_device=expected[0], expect_firmware=expected[1],
                             expect_build=expected[2], expect_catalog=expected[3])
    finally:
        try:
            port.close()
        except Exception:
            raise SetupError("USB-Port konnte nicht sicher geschlossen werden") from None


def read_boot_pair(port: SerialPort, *, expect_device: str,
                   expect_firmware: str, expect_build: str, expect_catalog: str,
                   nonce_factory: Callable[[], str] = lambda: secrets.token_hex(16),
                   monotonic: Callable[[], float] = time.monotonic) -> dict[str, int]:
    """Bind one real BOOT status to the first complete numeric sample, on one port."""
    nonce = _bound_nonce(nonce_factory)
    _drain_startup(port, clock=monotonic)
    reply = _bound_exchange(port, f"KM1 BOOT n={nonce}\n".encode("ascii"))
    bound = _bound_fields(reply, "BOOT", nonce,
                          expect_device=expect_device, expect_firmware=expect_firmware,
                          expect_build=expect_build, expect_catalog=expect_catalog)
    if bound["configured"] != 1 or bound["durable"] != 1:
        raise SetupError("Gebundener Bootstatus ist nicht dauerhaft konfiguriert")
    bound["status_uptime_ms"] = bound.pop("uptime_ms")
    deadline = monotonic() + 30.0
    metric_uptime: int | None = None
    state: dict[str, int] | None = None
    count = 0
    total = 0
    while monotonic() < deadline and count < 64 and total < 32 * 1024:
        try:
            raw = port.read_until(b"\n", MAX_DIAG_CHARS + 2)
        except Exception:
            raise SetupError("Gebundene Diagnose konnte nicht gelesen werden") from None
        if not isinstance(raw, bytes):
            raise SetupError("Gebundene Diagnose ist ungültig")
        if not raw:
            continue
        count += 1
        total += len(raw)
        if len(raw) > MAX_DIAG_CHARS + 2 or not raw.endswith(b"\n"):
            raise SetupError("Gebundene Diagnose ist zu lang oder unvollständig")
        body = raw[:-1].removesuffix(b"\r")
        if b"\r" in body or body.startswith(BOOT_MARKERS) or body.startswith(b"KM1 "):
            raise SetupError("Gebundene Diagnose enthält einen Neustart oder Fremdstatus")
        recognized = body.startswith((b"I (",)) and any(
            tag in body[:64] for tag in (b"katmed_metrics:", b"katmed_state:", b"katmed_health:", b"katmed_net:", b"katmed_dhcp:", b"katmed_txres:"))
        parsed_metric = _metric_line(body)
        parsed_evidence = _evidence_line(body)
        if recognized and parsed_metric is None and parsed_evidence is None:
            raise SetupError("Gebundene numerische Diagnose ist beschädigt")
        if parsed_metric is not None:
            if metric_uptime is not None or state is not None or parsed_metric[1] <= bound["status_uptime_ms"]:
                raise SetupError("Erste Diagnose gehört nicht strikt zum gebundenen Status")
            metric_uptime = parsed_metric[1]
        elif parsed_evidence is not None:
            kind, fields = parsed_evidence
            if metric_uptime is None or fields["uptime_ms"] != metric_uptime or fields["boot_domain"] != bound["domain"]:
                raise SetupError("Diagnosepaar gehört zu einer anderen Zeitdomäne")
            if kind in ("net", "dhcp", "txres"):
                continue  # Validated direct-network evidence is not the core state/health pair.
            if kind == "state" and state is None:
                state = fields
            elif kind == "health" and state is not None and fields["sample_seq"] == state["sample_seq"]:
                bound["pair_uptime_ms"] = metric_uptime
                bound["pair_sequence"] = fields["sample_seq"]
                return bound
            else:
                raise SetupError("Erstes Diagnosepaar ist unvollständig oder doppelt")
    raise SetupError("Erstes gebundenes Diagnosepaar fehlt innerhalb der Grenze")


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Begrenzte USB-Einrichtung für Katmed (KM1).")
    p.add_argument("--port", required=True, help="Stabiler Pfad unter /dev/serial/by-id/")
    p.add_argument("--expect-device", help="Erwartete 12-stellige Gerätekennung")
    p.add_argument("--expect-firmware", help="Erwarteter Firmwarekurz-Hash mit 7 bis 12 Hexzeichen")
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("status", help="Nicht geheimen Gerätestatus lesen")
    provision = sub.add_parser("provision", help="Private Konfiguration dauerhaft übernehmen")
    provision.add_argument("--config", default=str(DEFAULT_CONFIG), help="JSON-Datei direkt unter .local/")
    reset = sub.add_parser("reset-config", help="Nur Einrichtungsdaten löschen")
    reset.add_argument("--confirm-reset", help="Wörtlich RESET CONFIG")
    observe = sub.add_parser("observe", help="Eine begrenzte durchgehende Lese- und Messsitzung")
    observe.add_argument("--duration-seconds", type=int, default=600,
                         help="Sitzungsdauer in Sekunden, 30 bis 1800")
    return p


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.command == "observe":
            run_observation(args)
            return 0
        print(run_command(args))
        return 0
    except SetupError as exc:
        print(f"Fehler: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
