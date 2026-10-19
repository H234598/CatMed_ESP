#!/usr/bin/env python3
"""Loopback-only UI transport fixture. It does not implement firmware handlers."""

import hashlib
import json
import secrets
import signal
import ssl
import subprocess
import tempfile
import threading
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ASSETS = Path(__file__).resolve().parents[2] / "firmware/components/katmed_web/ui"
FILES = {"/": ("index.html", "text/html; charset=utf-8"),
         "/style.css": ("style.css", "text/css; charset=utf-8"),
         "/app.js": ("app.js", "text/javascript; charset=utf-8")}


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.password = None
        self.nonce = secrets.token_hex(16)
        self.setup_code = "314159"  # Synthetic local fixture, never part of bootstrap.
        self.session = None
        self.csrf = None
        self.auth_method = None
        self.enrolled_der = None
        self.revision = 1
        self.preview = None
        self.config = {
            "medication_hours": "12", "image_hours": "6",
            "next_due_hhmm": "08:30", "next_due_state": "active",
            "restore_status": "none",
            "clock_style": "digital_large", "orientation": 0,
            "rotation": "name_order"
        }

    def snapshot(self):
        return {**self.config, "revision": f"r{self.revision}"}


STATE = State()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    @property
    def https(self):
        return isinstance(self.connection, ssl.SSLSocket)

    def send_bytes(self, status, payload=b"", content_type="application/json; charset=utf-8", cookie=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; form-action 'self'; base-uri 'none'")
        if cookie is not None:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        self.wfile.write(payload)

    def send_json(self, status, payload, cookie=None):
        self.send_bytes(status, json.dumps(payload, separators=(",", ":")).encode(), cookie=cookie)

    def redirect(self):
        self.send_response(308)
        self.send_header("Location", f"https://127.0.0.1:{self.server.https_port}{self.path}")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def authenticated(self):
        cookie = SimpleCookie()
        try:
            cookie.load(self.headers.get("Cookie", ""))
        except Exception:
            return False
        return STATE.password is not None and STATE.session is not None and cookie.get("katmed_session") is not None and cookie["katmed_session"].value == STATE.session

    def csrf_ok(self):
        return self.headers.get("X-Katmed-CSRF") == STATE.csrf

    def bootstrap(self, authenticated):
        return {
            "setup_required": STATE.password is None,
            "authenticated": authenticated,
            "auth_method": STATE.auth_method if authenticated else None,
            "csrf_token": STATE.csrf if authenticated else None,
            "setup_nonce": STATE.nonce if STATE.password is None else None,
            "client_certificate": {"enrolled": STATE.enrolled_der is not None}
        }

    def body(self):
        try:
            size = int(self.headers.get("Content-Length", "-1"))
            if size < 0 or size > 10000:
                return None
            value = json.loads(self.rfile.read(size))
            return value if isinstance(value, dict) else None
        except (ValueError, UnicodeError, json.JSONDecodeError):
            return None

    def do_GET(self):
        if not self.https:
            self.redirect()
            return
        with STATE.lock:
            if self.path == "/api/bootstrap":
                self.send_json(200, self.bootstrap(self.authenticated()))
            elif self.path == "/api/config":
                self.send_json(200, STATE.snapshot()) if self.authenticated() else self.send_json(401, {"error": "unauthorized"})
            elif self.path == "/api/client-cert":
                self.send_json(200, {"enrolled": STATE.enrolled_der is not None,
                                     "subject": "CN=Fixture Client" if STATE.enrolled_der else None}) if self.authenticated() else self.send_json(401, {"error": "unauthorized"})
            elif self.path in FILES:
                name, content_type = FILES[self.path]
                self.send_bytes(200, (ASSETS / name).read_bytes(), content_type)
            else:
                self.send_json(404, {"error": "not_found"})

    def do_POST(self):
        self.mutate("POST")

    def do_PUT(self):
        self.mutate("PUT")

    def do_DELETE(self):
        self.mutate("DELETE")

    def mutate(self, method):
        if not self.https:
            self.redirect()
            return
        data = self.body()
        if data is None:
            self.send_json(400, {"error": "bad_json"})
            return
        with STATE.lock:
            if self.path == "/api/setup" and method == "POST":
                if STATE.password is not None:
                    self.send_json(409, {"error": "already_setup"})
                elif data.get("setup_nonce") != STATE.nonce or data.get("setup_code") != STATE.setup_code:
                    self.send_json(401, {"error": "denied"})
                elif not isinstance(data.get("password"), str) or len(data["password"]) < 12:
                    self.send_json(422, {"error": "invalid"})
                else:
                    STATE.password = data["password"]
                    STATE.session = secrets.token_hex(16)
                    STATE.csrf = secrets.token_hex(16)
                    STATE.auth_method = "password"
                    self.send_json(200, self.bootstrap(True),
                                   f"katmed_session={STATE.session}; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=3600")
                return
            if self.path == "/api/login" and method == "POST":
                cert = self.connection.getpeercert(binary_form=True)
                valid_password = data.get("method") == "password" and data.get("password") == STATE.password
                valid_cert = data.get("method") == "certificate" and cert and STATE.enrolled_der == hashlib.sha256(cert).digest()
                if STATE.password is not None and (valid_password or valid_cert):
                    STATE.session = secrets.token_hex(16)
                    STATE.csrf = secrets.token_hex(16)
                    STATE.auth_method = data["method"]
                    self.send_json(200, self.bootstrap(True),
                                   f"katmed_session={STATE.session}; Path=/; Secure; HttpOnly; SameSite=Strict")
                else:
                    self.send_json(401, {"error": "unauthorized"})
                return
            if not self.authenticated():
                self.send_json(401, {"error": "unauthorized"})
                return
            if not self.csrf_ok():
                self.send_json(403, {"error": "csrf"})
                return
            if self.path == "/api/logout" and method == "POST":
                STATE.session = None
                STATE.csrf = None
                STATE.auth_method = None
                self.send_json(200, {}, "katmed_session=; Path=/; Max-Age=0; Secure; HttpOnly; SameSite=Strict")
            elif self.path == "/api/config/preview-time" and method == "POST":
                time = data.get("time")
                if data.get("revision") != f"r{STATE.revision}":
                    self.send_json(409, {"error": "stale"})
                elif not isinstance(time, str) or len(time) != 5 or time[2] != ":" or not time[:2].isdigit() or not time[3:].isdigit() or int(time[:2]) > 23 or int(time[3:]) > 59:
                    self.send_json(422, {"error": "invalid"})
                else:
                    STATE.preview = (time, STATE.revision)
                    self.send_json(200, {"time": time, "regular_local": f"19.10.2026 {time} CEST",
                                         "effective_local": f"19.10.2026 {time} CEST",
                                         "night_advanced": False, "skipped_nonexistent_time": False,
                                         "preview_token": f"p{STATE.revision}:{time}"})
            elif self.path == "/api/config" and method == "PUT":
                field, value = data.get("field"), data.get("value")
                if data.get("revision") != f"r{STATE.revision}":
                    self.send_json(409, {"error": "stale"})
                elif field not in ("medication_hours", "image_hours", "next_due_hhmm", "clock_style", "orientation", "rotation"):
                    self.send_json(422, {"error": "invalid"})
                elif field == "next_due_hhmm" and (STATE.preview != (value, STATE.revision) or data.get("preview_token") != f"p{STATE.revision}:{value}"):
                    self.send_json(409, {"error": "stale"})
                elif field in ("medication_hours", "image_hours") and (not isinstance(value, str) or not value.replace(",", ".").replace(".", "", 1).isdigit() or float(value.replace(",", ".")) <= 0):
                    self.send_json(422, {"error": "invalid"})
                elif field == "clock_style" and value not in ("digital_large", "digital_date", "analog") or field == "orientation" and value not in (0, 90, 180, 270) or field == "rotation" and value not in ("off", "name_order", "random"):
                    self.send_json(422, {"error": "invalid"})
                else:
                    STATE.config[field] = value
                    if field == "next_due_hhmm":
                        STATE.config["next_due_state"] = "active"
                        STATE.config["restore_status"] = "none"
                    STATE.revision += 1
                    STATE.preview = None
                    self.send_json(200, STATE.snapshot())
            elif self.path == "/api/client-cert" and method == "POST":
                pem = data.get("certificate_pem")
                if data.get("password") != STATE.password or not isinstance(pem, str):
                    self.send_json(401, {"error": "denied"})
                else:
                    try:
                        der = ssl.PEM_cert_to_DER_cert(pem)
                        STATE.enrolled_der = hashlib.sha256(der).digest()
                        self.send_json(200, {"enrolled": True})
                    except ValueError:
                        self.send_json(422, {"error": "invalid"})
            elif self.path == "/api/client-cert" and method == "DELETE":
                if data.get("password") != STATE.password:
                    self.send_json(401, {"error": "denied"})
                else:
                    STATE.enrolled_der = None
                    self.send_json(200, {"enrolled": False})
            else:
                self.send_json(404, {"error": "not_found"})


def certificate(path, name, client=False):
    cert, key = path / f"{name}.crt", path / f"{name}.key"
    command = ["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
               "-nodes", "-days", "1", "-subj", f"/CN={name}", "-keyout", str(key), "-out", str(cert)]
    if client:
        command += ["-addext", "basicConstraints=critical,CA:TRUE", "-addext", "extendedKeyUsage=clientAuth"]
    else:
        command += ["-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost"]
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return cert, key


def main():
    with tempfile.TemporaryDirectory(prefix="katmed-ui-fixture-") as directory:
        path = Path(directory)
        server_cert, server_key = certificate(path, "localhost")
        client_cert, client_key = certificate(path, "Fixture Client", client=True)
        http = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        https = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        http.https_port = https.server_port
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(server_cert), str(server_key))
        context.load_verify_locations(cafile=str(client_cert))
        context.verify_mode = ssl.CERT_OPTIONAL
        https.socket = context.wrap_socket(https.socket, server_side=True)
        for server in (http, https):
            threading.Thread(target=server.serve_forever, daemon=True).start()
        print(json.dumps({"http": http.server_port, "https": https.server_port,
                          "server_cert": str(server_cert), "client_cert": str(client_cert),
                          "client_key": str(client_key)}), flush=True)
        stop = threading.Event()
        signal.signal(signal.SIGTERM, lambda *_args: stop.set())
        signal.signal(signal.SIGINT, lambda *_args: stop.set())
        stop.wait()
        http.shutdown()
        https.shutdown()


if __name__ == "__main__":
    main()
