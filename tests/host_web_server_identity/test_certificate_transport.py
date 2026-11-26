"""Exercise certificate delivery over a real TLS connection to a synthetic local server."""

import hashlib
import json
import socket
import ssl
import sys
import tempfile
import threading
import unittest
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from socketserver import TCPServer
from unittest.mock import patch

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

from tools.katmed_certificate_delivery import DeliveryError, Device, deliver, stage
from tools.katmed_certificate_transport import (
    BODY_LIMIT, BootstrapHTTPS, BootstrapIdentity, DeviceHTTPS, LoopbackHTTPS, TransportError,
)


HOST = "katmed.example"
ORIGIN = f"https://{HOST}"
BOOTSTRAP_NAME = "katmed-abcdef.local"
LOOPBACK = "127.0.0.1"
OPERATOR_INPUT = "synthetic operator confirmation"
SESSION = "a" * 64
CSRF = "b" * 64
GET_STATUS = "/api/server-identity"
POST_HEADERS = {"Origin": ORIGIN, "Content-Type": "application/json"}
SYSTEM_GETADDRINFO = socket.getaddrinfo


def issue(name, key, now, *, parent=None, signing_key=None):
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    authority = parent.subject if parent else subject
    issuer_key = signing_key or key
    builder = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(authority)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(days=1))
        .not_valid_after(now + timedelta(days=3))
        .add_extension(x509.BasicConstraints(ca=parent is None, path_length=None), critical=True)
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
        .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(issuer_key.public_key()),
                       critical=False)
        .add_extension(x509.KeyUsage(True, False, False, False, False, parent is None,
                                    parent is None, None, None), critical=True)
    )
    if parent:
        builder = builder.add_extension(x509.SubjectAlternativeName([x509.DNSName(name)]),
                                        critical=False)
        builder = builder.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]),
                                        critical=False)
    return builder.sign(issuer_key, hashes.SHA256())


def bootstrap_leaf(name, key, now):
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    return (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(days=1))
        .not_valid_after(now + timedelta(days=3))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(x509.KeyUsage(True, False, False, False, False, False,
                                    False, None, None), critical=True)
        .add_extension(x509.SubjectAlternativeName([x509.DNSName(name)]), critical=False)
        .sign(key, hashes.SHA256())
    )


def pem(cert):
    return cert.public_bytes(serialization.Encoding.PEM)


def key_pem(key):
    return key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                             serialization.NoEncryption())


def leaf_digest(cert):
    return hashlib.sha256(cert.public_bytes(serialization.Encoding.DER)).hexdigest()


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        # The assertions use structured request records instead of server logs.
        pass

    def _run(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        request = (self.command, self.path, self.headers, body, self.connection.fixture_id)
        self.server.requests.append(request)
        status, headers, response = self.server.respond(request)
        self.send_response(status)
        if "Content-Length" not in headers:
            self.send_header("Content-Length", str(len(response)))
        self.send_header("Content-Type", "application/json")
        for name, value in headers.items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(response)
        if headers.get("Connection") == "close":
            self.close_connection = True

    do_GET = _run
    do_POST = _run


class _LocalTLSServer(ThreadingHTTPServer):
    def __init__(self, context):
        self.context = context
        self.requests = []
        self.handshakes = 0
        self.respond = lambda _request: (200, {}, b"{}")
        super().__init__((LOOPBACK, 0), _Handler)

    def server_bind(self):
        # Use the numeric bound address directly, without HTTPServer's reverse lookup.
        TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address

    def get_request(self):
        raw, address = super().get_request()
        try:
            raw.settimeout(3)
            client = self.context.wrap_socket(raw, server_side=True)
        except BaseException:
            raw.close()
            raise
        self.handshakes += 1
        client.fixture_id = self.handshakes
        return client, address

    def handle_error(self, request, client_address):
        if not isinstance(sys.exception(), (BrokenPipeError, ConnectionResetError, ssl.SSLEOFError)):
            super().handle_error(request, client_address)


class _BootstrapFixture(BootstrapHTTPS):
    """Keep the real bootstrap TLS and HTTP checks on an unprivileged fixture port."""

    def __init__(self, origin, binding, leaf_pem, port):
        super().__init__(origin, binding, leaf_pem)
        self.fixture_port = port

    def _new_connection(self):
        connection = super()._new_connection()
        if connection._address != (LOOPBACK, 443):
            raise AssertionError("Unexpected bootstrap dial binding")
        connection._address = (LOOPBACK, self.fixture_port)
        return connection


class CertificateTransportTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.directory = Path(temp.name)
        self.now = datetime.now(timezone.utc).replace(microsecond=0)
        self.ca_key = ec.generate_private_key(ec.SECP256R1())
        self.ca = issue("synthetic root", self.ca_key, self.now)
        self.old_key, self.old = self.make_leaf()
        for resolver in ("socket.getaddrinfo", "socket.gethostbyaddr"):
            self.enterContext(patch(resolver, side_effect=AssertionError("Unexpected DNS lookup")))
        self.server = _LocalTLSServer(self.server_context("old", self.old_key, self.old))
        self.server_port = self.server.server_address[1]
        thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.01})
        thread.start()
        self.addCleanup(self.stop_server, thread)
        self.transport = self.make_transport()

    def stop_server(self, thread):
        self.server.shutdown()
        self.server.server_close()
        thread.join()

    def make_leaf(self, host=HOST):
        key = ec.generate_private_key(ec.SECP256R1())
        return key, issue(host, key, self.now, parent=self.ca, signing_key=self.ca_key)

    def server_context(self, name, key, cert):
        certificate_path = self.directory / f"{name}.pem"
        key_path = self.directory / f"{name}.key"
        certificate_path.write_bytes(pem(cert))
        key_path.write_bytes(key_pem(key))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate_path, key_path)
        return context

    def make_transport(self, origin=ORIGIN, ca=None):
        return LoopbackHTTPS(origin, pem(ca or self.ca).decode("ascii"), LOOPBACK, self.server_port)

    def test_single_tls_authority_cannot_reach_firmware_bootstrap(self):
        key = ec.generate_private_key(ec.SECP256R1())
        leaf = bootstrap_leaf(BOOTSTRAP_NAME, key, self.now)
        self.server.context = self.server_context("bootstrap-seam", key, leaf)
        for origin in (ORIGIN, "https://127.0.0.1"):
            with self.subTest(origin=origin):
                with self.assertRaisesRegex(TransportError, "^tls_peer_verification_failed$"):
                    with self.make_transport(origin=origin, ca=leaf)(origin):
                        self.fail("A mismatched certificate name was accepted")
        self.assertEqual(self.server.requests, [])

        def authority_guard(request):
            allowed = {HOST, LOOPBACK}
            return (200, {}, b"{}") if request[2]["Host"] in allowed else (403, {}, b"{}")

        self.server.respond = authority_guard
        bootstrap_origin = f"https://{BOOTSTRAP_NAME}"
        with self.make_transport(origin=bootstrap_origin, ca=leaf)(bootstrap_origin) as connection:
            self.assertEqual(connection.leaf_sha256, leaf_digest(leaf))
            self.assertEqual(connection.request("GET", GET_STATUS, {}, None)[0], 403)

    def first_identity(self, *, anchor_configured=True):
        key = ec.generate_private_key(ec.SECP256R1())
        leaf = bootstrap_leaf(BOOTSTRAP_NAME, key, self.now)
        self.server.context = self.server_context("bootstrap", key, leaf)
        binding = BootstrapIdentity(BOOTSTRAP_NAME, LOOPBACK, leaf_digest(leaf))
        boot = _BootstrapFixture(ORIGIN, binding, pem(leaf).decode("ascii"), self.server_port)
        key, fresh = self.make_leaf()
        rotated = self.server_context("issued", key, fresh)
        lineage, archive = self.make_lineage(key, fresh)
        journal = self.directory / "first-delivery.json"
        device = Device("device-001", HOST, ORIGIN, leaf_digest(leaf), pem(self.ca),
                        anchor_configured, bootstrap=binding)
        stage(device, lineage, archive, journal, self.now)
        return device, lineage, archive, journal, boot, leaf, fresh, rotated

    def test_first_transition_keeps_http_authority_and_requires_separate_anchor(self):
        device, lineage, archive, journal, boot, _, fresh, rotated = self.first_identity(
            anchor_configured=False
        )
        status = self.deliver_response(rotated, issued_connection=3)

        def respond(request):
            method, path, headers, _, _ = request
            self.assertEqual(headers["Host"], HOST)
            if method == "POST":
                self.assertEqual(headers["Origin"], ORIGIN)
            if path == "/api/server-anchor":
                return 200, {}, b'{"anchor_configured":true}'
            return status(request)

        self.server.respond = respond
        options = dict(clock=lambda: self.now, transport=self.transport, bootstrap_transport=boot,
                       operator_password=lambda: OPERATOR_INPUT, approved=True)
        with self.assertRaisesRegex(DeliveryError, "^anchor_approval_required$"):
            deliver(device, lineage, archive, journal, **options)
        self.assertEqual(self.server.requests, [])
        self.assertTrue(deliver(device, lineage, archive, journal, first_anchor=True, **options)["anchor_ready"])
        self.assertEqual(deliver(device, lineage, archive, journal, **options)["state"], "active")
        record = json.loads(journal.read_text())
        self.assertEqual(record["fingerprint"], leaf_digest(fresh))
        self.assertEqual(self.server.handshakes, 3)
        mutations = [(r[1], r[4]) for r in self.server.requests
                     if r[0] == "POST" and r[1] != "/api/login"]
        self.assertEqual(mutations, [("/api/server-anchor", 1), (GET_STATUS, 2)])
        self.assertEqual((self.server.requests[-1][1], self.server.requests[-1][4]), (GET_STATUS, 3))
        for private_value in (OPERATOR_INPUT, SESSION, CSRF, "BEGIN CERTIFICATE", "key_hex"):
            self.assertNotIn(private_value, journal.read_text())

    def test_bootstrap_name_trust_and_authority_fail_before_authentication(self):
        device, lineage, archive, journal, _, leaf, _, _ = self.first_identity()
        binding = device.bootstrap
        encoded = pem(leaf).decode("ascii")
        with self.assertRaisesRegex(TransportError, "^bootstrap_leaf_pin_mismatch$"):
            _BootstrapFixture(ORIGIN, binding, pem(self.old).decode("ascii"), self.server_port)
        wrong_name = BootstrapIdentity("katmed-abcde0.local", LOOPBACK, binding.leaf_sha256)
        wrong_device = Device("device-001", HOST, ORIGIN, binding.leaf_sha256, pem(self.ca), True,
                              bootstrap=wrong_name)
        cases = (
            (_BootstrapFixture("https://other.example", binding, encoded, self.server_port),
             device, "origin_rejected"),
            (_BootstrapFixture(ORIGIN, wrong_name, encoded, self.server_port),
             wrong_device, "tls_peer_verification_failed"),
            (_BootstrapFixture(ORIGIN, binding, encoded, self.server_port),
             device, "tls_peer_verification_failed"),
        )
        alien_key = ec.generate_private_key(ec.SECP256R1())
        alien = bootstrap_leaf(BOOTSTRAP_NAME, alien_key, self.now)
        alien_context = self.server_context("not-authorized", alien_key, alien)
        for index, (transport, configured, error) in enumerate(cases):
            with self.subTest(error=error, case=index):
                attempted = journal.with_name(f"attempt-{index}.json")
                stage(configured, lineage, archive, attempted, self.now)
                if index == 2:
                    self.server.context = alien_context
                with self.assertRaisesRegex(TransportError, f"^{error}$"):
                    deliver(configured, lineage, archive, attempted, clock=lambda: self.now,
                            transport=self.transport, bootstrap_transport=transport,
                            operator_password=lambda: self.fail("Password read"), approved=True)
        self.assertEqual(self.server.requests, [])

    def test_lost_first_post_cannot_repeat_and_explicit_rollback_is_read_only(self):
        device, lineage, archive, journal, boot, _, _, _ = self.first_identity()
        status = self.deliver_response(None)
        submitted = False

        def respond(request):
            nonlocal submitted
            method, path, _, _, _ = request
            if path == GET_STATUS and method == "POST":
                self.assertFalse(submitted)
                submitted = True
                return 202, {"Content-Length": "20", "Connection": "close"}, b"{"
            if path == GET_STATUS and submitted:
                return 200, {}, b'{"state":"failed"}'
            return status(request)

        self.server.respond = respond
        options = dict(clock=lambda: self.now, transport=self.transport, bootstrap_transport=boot,
                       operator_password=lambda: OPERATOR_INPUT, approved=True)
        self.assertEqual(deliver(device, lineage, archive, journal, **options)["state"], "uncertain")
        before = list(self.server.requests)
        with self.assertRaisesRegex(TransportError, "^tls_peer_verification_failed$"):
            deliver(device, lineage, archive, journal, **options)
        self.assertEqual(self.server.requests, before)
        self.assertEqual(deliver(device, lineage, archive, journal, observe_bootstrap=True, **options)["state"],
                         "failed")
        self.assertEqual(len([r for r in self.server.requests if r[:2] == ("POST", GET_STATUS)]), 1)

    def resolve_stock_loopback(self, host, port, *args, **kwargs):
        self.assertEqual((host, port), ("localhost", self.server_port))
        answers = SYSTEM_GETADDRINFO(host, port, *args, **kwargs)
        loopback = [answer for answer in answers if answer[4] == self.server.server_address]
        self.assertTrue(loopback)
        return loopback

    def test_stock_https_uses_configured_origin_and_verified_socket(self):
        key, certificate = self.make_leaf("localhost")
        self.server.context = self.server_context("localhost", key, certificate)
        origin = f"https://localhost:{self.server_port}"
        transport = DeviceHTTPS(origin, pem(self.ca).decode("ascii"))
        headers = {"Origin": origin, "Content-Type": "application/json"}
        self.enterContext(patch("socket.getaddrinfo", side_effect=self.resolve_stock_loopback))
        with transport(origin) as connection:
            self.assertIs(connection.verified, True)
            self.assertEqual(connection.origin, origin)
            self.assertEqual(connection.leaf_sha256, leaf_digest(certificate))
            self.assertEqual(connection.request("GET", GET_STATUS, {}, None), (200, {}, {}))
            self.assertEqual(connection.request("POST", "/api/login", headers, b"{}"), (200, {}, {}))
        self.assertEqual([(r[0], r[1], r[4]) for r in self.server.requests],
                         [("GET", GET_STATUS, 1), ("POST", "/api/login", 1)])
        self.assertTrue(all(r[2]["Host"] == f"localhost:{self.server_port}" for r in self.server.requests))

    def test_stock_https_rejects_different_origin_before_resolution(self):
        origin = f"https://localhost:{self.server_port}"
        transport = DeviceHTTPS(origin, pem(self.ca).decode("ascii"))
        with self.assertRaisesRegex(TransportError, "^origin_rejected$"):
            with transport("https://localhost"):
                self.fail("Wrong origin was accepted")
        self.assertEqual(self.server.requests, [])
        self.assertEqual(self.server.handshakes, 0)

    def test_stock_https_rejects_wrong_hostname_or_ca_before_requests(self):
        origin = f"https://localhost:{self.server_port}"
        self.enterContext(patch("socket.getaddrinfo", side_effect=self.resolve_stock_loopback))
        with self.assertRaisesRegex(TransportError, "^tls_peer_verification_failed$"):
            with DeviceHTTPS(origin, pem(self.ca).decode("ascii"))(origin):
                self.fail("Wrong hostname was verified")
        key, certificate = self.make_leaf("localhost")
        self.server.context = self.server_context("localhost", key, certificate)
        foreign = issue("unrelated fixture root", ec.generate_private_key(ec.SECP256R1()), self.now)
        with self.assertRaisesRegex(TransportError, "^tls_peer_verification_failed$"):
            with DeviceHTTPS(origin, pem(foreign).decode("ascii"))(origin):
                self.fail("Wrong CA was verified")
        self.assertEqual(self.server.requests, [])

    def test_stock_https_requires_new_context_after_peer_rotation(self):
        key, old = self.make_leaf("localhost")
        self.server.context = self.server_context("old-localhost", key, old)
        self.server.respond = lambda _: (200, {"Connection": "close"}, b"{}")
        origin = f"https://localhost:{self.server_port}"
        transport = DeviceHTTPS(origin, pem(self.ca).decode("ascii"))
        self.enterContext(patch("socket.getaddrinfo", side_effect=self.resolve_stock_loopback))
        with transport(origin) as connection:
            self.assertEqual(connection.leaf_sha256, leaf_digest(old))
            self.assertEqual(connection.request("GET", GET_STATUS, {}, None), (200, {}, {}))
            key, fresh = self.make_leaf("localhost")
            self.server.context = self.server_context("fresh-localhost", key, fresh)
            with self.assertRaisesRegex(TransportError, "^connection_closed$"):
                connection.request("GET", GET_STATUS, {}, None)
            self.assertEqual(self.server.handshakes, 1)
        with transport(origin) as connection:
            self.assertEqual(connection.leaf_sha256, leaf_digest(fresh))
            self.assertEqual(connection.request("GET", GET_STATUS, {}, None), (200, {}, {}))
        self.assertEqual([r[4] for r in self.server.requests], [1, 2])

    def test_verified_socket_exposes_origin_and_peer_and_binds_requests(self):
        with self.transport(ORIGIN) as connection:
            self.assertIs(connection.verified, True)
            self.assertEqual(connection.origin, ORIGIN)
            self.assertEqual(connection.leaf_sha256, leaf_digest(self.old))
            self.assertEqual(connection.request("GET", GET_STATUS, {}, None), (200, {}, {}))
            self.assertEqual(connection.request("GET", GET_STATUS, {}, None), (200, {}, {}))
        self.assertEqual([request[4] for request in self.server.requests], [1, 1])
        self.assertEqual([request[2]["Host"] for request in self.server.requests], [HOST, HOST])

    def test_wrong_ca_or_hostname_is_rejected_before_any_request(self):
        foreign_key = ec.generate_private_key(ec.SECP256R1())
        foreign = issue("unrelated fixture root", foreign_key, self.now)
        for origin, ca in (("https://different.example", self.ca), (ORIGIN, foreign)):
            with self.subTest(origin=origin):
                with self.assertRaisesRegex(TransportError, "^tls_peer_verification_failed$"):
                    with self.make_transport(origin, ca)(origin):
                        self.fail("Unverified transport was made available")
        self.assertEqual(self.server.requests, [])

    def test_loopback_and_origin_are_checked_before_synthetic_auth(self):
        with self.assertRaisesRegex(TransportError, "^loopback_required$"):
            LoopbackHTTPS(ORIGIN, pem(self.ca).decode("ascii"), "192.0.2.1", self.server_port)
        with self.assertRaisesRegex(TransportError, "^origin_rejected$"):
            with self.transport("http://katmed.example"):
                self.fail("Wrong origin was accepted")
        with self.transport(ORIGIN) as connection:
            for headers, error in (
                ({**POST_HEADERS, "Origin": "https://different.example"}, "origin_rejected"),
                ({**POST_HEADERS, "hOsT": "different.example"}, "header_rejected"),
            ):
                with self.assertRaisesRegex(TransportError, f"^{error}$"):
                    connection.request("POST", "/api/login", headers, OPERATOR_INPUT.encode())
        self.assertEqual(self.server.requests, [])

    def test_redirect_stops_exchange_without_following_location(self):
        self.server.respond = lambda _: (307, {"Location": "https://different.example/api/login"}, b"{}")
        with self.transport(ORIGIN) as connection:
            with self.assertRaisesRegex(TransportError, "^redirect_rejected$"):
                connection.request("POST", "/api/login", POST_HEADERS, OPERATOR_INPUT.encode())
            with self.assertRaisesRegex(TransportError, "^connection_closed$"):
                connection.request("POST", "/api/login", POST_HEADERS, OPERATOR_INPUT.encode())
        self.assertEqual([(r[0], r[1]) for r in self.server.requests], [("POST", "/api/login")])
        self.assertEqual(self.server.handshakes, 1)

    def test_response_body_boundary_and_truncation(self):
        at_limit = b'{"data":"' + b"x" * (BODY_LIMIT - 11) + b'"}'
        self.assertEqual(len(at_limit), BODY_LIMIT)
        self.server.respond = lambda _: (200, {}, at_limit)
        with self.transport(ORIGIN) as connection:
            status, _, body = connection.request("GET", GET_STATUS, {}, None)
            self.assertEqual(status, 200)
            self.assertEqual(len(body["data"]), BODY_LIMIT - 11)
        for length, error in ((BODY_LIMIT + 1, "response_size"), (10, "response_truncated")):
            with self.subTest(length=length):
                self.server.respond = lambda _, length=length: (
                    200, {"Content-Length": str(length), "Connection": "close"}, b"{}"
                )
                with self.transport(ORIGIN) as connection:
                    with self.assertRaisesRegex(TransportError, f"^{error}$"):
                        connection.request("GET", GET_STATUS, {}, None)
                    with self.assertRaisesRegex(TransportError, "^connection_closed$"):
                        connection.request("GET", GET_STATUS, {}, None)

    def test_close_and_next_context_require_fresh_verified_identity(self):
        key, fresh = self.make_leaf()
        self.server.respond = lambda _: (200, {"Connection": "close"}, b"{}")
        with self.transport(ORIGIN) as connection:
            self.assertEqual(connection.leaf_sha256, leaf_digest(self.old))
            self.assertEqual(connection.request("GET", GET_STATUS, {}, None)[0], 200)
            self.server.context = self.server_context("fresh", key, fresh)
            with self.assertRaisesRegex(TransportError, "^connection_closed$"):
                connection.request("GET", GET_STATUS, {}, None)
            self.assertEqual(self.server.handshakes, 1)
        with self.transport(ORIGIN) as connection:
            self.assertEqual(connection.leaf_sha256, leaf_digest(fresh))
            self.assertEqual(connection.request("GET", GET_STATUS, {}, None)[0], 200)
        self.assertEqual([request[4] for request in self.server.requests], [1, 2])

    def make_lineage(self, key, certificate):
        lineage = self.directory / "live"
        archive = self.directory / "archive"
        lineage.mkdir()
        archive.mkdir()
        for name, value in (("cert", pem(certificate)), ("fullchain", pem(certificate)),
                            ("privkey", key_pem(key))):
            (archive / f"{name}1.pem").write_bytes(value)
            (lineage / f"{name}.pem").symlink_to(Path("../archive") / f"{name}1.pem")
        return lineage, archive

    def test_coordinator_uses_verified_tls_session_and_confirms_new_leaf(self):
        key, fresh = self.make_leaf()
        rotated = self.server_context("fresh", key, fresh)
        lineage, archive = self.make_lineage(key, fresh)
        journal = self.directory / "delivery.json"
        device = Device("device-001", HOST, ORIGIN, leaf_digest(self.old), pem(self.ca), True)
        stage(device, lineage, archive, journal, self.now)
        self.server.respond = self.deliver_response(rotated)

        result = deliver(device, lineage, archive, journal, clock=lambda: self.now,
                         transport=self.transport, operator_password=lambda: OPERATOR_INPUT,
                         approved=True)
        self.assertEqual(result["state"], "active")
        self.assertEqual(result["fingerprint"], leaf_digest(fresh))
        self.assertEqual(self.server.handshakes, 2)
        calls = self.server.requests
        posts = [r for r in calls if r[0] == "POST" and r[1] == GET_STATUS]
        self.assertEqual(len(posts), 1)
        sent = posts[0]
        self.assertEqual(sent[2]["Origin"], ORIGIN)
        self.assertEqual(sent[2]["Host"], HOST)
        self.assertEqual(sent[2]["Cookie"], f"katmed_session={SESSION}")
        self.assertEqual(sent[2]["X-Katmed-CSRF"], CSRF)
        self.assertEqual(json.loads(sent[3]), {
            "chain_pem": pem(fresh).decode("ascii"),
            "key_hex": key.private_numbers().private_value.to_bytes(32, "big").hex(),
            "password": OPERATOR_INPUT,
        })
        self.assertEqual((calls[-1][0], calls[-1][1], calls[-1][4]), ("GET", GET_STATUS, 2))
        self.assertNotIn(OPERATOR_INPUT, journal.read_text())

    def deliver_response(self, rotated, issued_connection=2):
        def respond(request):
            method, path, _, _, connection_id = request
            if path == "/api/login":
                return 200, {"set-cookie": f"katmed_session={SESSION}; Secure; HttpOnly; "
                                          "SameSite=Strict; Path=/"}, json.dumps(
                                              {"csrf_token": CSRF}
                                          ).encode()
            if path == "/api/acme-identifier":
                return 200, {}, json.dumps({"identifier": HOST}).encode()
            if method == "POST":
                self.server.context = rotated
                return 202, {}, b'{"state":"pending"}'
            return 200, {}, b'{"state":"active"}' if connection_id == issued_connection else b'{"state":"legacy"}'

        return respond

    def test_ca_valid_wrong_leaf_does_not_reach_coordinator_auth(self):
        key, fresh = self.make_leaf()
        lineage, archive = self.make_lineage(key, fresh)
        journal = self.directory / "delivery.json"
        device = Device("device-001", HOST, ORIGIN, "c" * 64, pem(self.ca), True)
        stage(device, lineage, archive, journal, self.now)
        with self.assertRaisesRegex(ValueError, "^untrusted_peer$"):
            deliver(device, lineage, archive, journal, clock=lambda: self.now,
                    transport=self.transport, operator_password=lambda: self.fail("Password read"),
                    approved=True)
        self.assertEqual(self.server.requests, [])


if __name__ == "__main__":
    unittest.main()
