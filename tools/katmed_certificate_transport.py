"""Bound HTTPS transport for certificate delivery."""

import hashlib
import http.client
import ipaddress
import json
import re
import socket
import ssl
from contextlib import contextmanager
from dataclasses import dataclass
from urllib.parse import urlsplit


BODY_LIMIT = 8192
IO_TIMEOUT = 3
ROUTES = {
    "GET": {"/api/acme-identifier", "/api/server-identity"},
    "POST": {"/api/login", "/api/server-anchor", "/api/server-identity"},
}
HEADERS = {"origin", "content-type", "cookie", "x-katmed-csrf"}


class TransportError(OSError):
    """Sanitized error without request or response contents."""


class _AddressedHTTPSConnection(http.client.HTTPSConnection):
    def __init__(self, host, port, context, address, timeout, tls_name=None):
        super().__init__(host, port, context=context, timeout=timeout)
        self._address = address
        self._context = context
        self._tls_name = tls_name or host

    def connect(self):
        raw = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            raw.settimeout(self.timeout)
            raw.connect(self._address)
            self.sock = self._context.wrap_socket(raw, server_hostname=self._tls_name)
        except BaseException:
            raw.close()
            raise


def _leaf_digest(sock):
    return hashlib.sha256(sock.getpeercert(binary_form=True)).hexdigest()


def _response_body(response):
    if 300 <= response.status < 400:
        raise TransportError("redirect_rejected")
    lengths = response.headers.get_all("Content-Length", [])
    if (response.headers.get_all("Transfer-Encoding") or len(lengths) != 1
            or not lengths[0].isascii() or not lengths[0].isdecimal()):
        raise TransportError("response_framing")
    if len(lengths[0]) > len(str(BODY_LIMIT)) or int(lengths[0]) > BODY_LIMIT:
        raise TransportError("response_size")
    length = int(lengths[0])
    data = response.read(length)
    if len(data) != length:
        raise TransportError("response_truncated")
    if response.headers.get_content_type() != "application/json":
        raise TransportError("response_type")
    payload = json.loads(data)
    if not isinstance(payload, dict):
        raise TransportError("response_json")
    return payload


class _VerifiedHTTPS:
    def __init__(self, origin, connection):
        self.origin = origin
        self._connection = connection
        self._socket = connection.sock
        self.leaf_sha256 = _leaf_digest(self._socket)
        self.verified = True

    def _check_binding(self):
        if self._connection.sock is not self._socket or self._socket.fileno() < 0:
            raise TransportError("connection_closed")
        if _leaf_digest(self._socket) != self.leaf_sha256:
            raise TransportError("peer_changed")

    def _check_request(self, method, path, headers, body):
        if path not in ROUTES.get(method, ()):
            raise TransportError("route_rejected")
        names = {name.lower(): value for name, value in headers.items()}
        if len(names) != len(headers) or names.keys() - HEADERS:
            raise TransportError("header_rejected")
        if "origin" in names and names["origin"] != self.origin:
            raise TransportError("origin_rejected")
        if method == "POST" and (
            names.get("origin") != self.origin or names.get("content-type") != "application/json"
            or not isinstance(body, bytes) or not 0 < len(body) <= BODY_LIMIT
        ):
            raise TransportError("invalid_post")
        if method == "GET" and body is not None:
            raise TransportError("invalid_get")

    def request(self, method, path, headers, body):
        self._check_request(method, path, headers, body)
        try:
            self._check_binding()
            self._connection.request(method, path, body=body, headers=headers)
            with self._connection.getresponse() as response:
                payload = _response_body(response)
                cookies = response.headers.get_all("Set-Cookie", [])
                if len(cookies) > 1:
                    raise TransportError("ambiguous_cookie")
                fields = {"Set-Cookie": cookies[0]} if cookies else {}
                return response.status, fields, payload
        except TransportError:
            self._connection.close()
            raise
        except (OSError, http.client.HTTPException, ValueError, RecursionError):
            self._connection.close()
            raise TransportError("https_exchange_failed") from None


class DeviceHTTPS:
    """Connect directly to one explicitly configured HTTPS origin."""

    def __init__(self, origin, ca_pem):
        try:
            authority = urlsplit(origin)
            host = authority.hostname
            https_port = authority.port or 443
        except (TypeError, ValueError):
            raise TransportError("invalid_target") from None
        if not host or origin not in {f"https://{host}", f"https://{host}:{https_port}"}:
            raise TransportError("invalid_origin")
        if not isinstance(ca_pem, str) or not ca_pem:
            raise TransportError("explicit_ca_required")
        self._context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        self._context.minimum_version = ssl.TLSVersion.TLSv1_2
        self._context.options |= ssl.OP_NO_RENEGOTIATION
        self._context.hostname_checks_common_name = False
        try:
            self._context.load_verify_locations(cadata=ca_pem)
        except (OSError, ValueError):
            raise TransportError("invalid_ca") from None
        self._origin = origin
        self._host = host
        self._https_port = https_port

    def _new_connection(self):
        return http.client.HTTPSConnection(
            self._host, self._https_port, context=self._context, timeout=IO_TIMEOUT
        )

    @contextmanager
    def __call__(self, origin):
        if origin != self._origin:
            raise TransportError("origin_rejected")
        connection = self._new_connection()
        connection.auto_open = 0
        try:
            try:
                connection.connect()
                verified = _VerifiedHTTPS(origin, connection)
            except ssl.SSLCertVerificationError:
                raise TransportError("tls_peer_verification_failed") from None
            except (OSError, ValueError):
                raise TransportError("tls_connection_failed") from None
            yield verified
        finally:
            connection.close()


class LoopbackHTTPS(DeviceHTTPS):
    """Connect to a numeric IPv4 loopback endpoint using the logical HTTPS origin."""

    def __init__(self, origin, ca_pem, address, port):
        try:
            target = ipaddress.IPv4Address(address)
        except (TypeError, ValueError):
            raise TransportError("invalid_target") from None
        if not target.is_loopback or type(port) is not int or not 1 <= port <= 65535:
            raise TransportError("loopback_required")
        super().__init__(origin, ca_pem)
        self._address = (str(target), port)

    def _new_connection(self):
        return _AddressedHTTPSConnection(
            self._host, self._https_port, self._context, self._address, IO_TIMEOUT
        )


@dataclass(frozen=True)
class BootstrapIdentity:
    """Independently authorized binding for one firmware bootstrap peer."""

    tls_name: str
    address: str
    leaf_sha256: str

    def __post_init__(self):
        try:
            address = ipaddress.IPv4Address(self.address)
        except (TypeError, ValueError):
            raise TransportError("invalid_bootstrap_binding") from None
        if (str(address) != self.address or not address.is_private
                or address.is_unspecified or address.is_reserved
                or not re.fullmatch(r"katmed-[0-9a-f]{6}\.local", self.tls_name)
                or not re.fullmatch(r"[0-9a-f]{64}", self.leaf_sha256)):
            raise TransportError("invalid_bootstrap_binding")


class BootstrapHTTPS(DeviceHTTPS):
    """Use the approved bootstrap SAN for TLS and the configured device origin for HTTP."""

    def __init__(self, origin, binding, leaf_pem):
        if not isinstance(binding, BootstrapIdentity):
            raise TransportError("invalid_bootstrap_binding")
        try:
            der = ssl.PEM_cert_to_DER_cert(leaf_pem)
        except (AttributeError, TypeError, ValueError):
            raise TransportError("invalid_bootstrap_leaf") from None
        if hashlib.sha256(der).hexdigest() != binding.leaf_sha256:
            raise TransportError("bootstrap_leaf_pin_mismatch")
        # Install exactly the independently supplied leaf in this private context.
        super().__init__(origin, ssl.DER_cert_to_PEM_cert(der))
        self.binding = binding

    def _new_connection(self):
        return _AddressedHTTPSConnection(
            self._host, self._https_port, self._context,
            (self.binding.address, self._https_port), IO_TIMEOUT, self.binding.tls_name
        )

    @contextmanager
    def __call__(self, origin):
        with super().__call__(origin) as verified:
            if verified.leaf_sha256 != self.binding.leaf_sha256:
                raise TransportError("bootstrap_peer_pin_mismatch")
            yield verified
