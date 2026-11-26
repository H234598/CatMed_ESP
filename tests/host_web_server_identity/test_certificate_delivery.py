"""Exercise offline delivery through scripted HTTPS responses and synthetic PKI."""

import fcntl
import json
import os
import tempfile
import unittest
from contextlib import contextmanager
from dataclasses import replace
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import patch

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

from tools import katmed_certificate_delivery as delivery
from tools.katmed_certificate_transport import BootstrapIdentity


NOW = datetime(2030, 1, 2, tzinfo=timezone.utc)
DNS = "device.example"
ORIGIN = f"https://{DNS}"
OLD = "a" * 64
SESSION = "b" * 64
CSRF = "c" * 64
LOGIN = "/api/login"
IDENTIFIER = "/api/acme-identifier"
IDENTITY = "/api/server-identity"
ANCHOR = "/api/server-anchor"
LOGIN_RESPONSE = (200, {"Set-Cookie": f"katmed_session={SESSION}; Path=/; Secure; HttpOnly; SameSite=Strict"},
                  {"csrf_token": CSRF})


def issue(name, key, issuer=None, issuer_key=None, *, ca=False, server_auth=True):
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    signing_key = issuer_key or key
    builder = (x509.CertificateBuilder().subject_name(subject)
               .issuer_name(issuer.subject if issuer else subject)
               .public_key(key.public_key()).serial_number(x509.random_serial_number())
               .not_valid_before(NOW - timedelta(days=2))
               .not_valid_after(NOW + timedelta(days=2))
               .add_extension(x509.BasicConstraints(ca=ca, path_length=1 if ca else None), True)
               .add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False,
                   key_encipherment=False, data_encipherment=False, key_agreement=False,
                   key_cert_sign=ca, crl_sign=ca, encipher_only=False, decipher_only=False), True)
               .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), False)
               .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(signing_key.public_key()), False))
    if not ca:
        purpose = ExtendedKeyUsageOID.SERVER_AUTH if server_auth else ExtendedKeyUsageOID.CLIENT_AUTH
        builder = (builder.add_extension(x509.SubjectAlternativeName([x509.DNSName(name)]), False)
                   .add_extension(x509.ExtendedKeyUsage([purpose]), False))
    return builder.sign(signing_key, hashes.SHA256())


class FakeConnection:
    def __init__(self, transport, fingerprint, state, *, origin=ORIGIN, verified=True):
        self.transport = transport
        self.origin = origin
        self.verified = verified
        self.leaf_sha256 = fingerprint
        self.state = state

    def request(self, method, route, headers, body):
        payload = json.loads(body) if body else None
        self.transport.calls.append((method, route, headers.copy(), payload))
        if route == LOGIN:
            return self.transport.login
        if method == "GET":
            return 200, {}, {"identifier": self.transport.identifier, "state": self.state}
        if self.transport.loss:
            raise ConnectionError("synthetic response loss")
        if route == ANCHOR:
            return 200, {}, {"anchor_configured": True}
        return 202, {}, {"state": "pending"}


class FakeTransport:
    def __init__(self):
        self.connections = []
        self.calls = []
        self.login = LOGIN_RESPONSE
        self.identifier = DNS
        self.loss = False

    @contextmanager
    def __call__(self, origin):
        if origin != ORIGIN:
            raise AssertionError("incorrect transport target")
        fingerprint, state, options = self.connections.pop(0)
        yield FakeConnection(self, fingerprint, state, **options)

    def add(self, fingerprint, state, **options):
        self.connections.append((fingerprint, state, options))

    def writes(self, path):
        return [call for call in self.calls if call[:2] == ("POST", path)]


class CertificateDeliveryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root_key = ec.generate_private_key(ec.SECP256R1())
        cls.root = issue("synthetic root", cls.root_key, ca=True)
        cls.inter_key = ec.generate_private_key(ec.SECP256R1())
        cls.inter = issue("synthetic intermediate", cls.inter_key, cls.root, cls.root_key, ca=True)

    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        base = Path(scratch.name)
        self.live = base / "live"
        self.archive = base / "archive"
        self.journal = base / "delivery.json"
        self.live.mkdir()
        self.archive.mkdir()
        self.device = delivery.Device("device-001", DNS, ORIGIN, OLD,
                                      self.root.public_bytes(serialization.Encoding.PEM), True)
        self.now = NOW
        self.transport = FakeTransport()
        self.password_reads = 0
        self.lineage()

    def lineage(self, generation=1, *, dns=DNS, curve=None, server_auth=True, wrong_key=False):
        key = ec.generate_private_key(curve or ec.SECP256R1())
        leaf = issue(dns, key, self.inter, self.inter_key, server_auth=server_auth)
        self.fingerprint = leaf.fingerprint(hashes.SHA256()).hex()
        leaf_data = leaf.public_bytes(serialization.Encoding.PEM)
        chain = leaf_data + self.inter.public_bytes(serialization.Encoding.PEM)
        key = ec.generate_private_key(ec.SECP256R1()) if wrong_key else key
        self.key_hex = key.private_numbers().private_value.to_bytes((key.key_size + 7) // 8, "big").hex()
        key_data = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                     serialization.NoEncryption())
        for name, data in (("cert", leaf_data), ("fullchain", chain), ("privkey", key_data)):
            path = self.archive / f"{name}{generation}.pem"
            path.write_bytes(data)
            link = self.live / f"{name}.pem"
            link.unlink(missing_ok=True)
            link.symlink_to(Path("../archive") / path.name)

    def stage(self):
        return delivery.stage(self.device, self.live, self.archive, self.journal, self.now)

    def password(self):
        self.password_reads += 1
        return "synthetic operator proof"

    def deliver(self, **options):
        return delivery.deliver(self.device, self.live, self.archive, self.journal,
                                clock=lambda: self.now, transport=self.transport,
                                operator_password=self.password, **{"approved": True, **options})

    def assert_redacted(self):
        data = self.journal.read_text()
        for value in ("synthetic operator proof", self.key_hex, "CERTIFICATE", SESSION, CSRF):
            self.assertNotIn(value, data)

    def test_stage_and_confirm_exact_leaf_without_duplicate_post(self):
        record = self.stage()
        self.assertEqual(record["generation"], 1)
        self.assertEqual(record["fingerprint"], self.fingerprint)
        self.assertEqual(record["expires"], (NOW + timedelta(days=2)).isoformat())
        self.assertEqual(self.transport.calls, [])
        self.assertEqual(self.password_reads, 0)
        self.assert_redacted()
        self.transport.add(OLD, "active")
        self.transport.add(self.fingerprint, "active")
        self.assertEqual(self.deliver()["state"], "active")
        self.transport.add(self.fingerprint, "active")
        self.assertEqual(self.deliver()["state"], "active")
        [call] = self.transport.writes(IDENTITY)
        self.assertEqual(call[2], {"Origin": ORIGIN, "Cookie": f"katmed_session={SESSION}",
                                  "X-Katmed-CSRF": CSRF, "Content-Type": "application/json"})
        self.assertEqual(set(call[3]), {"key_hex", "chain_pem", "password"})
        self.assertEqual(call[3]["key_hex"], self.key_hex)
        self.assertEqual(call[3]["password"], "synthetic operator proof")
        self.assertTrue(call[3]["chain_pem"].startswith("-----BEGIN CERTIFICATE-----"))
        self.assert_redacted()

    def test_staged_bootstrap_binding_cannot_be_swapped_or_removed(self):
        bootstrap = BootstrapIdentity("katmed-abcdef.local", "127.0.0.1", OLD)
        self.device = replace(self.device, bootstrap=bootstrap)
        self.stage()
        with self.assertRaisesRegex(delivery.DeliveryError, "^bootstrap_observation_unavailable$"):
            self.deliver(observe_bootstrap=True)
        with self.assertRaisesRegex(delivery.DeliveryError, "^bootstrap_transport_mismatch$"):
            self.deliver(bootstrap_transport=self.transport)
        for changed in (None, replace(bootstrap, address="127.0.0.2"),
                        replace(bootstrap, tls_name="katmed-012345.local")):
            with self.subTest(binding=changed):
                self.device = replace(self.device, bootstrap=changed)
                with self.assertRaisesRegex(delivery.DeliveryError, "^journal_bootstrap_mismatch$"):
                    self.deliver()
                with self.assertRaisesRegex(delivery.DeliveryError, "^journal_bootstrap_mismatch$"):
                    self.stage()
        self.assertEqual(self.password_reads, 0)
        self.assertEqual(self.transport.calls, [])

    def test_verified_first_identity_can_progress_to_normal_renewal(self):
        bootstrap = BootstrapIdentity("katmed-abcdef.local", "127.0.0.1", OLD)
        self.device = replace(self.device, bootstrap=bootstrap)
        self.stage()
        boot = FakeTransport()
        boot.binding = bootstrap
        boot.add(OLD, "legacy")
        first_pin = self.fingerprint
        self.transport.add(first_pin, "active")
        self.assertEqual(self.deliver(bootstrap_transport=boot)["state"], "active")
        self.assertEqual(len(boot.writes(IDENTITY)), 1)
        with self.assertRaisesRegex(delivery.DeliveryError, "^bootstrap_observation_unavailable$"):
            self.deliver(bootstrap_transport=boot, observe_bootstrap=True)
        self.lineage(2)
        with self.assertRaisesRegex(delivery.DeliveryError, "^bootstrap_only_for_first_identity$"):
            self.stage()
        self.device = replace(self.device, bootstrap=None, current_pin=first_pin)
        self.assertNotIn("bootstrap", self.stage())
        self.transport.add(first_pin, "active")
        self.transport.add(self.fingerprint, "active")
        self.assertEqual(self.deliver()["state"], "active")
        self.assertEqual(len(self.transport.writes(IDENTITY)), 1)
        self.assertEqual(len(boot.writes(IDENTITY)), 1)

    def test_interrupted_post_reconciles_after_old_leaf_then_pending(self):
        self.stage()
        self.transport.loss = True
        self.transport.add(OLD, "active")
        self.transport.add(OLD, "active")
        self.assertEqual(self.deliver()["state"], "uncertain")
        self.transport.add(self.fingerprint, "pending")
        self.assertEqual(self.deliver()["state"], "pending")
        self.transport.add(self.fingerprint, "active")
        self.assertEqual(self.deliver()["state"], "active")
        self.assertEqual(len(self.transport.writes(IDENTITY)), 1)
        self.assert_redacted()

    def test_active_record_needs_new_exact_readback(self):
        self.stage()
        self.transport.add(self.fingerprint, "active")
        self.assertEqual(self.deliver()["state"], "active")
        self.transport.add(OLD, "active")
        self.assertEqual(self.deliver()["state"], "uncertain")
        self.assertFalse(self.transport.writes(IDENTITY))

    def test_inflight_generation_can_reconcile_after_live_links_rotate(self):
        self.stage()
        first = self.fingerprint
        self.transport.add(OLD, "legacy")
        self.transport.add(first, "pending")
        self.assertEqual(self.deliver()["state"], "pending")
        self.lineage(2)
        with self.assertRaisesRegex(delivery.DeliveryError, "previous_generation_open"):
            self.stage()
        self.transport.add(first, "active")
        self.assertEqual(self.deliver()["state"], "active")
        self.assertEqual(self.stage()["generation"], 2)
        self.assertEqual(len(self.transport.writes(IDENTITY)), 1)

    def test_process_interruption_after_record_before_post_response(self):
        self.stage()
        self.transport.add(OLD, "legacy")
        original = FakeConnection.request

        def interrupt(connection, method, route, headers, data):
            if (method, route) == ("POST", IDENTITY):
                persisted = json.loads(self.journal.read_text())
                self.assertEqual((persisted["phase"], persisted["state"]), ("issued", "uncertain"))
                raise KeyboardInterrupt
            return original(connection, method, route, headers, data)

        with patch.object(FakeConnection, "request", interrupt), self.assertRaises(KeyboardInterrupt):
            self.deliver()
        self.transport.add(OLD, "legacy")
        self.assertEqual(self.deliver()["state"], "uncertain")
        self.assertFalse(self.transport.writes(IDENTITY))

    def test_failed_and_expired_pending_remain_read_only(self):
        self.stage()
        self.transport.add(OLD, "active")
        self.transport.add(self.fingerprint, "pending")
        self.assertEqual(self.deliver()["state"], "pending")
        self.now += timedelta(seconds=16)
        self.transport.add(self.fingerprint, "pending")
        self.assertEqual(self.deliver()["state"], "uncertain")
        self.transport.add(OLD, "failed")
        self.assertEqual(self.deliver()["state"], "failed")
        self.transport.add(OLD, "uncertain")
        self.assertEqual(self.deliver()["state"], "uncertain")
        self.assertEqual(len(self.transport.writes(IDENTITY)), 1)

    def test_no_credentials_before_approval_and_tls_binding(self):
        self.stage()
        with self.assertRaisesRegex(delivery.DeliveryError, "operator_approval_required"):
            self.deliver(approved=False)
        for fingerprint, options in (("f" * 64, {}), (OLD, {"verified": False}), (OLD, {"verified": "yes"}),
                                     (OLD, {"origin": "https://wrong.example"})):
            with self.subTest(fingerprint=fingerprint, options=options):
                self.transport.add(fingerprint, "legacy", **options)
                with self.assertRaisesRegex(delivery.DeliveryError, "untrusted_peer"):
                    self.deliver()
        self.assertEqual(self.transport.calls, [])
        self.assertEqual(self.password_reads, 0)

    def test_submission_holds_lock_until_reconciliation(self):
        self.stage()
        self.transport.add(OLD, "legacy")
        self.transport.add(self.fingerprint, "active")
        original = FakeConnection.request

        def check_lock(connection, method, route, headers, data):
            if (method, route) == ("POST", IDENTITY) or connection.leaf_sha256 == self.fingerprint:
                fd = os.open(self.journal.parent, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                finally:
                    os.close(fd)
            return original(connection, method, route, headers, data)

        with patch.object(FakeConnection, "request", check_lock):
            self.assertEqual(self.deliver()["state"], "active")
        self.assertEqual(len(self.transport.writes(IDENTITY)), 1)

    def test_wrong_identifier_or_session_does_not_write(self):
        self.stage()
        self.transport.identifier = "wrong.example"
        self.transport.add(OLD, "legacy")
        with self.assertRaisesRegex(delivery.DeliveryError, "identifier_mismatch"):
            self.deliver()
        self.transport.login = (200, {"Set-Cookie": f"katmed_session={SESSION}"}, {"csrf_token": CSRF})
        self.transport.add(OLD, "legacy")
        with self.assertRaisesRegex(delivery.DeliveryError, "invalid_session"):
            self.deliver()
        self.assertFalse(self.transport.writes(IDENTITY))
        self.assertFalse(self.transport.writes(ANCHOR))

    def test_first_anchor_requires_own_operator_step(self):
        with self.assertRaisesRegex(delivery.DeliveryError, "invalid_device"):
            replace(self.device, anchor_configured="false")
        self.device = replace(self.device, anchor_configured=False)
        self.stage()
        with self.assertRaisesRegex(delivery.DeliveryError, "invalid_anchor_approval"):
            self.deliver(first_anchor="false")
        with self.assertRaisesRegex(delivery.DeliveryError, "anchor_approval_required"):
            self.deliver()
        self.assertEqual(self.transport.calls, [])
        self.transport.add(OLD, "legacy")
        self.assertTrue(self.deliver(first_anchor=True)["anchor_ready"])
        self.assertEqual(len(self.transport.writes(ANCHOR)), 1)
        self.assertFalse(self.transport.writes(IDENTITY))
        self.transport.add(OLD, "legacy")
        self.transport.add(self.fingerprint, "active")
        self.assertEqual(self.deliver()["state"], "active")
        self.assert_redacted()

    def test_uncertain_anchor_never_repeats(self):
        self.device = replace(self.device, anchor_configured=False)
        self.stage()
        self.transport.loss = True
        self.transport.add(OLD, "legacy")
        self.assertEqual(self.deliver(first_anchor=True)["phase"], "anchor_uncertain")
        self.assertEqual(self.deliver(first_anchor=True)["phase"], "anchor_uncertain")
        self.assertEqual(self.deliver()["phase"], "anchor_uncertain")
        self.assertEqual(len(self.transport.writes(ANCHOR)), 1)
        self.assertFalse(self.transport.writes(IDENTITY))

    def test_rotation_after_active_only(self):
        original = self.stage()
        self.transport.add(self.fingerprint, "active")
        self.assertEqual(self.deliver()["state"], "active")
        self.lineage(2)
        new = self.stage()
        self.assertEqual(new["generation"], 2)
        self.assertNotEqual(original["fingerprint"], new["fingerprint"])
        self.assertTrue(new["anchor_ready"])
        self.lineage(3)
        with self.assertRaisesRegex(delivery.DeliveryError, "previous_generation_open"):
            self.stage()
        self.assertEqual(json.loads(self.journal.read_text()), new)

    def test_changing_links_and_path_escape_fail(self):
        with patch.object(delivery, "_generation", side_effect=(1, 2)), self.assertRaisesRegex(
            delivery.DeliveryError, "lineage_changed"
        ):
            self.stage()
        link = self.live / "cert.pem"
        link.unlink()
        link.symlink_to(Path("../cert1.pem"))
        with self.assertRaisesRegex(delivery.DeliveryError, "lineage_link"):
            self.stage()
        self.assertFalse(self.journal.exists())

    def test_material_rejects_wrong_curve_pair_san_eku_dates_and_trust(self):
        for options in ({"curve": ec.SECP384R1()}, {"wrong_key": True}, {"dns": "wrong.example"},
                        {"dns": "*.example"}, {"server_auth": False}):
            with self.subTest(options=options):
                self.lineage(**options)
                with self.assertRaisesRegex(delivery.DeliveryError, "invalid_material"):
                    self.stage()
        self.lineage()
        self.now += timedelta(days=3)
        with self.assertRaisesRegex(delivery.DeliveryError, "invalid_material"):
            self.stage()
        self.now = NOW
        wrong_key = ec.generate_private_key(ec.SECP256R1())
        wrong_root = issue("unrelated root", wrong_key, ca=True)
        self.device = replace(self.device, anchor_pem=wrong_root.public_bytes(serialization.Encoding.PEM))
        with self.assertRaisesRegex(delivery.DeliveryError, "invalid_material"):
            self.stage()

    def test_chain_cannot_carry_extra_text_and_sizes_are_bounded(self):
        chain = self.archive / "fullchain1.pem"
        original = chain.read_bytes()
        chain.write_bytes(original + b"extra synthetic text")
        with self.assertRaisesRegex(delivery.DeliveryError, "invalid_material"):
            self.stage()
        chain.write_bytes(b"x" * (delivery.CHAIN_LIMIT + 1))
        with self.assertRaisesRegex(delivery.DeliveryError, "file_size"):
            self.stage()
        with self.assertRaisesRegex(delivery.DeliveryError, "body_size"):
            delivery._body({"chain_pem": "\n" * 6000})
        self.assertFalse(self.journal.exists())


if __name__ == "__main__":
    unittest.main()
