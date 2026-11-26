"""Operator entry point for one explicitly configured certificate recipient."""

import argparse
import getpass
import json
import os
import stat
import sys
import warnings
from datetime import datetime, timezone
from pathlib import Path

from tools.katmed_certificate_delivery import ANCHOR_LIMIT, DeliveryError, Device, deliver, stage
from tools.katmed_certificate_transport import BootstrapHTTPS, BootstrapIdentity, DeviceHTTPS


CONFIG_FIELDS = {
    "identifier", "dns", "origin", "current_pin", "anchor_configured", "ca_pem_file",
    "lineage", "archive", "journal",
}
BOOTSTRAP_FIELDS = {"tls_name", "address", "leaf_sha256", "leaf_pem_file"}
DELIVERY_EXITS = {"active": 0, "ready": 3, "pending": 3, "uncertain": 4, "failed": 5}


class InvalidInput(ValueError):
    """The operation or configuration did not match the CLI contract."""


class TerminalUnavailable(RuntimeError):
    """A protected interactive password prompt was unavailable."""


class OperatorParser(argparse.ArgumentParser):
    def error(self, message):
        raise InvalidInput


class ApprovedBootstrap:
    """Load the approved public leaf only if the coordinator selects bootstrap."""

    def __init__(self, binding, leaf_file):
        self.binding = binding
        self.leaf_file = leaf_file

    def __call__(self, origin):
        leaf = _read_public_file(self.leaf_file, 8192).decode("ascii")
        return BootstrapHTTPS(origin, self.binding, leaf)(origin)


def _object(pairs):
    obj = dict(pairs)
    if len(obj) != len(pairs):
        raise InvalidInput
    return obj


def _absolute(value):
    if not isinstance(value, str) or "\0" in value or value.startswith("//"):
        raise InvalidInput
    path = Path(value)
    if not path.is_absolute() or ".." in path.parts or str(path) != value:
        raise InvalidInput
    return path


def _outside_git(path):
    if any(os.path.lexists(directory / ".git") for directory in path.resolve().parents):
        raise InvalidInput
    return path


def _read_public_file(path, limit):
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
    with open(path, "rb", opener=lambda name, _: os.open(name, flags)) as handle:
        if not stat.S_ISREG(os.fstat(handle.fileno()).st_mode):
            raise InvalidInput
        data = handle.read(limit + 1)
    if len(data) > limit:
        raise InvalidInput
    return data


def _load_config(path):
    path = _outside_git(_absolute(path))
    config = json.loads(_read_public_file(path, 8192).decode("utf-8"), object_pairs_hook=_object)
    if not isinstance(config, dict) or config.keys() - {"bootstrap"} != CONFIG_FIELDS:
        raise InvalidInput
    text_fields = CONFIG_FIELDS - {"anchor_configured"}
    if not all(isinstance(config[field], str) for field in text_fields) or type(config["anchor_configured"]) is not bool:
        raise InvalidInput
    bootstrap = None
    bootstrap_file = None
    if "bootstrap" in config:
        binding = config["bootstrap"]
        if (not isinstance(binding, dict) or binding.keys() != BOOTSTRAP_FIELDS
                or not all(isinstance(value, str) for value in binding.values())):
            raise InvalidInput
        bootstrap_file = _absolute(binding["leaf_pem_file"])
        bootstrap = BootstrapIdentity(binding["tls_name"], binding["address"], binding["leaf_sha256"])
    ca_file = _absolute(config["ca_pem_file"])
    lineage = _absolute(config["lineage"])
    archive = _absolute(config["archive"])
    journal = _outside_git(_absolute(config["journal"]))
    protected_paths = {path.resolve(), ca_file.resolve()}
    if bootstrap_file is not None:
        protected_paths.add(bootstrap_file.resolve())
    journal_target = journal.resolve()
    if (journal_target in protected_paths or journal_target.is_relative_to(lineage.resolve())
            or journal_target.is_relative_to(archive.resolve())):
        raise InvalidInput
    device = Device(config["identifier"], config["dns"], config["origin"], config["current_pin"],
                    _read_public_file(ca_file, ANCHOR_LIMIT), config["anchor_configured"], bootstrap)
    return device, (lineage, archive, journal), bootstrap_file


def _password():
    try:
        with open("/dev/tty", "w", encoding="utf-8") as terminal, warnings.catch_warnings():
            if not terminal.isatty():
                raise TerminalUnavailable
            warnings.simplefilter("error", getpass.GetPassWarning)
            # If getpass loses its own tty open, even a terminal stdin must not be used.
            original_stdin = sys.stdin
            sys.stdin = None
            try:
                return getpass.getpass("Operator password: ", stream=terminal)
            finally:
                sys.stdin = original_stdin
    except (OSError, EOFError, KeyboardInterrupt, getpass.GetPassWarning):
        raise TerminalUnavailable from None


def _clock():
    return datetime.now(timezone.utc)


def _arguments(argv):
    parser = OperatorParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--config", required=True, help="absolute path to the approved device JSON file")
    modes = parser.add_subparsers(dest="mode", required=True)
    modes.add_parser("stage", help="validate and stage the configured lineage offline")
    modes.add_parser("deploy-hook", help="stage only the exact configured RENEWED_LINEAGE")
    delivery = modes.add_parser("deliver", allow_abbrev=False, help="perform one approved delivery step")
    delivery.add_argument("--approve", action="store_true", help="approve contacting the configured device")
    choice = delivery.add_mutually_exclusive_group()
    choice.add_argument("--approve-first-anchor", action="store_true", help="separately approve first anchor setup")
    choice.add_argument("--observe-bootstrap", action="store_true", help="observe an issued first transition")
    return parser.parse_args(argv)


def _deliver(args, device, paths, bootstrap_file):
    if args.observe_bootstrap and device.bootstrap is None:
        raise InvalidInput
    transport = DeviceHTTPS(device.origin, device.anchor_pem.decode("ascii"))
    bootstrap_transport = None
    if bootstrap_file is not None:
        bootstrap_transport = ApprovedBootstrap(device.bootstrap, bootstrap_file)
    record = deliver(device, *paths, clock=_clock, transport=transport, operator_password=_password,
                     approved=args.approve, first_anchor=args.approve_first_anchor,
                     bootstrap_transport=bootstrap_transport, observe_bootstrap=args.observe_bootstrap)
    state = record["state"]
    if args.approve_first_anchor and state == "ready" and record["anchor_ready"]:
        return "anchor-ready", 0
    return state, DELIVERY_EXITS[state]


def main(argv=None):
    """Run the module command and print only fixed operator-facing tokens."""
    try:
        args = _arguments(argv)
        if args.mode == "deliver" and not args.approve:
            return _report("approval-required", 2)
        device, paths, bootstrap_file = _load_config(args.config)
        if args.mode == "deliver":
            result, code = _deliver(args, device, paths, bootstrap_file)
            return _report(result, code)
        if args.mode == "deploy-hook" and os.environ.get("RENEWED_LINEAGE") != str(paths[0]):
            return _report("lineage-not-approved", 2)
        stage(device, *paths, now=_clock())
        return _report("staged", 0)
    except TerminalUnavailable:
        return _report("terminal-required", 2)
    except (DeliveryError, OSError):
        return _report("operation-failed", 1)
    except (ValueError, TypeError, RuntimeError):
        return _report("invalid-input", 2)
    except KeyboardInterrupt:
        return _report("interrupted", 130)


def _report(result, code):
    print(result)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
