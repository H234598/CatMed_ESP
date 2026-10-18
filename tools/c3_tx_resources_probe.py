#!/usr/bin/env python3
"""Portfreie Vorlage: erst separat aktivierte, ELF-geprüfte Bindung erlaubt Open.

Der bewährte private DHCP-Reader wird hashgebunden wiederverwendet. Diese Hülle
verändert weder Gerätezugriff noch Framing-, Identitäts- oder Uptimeguards.
"""
from __future__ import annotations
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import sys


def verified(entry):
    path = Path(entry["path"])
    if not path.is_absolute() or hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
        raise ValueError("Readerbindung stimmt nicht")
    return path


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def extend_reader(base):
    original = base.BoundSession
    # The reused session inherits its original module's framing implementation.
    original_error = original.__mro__[1]._line.__globals__["SetupError"]

    class ResourceSession(original):
        def _line(self):
            try:
                body = super()._line()
            except original_error as exc:
                raise base.km.SetupError(str(exc)) from None
            if body is None or not self.identity_checked:
                return body
            parsed = base.km._evidence_line(body)
            if b"katmed_txres:" in body[:64] and parsed is None:
                raise base.km.SetupError("TX-Ressourcenrecord ist ungültig")
            if parsed is None or parsed[0] != "txres":
                return body
            fields = parsed[1]
            if (self.last_uptime_ms is None or fields["uptime_ms"] != self.last_uptime_ms or
                    fields["boot_domain"] != self.evidence_domain):
                raise base.km.SetupError("TX-Ressourcenrecord ist nicht gebunden")
            dhcp = self.result.get("last_dhcp")
            if dhcp is None or any(fields[k] != dhcp[k] for k in
                                   ("sample_seq", "uptime_ms", "boot_domain")):
                raise base.km.SetupError("TX-Ressourcenrecord passt nicht zur vorangehenden DHCPprobe")
            previous = self.result.get("last_txres")
            if previous is not None and (fields["sample_seq"] <= previous["sample_seq"] or
                                         fields["count"] < previous["count"]):
                raise base.km.SetupError("TX-Ressourcenfolge ist rückläufig oder doppelt")
            self.result["txres_count"] = self.result.get("txres_count", 0) + 1
            self.result.setdefault("first_txres", dict(fields))
            self.result["last_txres"] = dict(fields)
            return body

    base.BoundSession = ResourceSession


def finish(result):
    last = result.get("last_txres")
    if result.get("txres_count", 0) < 2 or last is None:
        raise ValueError("TX-Ressourcenfolge fehlt")
    for key in ("sample_seq", "uptime_ms", "boot_domain"):
        if last[key] != result["last_state"][key]:
            raise ValueError("TX-Ressourcenfolge passt nicht zum letzten State")
    if not last["task_v"] or not last["v"]:
        result["allocation_evidence"] = "unbound_or_no_target"
    elif last["calls"] == 0:
        result["allocation_evidence"] = "no_call_branch_unproven"
    elif last["calls"] != 1 or not last["resource_v"]:
        result["allocation_evidence"] = "multiple_or_wrong_caps"
    else:
        result["allocation_evidence"] = "dma_null" if last["nulls"] == 1 else "dma_success"


def main(binding_path):
    # No serial import or Open occurs until all immutable bindings and the
    # separately activated device-phase gate have passed.
    started = False
    try:
        binding = json.loads(Path(binding_path).read_text())
        if binding["device_phase_enabled"] is not True:
            raise ValueError("Gerätephase ist nicht aktiviert")
        if (re.fullmatch(r"[a-f0-9]{7}", binding["firmware"]) is None or
                re.fullmatch(r"[a-f0-9]{9}", binding["build"]) is None):
            raise ValueError("Firmware-/Buildbindung fehlt")
        verified(binding["elf"])
        peer = json.loads(verified(binding["peer_binding"]).read_text())
        if (peer["elf_sha256"] != binding["elf"]["sha256"] or
                peer["callsite_bound"] is not True or peer["scope_reviewed"] is not True or
                peer["firmware"] != binding["firmware"] or peer["build"] != binding["build"] or
                binding["build"] != binding["elf"]["sha256"][:9]):
            raise ValueError("Unabhängige finale ELFfreigabe fehlt")
        current_reader = verified(binding["reader"])
        if current_reader != Path(__file__).resolve():
            raise ValueError("Aktuelle Readerhülle ist nicht gebunden")
        for key in ("reader", "base_reader", "base_source", "candidate_source"):
            if peer[key + "_sha256"] != binding[key]["sha256"]:
                raise ValueError("Unabhängige Quellen-/Readerbindung fehlt")
        base_path = verified(binding["base_reader"])
        old_source = verified(binding["base_source"])
        candidate_source = verified(binding["candidate_source"])
        if old_source != base_path.parent.parent / "tools/katmed_usb_setup.py":
            raise ValueError("Originalimport des wiederverwendeten Readers ist ungebunden")
        base = module("katmed_existing_dhcp_reader", base_path)
        base.km = module("katmed_tx_resource_source", candidate_source)
        base.FIRMWARE, base.BUILD = binding["firmware"], binding["build"]
        extend_reader(base)
        output = io.StringIO()
        started = True
        with contextlib.redirect_stdout(output):
            code = base.main()
        result = json.loads(output.getvalue())
        if code == 0:
            try:
                finish(result)
            except ValueError as exc:
                result.update(result="stopped", failure=str(exc))
                code = 2
        print(json.dumps(result, sort_keys=True, separators=(",", ":")))
        return code
    except Exception:
        print(json.dumps({"result": "stopped", "failure": "binding_or_activation_failed",
                          "open_count": None if started else 0}))
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) == 2 else ""))
