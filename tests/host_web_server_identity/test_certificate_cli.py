"""Exercise the operator entry point with synthetic material and an injected transport."""

import getpass
import io
import json
import os
import secrets
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import Mock, mock_open, patch

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

from test_certificate_delivery import ANCHOR, DNS, IDENTITY, NOW, OLD, ORIGIN, FakeTransport, issue
from tools import katmed_certificate_cli as cli


PEM = serialization.Encoding.PEM
FAKE_PASSWORD = secrets.token_hex(16)


class CertificateCLITest(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.live = self.root / "live"
        self.archive = self.root / "archive"
        self.live.mkdir()
        self.archive.mkdir()
        self.journal = self.root / "state.json"
        self.config_file = self.root / "device.json"
        self.ca_file = self.root / "ca.pem"
        root_key = ec.generate_private_key(ec.SECP256R1())
        root_ca = issue("synthetic CLI root", root_key, ca=True)
        self.ca_file.write_bytes(root_ca.public_bytes(PEM))
        key = ec.generate_private_key(ec.SECP256R1())
        leaf = issue(DNS, key, root_ca, root_key)
        self.pin = leaf.fingerprint(hashes.SHA256()).hex()
        cert = leaf.public_bytes(PEM)
        private = key.private_bytes(PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
        for name, data in (("cert", cert), ("fullchain", cert), ("privkey", private)):
            filename = f"{name}1.pem"
            (self.archive / filename).write_bytes(data)
            (self.live / f"{name}.pem").symlink_to(Path("../archive") / filename)
        self.config = {
            "identifier": "device-001", "dns": DNS, "origin": ORIGIN, "current_pin": OLD,
            "anchor_configured": True, "ca_pem_file": str(self.ca_file),
            "lineage": str(self.live), "archive": str(self.archive), "journal": str(self.journal),
        }
        self.save_config()

    def save_config(self):
        self.config_file.write_text(json.dumps(self.config), encoding="utf-8")

    def run_cli(self, mode, *options):
        out = io.StringIO()
        err = io.StringIO()
        with redirect_stdout(out), redirect_stderr(err), patch.object(cli, "_clock", return_value=NOW):
            status = cli.main(["--config", str(self.config_file), mode, *options])
        self.assertEqual(err.getvalue(), "")
        self.assertNotIn(FAKE_PASSWORD, out.getvalue())
        return status, out.getvalue().strip()

    def configure_bootstrap(self):
        bootstrap_pem = self.root / "bootstrap.pem"
        bootstrap_pem.write_text("synthetic public leaf", encoding="ascii")
        self.config["bootstrap"] = {
            "tls_name": "katmed-abcdef.local", "address": "127.0.0.1",
            "leaf_sha256": OLD, "leaf_pem_file": str(bootstrap_pem),
        }
        self.save_config()
        return bootstrap_pem

    def test_offline_stage_and_only_exact_explicit_hook(self):
        with (patch.object(cli, "DeviceHTTPS") as tls, patch.object(cli.getpass, "getpass") as password,
              patch.dict(os.environ, {"RENEWED_LINEAGE": f"{self.live}/"})):
            self.assertEqual(self.run_cli("deploy-hook"), (2, "lineage-not-approved"))
            self.assertFalse(self.journal.exists())
            self.assertEqual(self.run_cli("stage"), (0, "staged"))
            before = self.journal.read_bytes()
            self.assertNotIn(b"PRIVATE KEY", before)
            self.assertEqual(json.loads(before)["fingerprint"], self.pin)
            with patch.dict(os.environ, {"RENEWED_LINEAGE": str(self.live)}):
                self.assertEqual(self.run_cli("deploy-hook"), (0, "staged"))
            with patch.dict(os.environ, {}, clear=True):
                self.assertEqual(self.run_cli("deploy-hook"), (2, "lineage-not-approved"))
            self.assertEqual(self.journal.read_bytes(), before)
            tls.assert_not_called()
            password.assert_not_called()

    def test_config_is_strict_bounded_and_outside_git(self):
        original = self.config_file.read_text(encoding="utf-8")
        for invalid in ('{"anchor_configured":true,' + original[1:], " " * 8193,
                        json.dumps({**self.config, "password": FAKE_PASSWORD}),
                        json.dumps({**self.config, "archive": "relative"}),
                        json.dumps({**self.config, "origin": {"redact": FAKE_PASSWORD}}),
                        json.dumps({**self.config, "journal": str(self.archive / "cert1.pem")})):
            with self.subTest():
                self.config_file.write_text(invalid, encoding="utf-8")
                self.assertEqual(self.run_cli("stage"), (2, "invalid-input"))
        self.save_config()
        (self.root / ".git").write_text("synthetic repository marker", encoding="utf-8")
        self.assertEqual(self.run_cli("stage"), (2, "invalid-input"))
        self.assertFalse(self.journal.exists())

    def test_approval_is_independent_and_argument_errors_are_redacted(self):
        with patch.object(cli, "DeviceHTTPS") as tls:
            self.assertEqual(self.run_cli("deliver", "--approve-first-anchor"), (2, "approval-required"))
            self.assertEqual(self.run_cli("deliver", "--password", FAKE_PASSWORD), (2, "invalid-input"))
            self.assertEqual(self.run_cli("deliver", "--approve", "--approve-first-anchor", "--observe-bootstrap"),
                             (2, "invalid-input"))
            self.assertEqual(self.run_cli("deliver", "--approve", "--observe-bootstrap"), (2, "invalid-input"))
            tls.assert_not_called()

    def test_ordinary_delivery_uses_verified_transport_and_existing_stage(self):
        self.assertEqual(self.run_cli("stage"), (0, "staged"))
        transport = FakeTransport()
        transport.add("f" * 64, "active")
        transport.add(OLD, "active")
        transport.add(self.pin, "active")
        with (patch.object(cli, "DeviceHTTPS", return_value=transport) as tls,
              patch.object(cli, "_password", return_value=FAKE_PASSWORD) as password):
            self.assertEqual(self.run_cli("deliver", "--approve"), (1, "operation-failed"))
            password.assert_not_called()
            self.assertEqual(transport.calls, [])
            self.assertEqual(self.run_cli("deliver", "--approve"), (0, "active"))
            password.assert_called_once_with()
            tls.assert_called_with(ORIGIN, self.ca_file.read_text(encoding="ascii"))
        self.assertEqual(len(transport.writes(IDENTITY)), 1)
        self.assertNotIn(FAKE_PASSWORD, self.journal.read_text(encoding="utf-8"))

    def test_first_anchor_is_one_separate_approved_bootstrap_action(self):
        self.config["anchor_configured"] = False
        self.configure_bootstrap()
        self.assertEqual(self.run_cli("stage"), (0, "staged"))
        issued = FakeTransport()
        bootstrap = FakeTransport()
        bootstrap.add(OLD, "legacy")
        bootstrap.add(OLD, "legacy")
        issued.add(self.pin, "active")
        with (patch.object(cli, "DeviceHTTPS", return_value=issued),
              patch.object(cli, "BootstrapHTTPS", return_value=bootstrap) as tls,
              patch.object(cli, "_password", return_value=FAKE_PASSWORD) as password):
            self.assertEqual(self.run_cli("deliver", "--approve"), (1, "operation-failed"))
            tls.assert_not_called()
            password.assert_not_called()
            self.assertEqual(self.run_cli("deliver", "--approve", "--approve-first-anchor"), (0, "anchor-ready"))
            self.assertEqual(len(bootstrap.writes(ANCHOR)), 1)
            self.assertEqual(bootstrap.writes(IDENTITY), [])
            self.assertEqual(self.run_cli("deliver", "--approve"), (0, "active"))
            origin, binding, leaf = tls.call_args.args
            self.assertEqual((origin, binding.tls_name, binding.address, binding.leaf_sha256, leaf),
                             (ORIGIN, "katmed-abcdef.local", "127.0.0.1", OLD, "synthetic public leaf"))
        self.assertEqual(len(bootstrap.writes(IDENTITY)), 1)
        self.assertEqual(issued.writes(IDENTITY), [])

    def test_explicit_bootstrap_observation_is_read_only_and_ordinary_observation_needs_no_leaf(self):
        bootstrap_pem = self.configure_bootstrap()
        self.assertEqual(self.run_cli("stage"), (0, "staged"))
        issued = FakeTransport()
        bootstrap = FakeTransport()
        bootstrap.add(OLD, "legacy")
        bootstrap.add(OLD, "pending")
        issued.add(OLD, "pending")
        issued.add(self.pin, "active")
        with (patch.object(cli, "DeviceHTTPS", return_value=issued),
              patch.object(cli, "BootstrapHTTPS", return_value=bootstrap) as tls,
              patch.object(cli, "_password", return_value=FAKE_PASSWORD)):
            self.assertEqual(self.run_cli("deliver", "--approve", "--observe-bootstrap"),
                             (1, "operation-failed"))
            tls.assert_not_called()
            self.assertEqual(self.run_cli("deliver", "--approve"), (3, "pending"))
            self.assertEqual(self.run_cli("deliver", "--approve", "--observe-bootstrap"), (3, "pending"))
            self.assertEqual(tls.call_count, 2)
            bootstrap_pem.unlink()
            self.assertEqual(self.run_cli("deliver", "--approve"), (0, "active"))
        self.assertEqual(len(bootstrap.writes(IDENTITY)), 1)
        self.assertEqual(issued.writes(IDENTITY), [])

    def test_no_terminal_provides_no_credential_or_api_request(self):
        self.assertEqual(self.run_cli("stage"), (0, "staged"))
        transport = FakeTransport()
        transport.add(OLD, "active")

        def open_without_terminal(path, *args, **kwargs):
            if path == "/dev/tty":
                raise OSError(FAKE_PASSWORD)
            return open(path, *args, **kwargs)

        with (patch.object(cli, "DeviceHTTPS", return_value=transport),
              patch.object(cli, "open", open_without_terminal, create=True),
              patch.object(cli.getpass, "getpass") as password):
            self.assertEqual(self.run_cli("deliver", "--approve"), (2, "terminal-required"))
            password.assert_not_called()
        self.assertEqual(transport.calls, [])

    def test_uncertain_and_failed_delivery_have_distinct_exits_without_duplicate_post(self):
        self.assertEqual(self.run_cli("stage"), (0, "staged"))
        transport = FakeTransport()
        transport.loss = True
        transport.add(OLD, "active")
        transport.add(OLD, "active")
        transport.add(OLD, "failed")
        with (patch.object(cli, "DeviceHTTPS", return_value=transport),
              patch.object(cli, "_password", return_value=FAKE_PASSWORD)):
            self.assertEqual(self.run_cli("deliver", "--approve"), (4, "uncertain"))
            self.assertEqual(self.run_cli("deliver", "--approve"), (5, "failed"))
        self.assertEqual(len(transport.writes(IDENTITY)), 1)

    def test_getpass_echo_warning_aborts_and_original_stdin_is_restored(self):
        terminal = Mock(spec=io.TextIOBase)
        terminal.isatty.return_value = True
        opener = mock_open()
        opener.return_value.__enter__.return_value = terminal
        original_stdin = Mock(spec=io.TextIOBase)
        with (patch.object(cli, "open", opener, create=True), patch.object(sys, "stdin", original_stdin),
              patch.object(getpass.os, "open", side_effect=OSError) as getpass_open):
            with self.assertRaises(cli.TerminalUnavailable):
                cli._password()
            self.assertIs(sys.stdin, original_stdin)
        getpass_open.assert_called_once_with("/dev/tty", os.O_RDWR | os.O_NOCTTY)
        opener.assert_called_once_with("/dev/tty", "w", encoding="utf-8")
        terminal.write.assert_not_called()
        original_stdin.fileno.assert_not_called()
        original_stdin.readline.assert_not_called()
