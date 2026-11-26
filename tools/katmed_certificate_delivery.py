"""Offline staging and operator-controlled certificate delivery via an injected transport."""

import fcntl
import ipaddress
import json
import os
import re
import stat
import tempfile
from contextlib import contextmanager
from dataclasses import asdict, dataclass
from datetime import datetime, timedelta
from http.cookies import CookieError, SimpleCookie
from pathlib import Path
from urllib.parse import urlsplit

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID
from cryptography.x509.verification import PolicyBuilder, Store, VerificationError

from tools.katmed_certificate_transport import BootstrapIdentity


CHAIN_LIMIT = 6143
ANCHOR_LIMIT = 2000
BODY_LIMIT = 8192
PIN = re.compile(r"[0-9a-f]{64}")
LABEL = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?")
FILES = ("cert", "fullchain", "privkey")
STATES = {"legacy", "active", "pending", "failed", "uncertain"}
RECORD_FIELDS = {"device", "dns", "origin", "anchor", "generation", "fingerprint",
                 "expires", "state", "phase", "anchor_ready", "deadline"}


class DeliveryError(ValueError):
    """A redacted local error. Messages never contain input or transport data."""


@dataclass(frozen=True)
class Device:
    identifier: str
    dns: str
    origin: str
    current_pin: str
    anchor_pem: bytes
    anchor_configured: bool
    bootstrap: BootstrapIdentity | None = None

    def __post_init__(self):
        if (not LABEL.fullmatch(self.identifier) or len(self.dns) > 253 or "." not in self.dns
                or not all(LABEL.fullmatch(label) for label in self.dns.split("."))
                or not PIN.fullmatch(self.current_pin)
                or type(self.anchor_configured) is not bool):
            raise DeliveryError("invalid_device")
        origin = urlsplit(self.origin)
        host = origin.hostname
        try:
            local = ipaddress.IPv4Address(host).is_private
        except ValueError:
            local = False
        if not host or (host != self.dns and not local) or self.origin not in {
            f"https://{host}", f"https://{host}:443",
        }:
            raise DeliveryError("invalid_origin")
        if self.bootstrap is not None and (
            not isinstance(self.bootstrap, BootstrapIdentity) or host != self.dns
            or self.current_pin != self.bootstrap.leaf_sha256
        ):
            raise DeliveryError("invalid_bootstrap_binding")


def _read_regular(path, limit):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as handle:
        if not stat.S_ISREG(os.fstat(handle.fileno()).st_mode):
            raise DeliveryError("non_regular_file")
        data = handle.read(limit + 1)
        if not data or len(data) > limit:
            raise DeliveryError("file_size")
        return data


def _fingerprint(cert):
    return cert.fingerprint(hashes.SHA256()).hex()


def _generation(lineage, archive):
    links = tuple((lineage / f"{name}.pem").readlink() for name in FILES)
    generations = set()
    for name, link in zip(FILES, links):
        match = re.fullmatch(rf"{name}([1-9][0-9]{{0,8}})\.pem", link.name)
        if (not match or (lineage / link).parent.resolve(strict=True) != archive):
            raise DeliveryError("lineage_link")
        generations.add(int(match[1]))
    if len(generations) != 1:
        raise DeliveryError("lineage_changed")
    return generations.pop()


def _validate_material(device, leaf_data, chain_data, key_data, now):
    if not 0 < len(device.anchor_pem) <= ANCHOR_LIMIT:
        raise DeliveryError("invalid_anchor_size")
    try:
        leaf, = x509.load_pem_x509_certificates(leaf_data)
        root, = x509.load_pem_x509_certificates(device.anchor_pem)
        chain = x509.load_pem_x509_certificates(chain_data)
        key = serialization.load_pem_private_key(key_data, password=None)
        if (leaf != chain[0]
                or root in chain or len(set(chain)) != len(chain)
                or chain_data != b"".join(cert.public_bytes(serialization.Encoding.PEM) for cert in chain)
                or device.anchor_pem != root.public_bytes(serialization.Encoding.PEM)
                or not root.extensions.get_extension_for_class(x509.BasicConstraints).value.ca):
            raise DeliveryError("invalid_chain")
        names = leaf.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
        usage = leaf.extensions.get_extension_for_class(x509.KeyUsage).value
        extended = leaf.extensions.get_extension_for_class(x509.ExtendedKeyUsage).value
        if (not isinstance(key, ec.EllipticCurvePrivateKey)
                or not isinstance(key.curve, ec.SECP256R1)
                or key.public_key() != leaf.public_key()
                or device.dns not in {n.lower() for n in names.get_values_for_type(x509.DNSName)}
                or not usage.digital_signature or ExtendedKeyUsageOID.SERVER_AUTH not in extended):
            raise DeliveryError("invalid_leaf")
        verifier = PolicyBuilder().store(Store([root])).time(now).build_server_verifier(
            x509.DNSName(device.dns)
        )
        if verifier.verify(leaf, chain[1:]) != [*chain, root]:
            raise DeliveryError("invalid_chain_order")
        return leaf, root, key
    except (ValueError, TypeError, x509.ExtensionNotFound, VerificationError):
        raise DeliveryError("invalid_material") from None


def _material(device, lineage, archive, now):
    """Read only the allowlisted archive generation selected by the live links."""
    lineage = Path(lineage).resolve(strict=True)
    archive = Path(archive).resolve(strict=True)
    generation = _generation(lineage, archive)
    leaf, chain, key = (
        _read_regular(archive / f"{name}{generation}.pem", CHAIN_LIMIT)
        for name in FILES
    )
    certificate, root, secret_key = _validate_material(device, leaf, chain, key, now)
    if _generation(lineage, archive) != generation:
        raise DeliveryError("lineage_changed")
    record = {
        "device": device.identifier, "dns": device.dns, "origin": device.origin,
        "anchor": _fingerprint(root), "generation": generation,
        "fingerprint": _fingerprint(certificate),
        "expires": certificate.not_valid_after_utc.isoformat(),
    }
    return record, chain.decode("ascii"), secret_key.private_numbers().private_value.to_bytes(32, "big").hex()


@contextmanager
def _locked(path):
    fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        yield fd
    finally:
        os.close(fd)


def _save(path, fd, record):
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False, mode="w") as output:
        temporary = Path(output.name)
        try:
            json.dump(record, output, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
            os.replace(temporary, path)
            os.fsync(fd)
        finally:
            temporary.unlink(missing_ok=True)


def _stored(path):
    try:
        record = json.loads(_read_regular(path, 2048))
    except FileNotFoundError:
        return None
    except ValueError:
        raise DeliveryError("invalid_journal") from None
    if (not isinstance(record, dict) or record.keys() not in (RECORD_FIELDS, RECORD_FIELDS | {"bootstrap"})
            or type(record["generation"]) is not int or record["generation"] < 1
            or not isinstance(record["fingerprint"], str) or not PIN.fullmatch(record["fingerprint"])
            or type(record["anchor_ready"]) is not bool
            or record["phase"] not in ("ready", "issued", "anchor_uncertain", "active")
            or record["state"] not in (*STATES, "ready")):
        raise DeliveryError("invalid_journal")
    try:
        if datetime.fromisoformat(record["expires"]).utcoffset() is None:
            raise ValueError
        if record["phase"] == "issued" and datetime.fromisoformat(record["deadline"]).utcoffset() is None:
            raise ValueError
    except (TypeError, ValueError):
        raise DeliveryError("invalid_journal") from None
    return record


def _restage(previous, record):
    if any(previous[k] != record[k] for k in ("device", "dns", "origin")):
        raise DeliveryError("journal_device_mismatch")
    _check_bootstrap_record(previous, record.get("bootstrap"))
    if previous["fingerprint"] == record["fingerprint"] and previous["anchor"] == record["anchor"]:
        if record["generation"] < previous["generation"]:
            raise DeliveryError("stale_generation")
        return {**previous, "generation": record["generation"]}
    if previous["state"] != "active" or record["generation"] <= previous["generation"]:
        raise DeliveryError("previous_generation_open")
    if "bootstrap" in record:
        raise DeliveryError("bootstrap_only_for_first_identity")
    return {**record, "state": "ready", "phase": "ready", "deadline": None,
            "anchor_ready": previous["anchor"] == record["anchor"] and previous["anchor_ready"]}


def stage(device, lineage, archive, journal, now):
    """Validate one allowlisted lineage and persist only its delivery metadata."""
    journal = Path(journal)
    with _locked(journal) as fd:
        record, _, _ = _material(device, lineage, archive, now)
        if device.bootstrap is not None:
            record["bootstrap"] = asdict(device.bootstrap)
        previous = _stored(journal)
        if previous:
            record = _restage(previous, record)
        else:
            record.update(state="ready", phase="ready", deadline=None, anchor_ready=device.anchor_configured)
        if record != previous:
            _save(journal, fd, record)
        return record


def _body(payload):
    data = json.dumps(payload, ensure_ascii=True, separators=(",", ":")).encode()
    if len(data) > BODY_LIMIT:
        raise DeliveryError("body_size")
    return data


def _check_bootstrap_record(record, configured):
    if record.get("bootstrap") != configured and not (
        record["state"] == "active" and configured is None
    ):
        raise DeliveryError("journal_bootstrap_mismatch")


def _check_peer(connection, device, record, bootstrap=False):
    pins = {record["fingerprint"]}
    if bootstrap:
        pins = {device.bootstrap.leaf_sha256}
    elif "bootstrap" not in record:
        pins.add(device.current_pin)
    if (connection.origin != device.origin or connection.verified is not True
            or connection.leaf_sha256 not in pins):
        raise DeliveryError("untrusted_peer")


def _session(connection, device, password):
    if not isinstance(password, str) or not 0 < len(password.encode()) <= 128 or "\0" in password:
        raise DeliveryError("invalid_operator_password")
    headers = {"Origin": device.origin, "Content-Type": "application/json"}
    code, fields, payload = connection.request(
        "POST", "/api/login", headers, _body({"method": "password", "password": password})
    )
    if code != 200:
        raise DeliveryError("login_rejected")
    cookie = SimpleCookie()
    try:
        cookie.load(fields["Set-Cookie"])
        session = cookie["katmed_session"]
        csrf = payload["csrf_token"]
        if (not PIN.fullmatch(session.value) or not PIN.fullmatch(csrf) or not session["secure"]
                or not session["httponly"] or session["samesite"].lower() != "strict"
                or session["path"] != "/" or session["domain"]):
            raise DeliveryError("invalid_session")
    except (KeyError, TypeError, CookieError):
        raise DeliveryError("invalid_session") from None
    headers.update({"Cookie": f"katmed_session={session.value}", "X-Katmed-CSRF": csrf})
    return headers


def _get(connection, route, headers, field):
    code, _, payload = connection.request("GET", route, {"Cookie": headers["Cookie"]}, None)
    if code != 200 or not isinstance(payload, dict) or not isinstance(payload.get(field), str):
        raise DeliveryError("invalid_readback")
    return payload[field]


def _observe(connection, headers, record, now):
    state = _get(connection, "/api/server-identity", headers, "state")
    exact = connection.leaf_sha256 == record["fingerprint"]
    if state not in STATES:
        raise DeliveryError("invalid_state")
    if state == "active" and exact and now <= datetime.fromisoformat(record["expires"]):
        record.update(state="active", phase="active")
        return False
    if record["phase"] == "ready":
        ready_states = {"legacy"} if "bootstrap" in record else {"legacy", "active"}
        if state in ready_states and not exact:
            return True
        raise DeliveryError("receiver_not_ready")
    if (record["phase"] == "issued" and state == "pending"
            and now < datetime.fromisoformat(record["deadline"])):
        record["state"] = "pending"
        return False
    record["state"] = "failed" if state == "failed" else "uncertain"
    return False


def _approved_record(journal, device, first_anchor):
    record = _stored(journal)
    if record is None:
        raise DeliveryError("not_staged")
    try:
        anchor, = x509.load_pem_x509_certificates(device.anchor_pem)
    except ValueError:
        raise DeliveryError("invalid_material") from None
    binding = {"device": device.identifier, "dns": device.dns, "origin": device.origin,
               "anchor": _fingerprint(anchor)}
    if any(record[k] != value for k, value in binding.items()):
        raise DeliveryError("journal_device_mismatch")
    _check_bootstrap_record(record, asdict(device.bootstrap) if device.bootstrap else None)
    if record["phase"] != "anchor_uncertain":
        if first_anchor and (record["anchor_ready"] or record["phase"] != "ready"):
            raise DeliveryError("anchor_already_processed")
        if not first_anchor and not record["anchor_ready"]:
            raise DeliveryError("anchor_approval_required")
    return record


def _submit_identity(connection, headers, record, journal, fd, body, now):
    record.update(phase="issued", state="uncertain", deadline=(now + timedelta(seconds=15)).isoformat())
    _save(journal, fd, record)
    try:
        code, _, payload = connection.request("POST", "/api/server-identity", headers, body)
        if code == 202 and payload == {"state": "pending"}:
            record["state"] = "pending"
    except OSError:
        pass


def _staged_material(device, lineage, archive, record, now):
    material, chain, key_hex = _material(device, lineage, archive, now)
    if any(record[k] != value for k, value in material.items()):
        raise DeliveryError("not_staged")
    return chain, key_hex


def _contact(device, record, transport, bootstrap_transport, observe_bootstrap):
    use_bootstrap = device.bootstrap is not None and record["phase"] == "ready"
    if observe_bootstrap:
        if device.bootstrap is None or record["phase"] != "issued":
            raise DeliveryError("bootstrap_observation_unavailable")
        use_bootstrap = True
    if not use_bootstrap:
        return transport, False
    if getattr(bootstrap_transport, "binding", None) != device.bootstrap:
        raise DeliveryError("bootstrap_transport_mismatch")
    return bootstrap_transport, True


def _check_identifier(connection, device, headers):
    if _get(connection, "/api/acme-identifier", headers, "identifier").lower() != device.dns:
        raise DeliveryError("identifier_mismatch")


def deliver(device, lineage, archive, journal, *, clock, transport, operator_password,
            approved=False, first_anchor=False, bootstrap_transport=None, observe_bootstrap=False):
    """Run one operator-authorized step with an injected transport.

    A connection exposes origin, verified and leaf_sha256 and supports a context
    manager and request(method, path, headers, body) returning status, headers and JSON.
    A new invocation observes an outstanding submission and never repeats its POST.
    Explicit bootstrap observation is available only for an issued first transition.
    """
    if approved is not True:
        raise DeliveryError("operator_approval_required")
    if type(first_anchor) is not bool:
        raise DeliveryError("invalid_anchor_approval")
    if type(observe_bootstrap) is not bool or (observe_bootstrap and first_anchor):
        raise DeliveryError("invalid_bootstrap_observation")
    journal = Path(journal)
    with _locked(journal) as fd:
        record = _approved_record(journal, device, first_anchor)
        if record["phase"] == "anchor_uncertain":
            return record
        contact, bootstrap = _contact(device, record, transport, bootstrap_transport, observe_bootstrap)
        if record["phase"] == "ready":
            chain, key_hex = _staged_material(device, lineage, archive, record, clock())
        with contact(device.origin) as connection:
            _check_peer(connection, device, record, bootstrap)
            password = operator_password()
            headers = _session(connection, device, password)
            _check_identifier(connection, device, headers)
            if not _observe(connection, headers, record, clock()):
                _save(journal, fd, record)
                return record
            if first_anchor:
                _install_anchor(connection, headers, record, device, password, journal, fd)
                return record
            body = _body({"chain_pem": chain, "key_hex": key_hex, "password": password})
            _submit_identity(connection, headers, record, journal, fd, body, clock())
        _observe_submission(device, record, clock, transport, bootstrap, password)
        _save(journal, fd, record)
        return record


def _observe_submission(device, record, clock, transport, bootstrap, password):
    try:
        with transport(device.origin) as connection:
            _check_peer(connection, device, record)
            headers = _session(connection, device, password)
            if bootstrap:
                _check_identifier(connection, device, headers)
            _observe(connection, headers, record, clock())
    except (OSError, DeliveryError):
        pass


def _install_anchor(connection, headers, record, device, password, journal, fd):
    body = _body({"anchor_pem": device.anchor_pem.decode("ascii"), "password": password})
    record.update(phase="anchor_uncertain", state="uncertain")
    _save(journal, fd, record)
    try:
        code, _, payload = connection.request("POST", "/api/server-anchor", headers, body)
    except OSError:
        return
    if code == 200 and payload == {"anchor_configured": True}:
        record.update(anchor_ready=True, phase="ready", state="ready")
        _save(journal, fd, record)
