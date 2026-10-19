#!/usr/bin/env python3
"""Local asset and API-shape browser fixture test; firmware handlers are separate."""

import http.client
import json
import ssl
import subprocess
import sys
from html.parser import HTMLParser
from pathlib import Path


HERE = Path(__file__).resolve().parent
ASSETS = HERE.parents[1] / "firmware/components/katmed_web/ui"
PASSWORD = "Fixture-pass-2026!"


class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.resources = []

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if tag == "script":
            self.resources.append(values.get("src"))
        elif tag == "link" and values.get("rel") == "stylesheet":
            self.resources.append(values.get("href"))


def request(info, path, method="GET", body=None, cookie=None, csrf=None, client=False, plain=False):
    headers = {"Accept": "application/json"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    if cookie:
        headers["Cookie"] = cookie
    if csrf:
        headers["X-Katmed-CSRF"] = csrf
    if plain:
        connection = http.client.HTTPConnection("127.0.0.1", info["http"], timeout=5)
    else:
        context = ssl.create_default_context(cafile=info["server_cert"])
        if client:
            context.load_cert_chain(info["client_cert"], info["client_key"])
        connection = http.client.HTTPSConnection("127.0.0.1", info["https"], context=context, timeout=5)
    connection.request(method, path, None if body is None else json.dumps(body), headers)
    response = connection.getresponse()
    payload = response.read()
    result = response.status, dict(response.getheaders()), payload
    connection.close()
    return result


def json_body(result):
    return json.loads(result[2])


def expect(status, result):
    assert result[0] == status, (status, result[0], result[2][:100])
    return result


def run(info):
    files = {name: (ASSETS / name).read_bytes() for name in ("index.html", "style.css", "app.js")}
    assert sum(map(len, files.values())) < 32768
    links = Links()
    links.feed(files["index.html"].decode())
    assert links.resources == ["/style.css", "/app.js"]
    assert b"@import" not in files["style.css"] and b"url(" not in files["style.css"]

    redirect = expect(308, request(info, "/", plain=True))
    assert redirect[1]["Location"] == f"https://127.0.0.1:{info['https']}/"
    for path, name in (("/", "index.html"), ("/style.css", "style.css"), ("/app.js", "app.js")):
        fetched = expect(200, request(info, path))
        assert fetched[2] == files[name]
        assert "script-src 'self'" in fetched[1]["Content-Security-Policy"]

    bootstrap = json_body(expect(200, request(info, "/api/bootstrap")))
    assert bootstrap["setup_required"] and not bootstrap["authenticated"]
    assert bootstrap["device_name"] == "KatmedESP-002"
    assert "setup_code" not in bootstrap and "pairing_code" not in bootstrap
    expect(401, request(info, "/api/config"))
    expect(401, request(info, "/api/info"))
    setup = {"password": PASSWORD, "setup_nonce": bootstrap["setup_nonce"]}
    expect(401, request(info, "/api/setup", "POST", {**setup, "setup_code": "000000"}))
    assert json_body(expect(200, request(info, "/api/bootstrap")))["setup_required"]
    fixed_cookie = "katmed_session=attacker-fixed-session"
    assert not json_body(expect(200, request(info, "/api/bootstrap", cookie=fixed_cookie)))["authenticated"]
    setup_reply = expect(200, request(info, "/api/setup", "POST",
                                      {**setup, "setup_code": "314159"}, cookie=fixed_cookie))
    setup_body = json_body(setup_reply)
    assert not setup_body["setup_required"] and setup_body["authenticated"]
    assert setup_body["auth_method"] == "password" and setup_body["csrf_token"]
    assert setup_body["device_name"] == "KatmedESP-002"
    setup_cookie = setup_reply[1]["Set-Cookie"]
    assert setup_cookie.startswith("katmed_session=")
    assert all(part in setup_cookie for part in ("Secure", "HttpOnly", "SameSite=Strict"))
    assert setup_cookie.split(";", 1)[0] != fixed_cookie
    assert json_body(expect(200, request(info, "/api/bootstrap",
                                      cookie=setup_cookie.split(";", 1)[0])))["authenticated"]
    expect(401, request(info, "/api/config", cookie=fixed_cookie))
    expect(409, request(info, "/api/setup", "POST", {**setup, "setup_code": "314159"}))
    expect(401, request(info, "/api/login", "POST", {"method": "password", "password": "wrong"}))
    login = expect(200, request(info, "/api/login", "POST", {"method": "password", "password": PASSWORD}))
    cookie_header = login[1]["Set-Cookie"]
    assert cookie_header.startswith("katmed_session=")
    assert all(part in cookie_header for part in ("Secure", "HttpOnly", "SameSite=Strict"))
    cookie = cookie_header.split(";", 1)[0]
    csrf = json_body(login)["csrf_token"]
    device_info = json_body(expect(200, request(info, "/api/info", cookie=cookie)))
    assert device_info["device_name"] == device_info["hostname"] == "KatmedESP-002"
    assert device_info["name_state"] == "assigned"
    config = json_body(expect(200, request(info, "/api/config", cookie=cookie)))
    assert config["next_due_state"] == "active" and config["revision"] == "r1"
    change = {"field": "image_hours", "value": "2,5", "revision": "r1"}
    expect(403, request(info, "/api/config", "PUT", change, cookie=cookie))
    expect(422, request(info, "/api/config", "PUT", {**change, "field": "unknown"}, cookie, csrf))
    changed = json_body(expect(200, request(info, "/api/config", "PUT", change, cookie, csrf)))
    assert changed["image_hours"] == "2,5" and changed["revision"] == "r2"
    expect(409, request(info, "/api/config", "PUT", change, cookie, csrf))
    preview = json_body(expect(200, request(info, "/api/config/preview-time", "POST",
                                            {"time": "09:45", "revision": "r2"}, cookie, csrf)))
    due = {"field": "next_due_hhmm", "value": "09:45", "revision": "r2"}
    expect(409, request(info, "/api/config", "PUT", {**due, "preview_token": "wrong"}, cookie, csrf))
    saved = json_body(expect(200, request(info, "/api/config", "PUT",
                                          {**due, "preview_token": preview["preview_token"]}, cookie, csrf)))
    assert saved["next_due_hhmm"] == "09:45" and saved["revision"] == "r3"
    assert json_body(expect(200, request(info, "/api/config", cookie=cookie)))["next_due_hhmm"] == "09:45"

    pem = Path(info["client_cert"]).read_text()
    expect(401, request(info, "/api/client-cert", "POST", {"certificate_pem": pem, "password": "wrong"}, cookie, csrf))
    expect(200, request(info, "/api/client-cert", "POST", {"certificate_pem": pem, "password": PASSWORD}, cookie, csrf))
    assert json_body(expect(200, request(info, "/api/client-cert", cookie=cookie)))["enrolled"]
    expect(401, request(info, "/api/login", "POST", {"method": "certificate"}))
    cert_login = expect(200, request(info, "/api/login", "POST", {"method": "certificate"}, client=True))
    assert json_body(cert_login)["auth_method"] == "certificate"
    cookie = cert_login[1]["Set-Cookie"].split(";", 1)[0]
    csrf = json_body(cert_login)["csrf_token"]
    expect(401, request(info, "/api/client-cert", "DELETE", {"password": "wrong"}, cookie, csrf))
    expect(200, request(info, "/api/client-cert", "DELETE", {"password": PASSWORD}, cookie, csrf))
    expect(401, request(info, "/api/login", "POST", {"method": "certificate"}, client=True))
    expect(200, request(info, "/api/logout", "POST", {}, cookie, csrf))
    expect(401, request(info, "/api/config", cookie=cookie))
    print("asset bytes", sum(map(len, files.values())))
    print("HTTP redirect, HTTPS assets, setup/code, login, CSRF, config/preview/readback, TLS client certificate, removal, logout: OK")


def main():
    process = subprocess.Popen([sys.executable, str(HERE / "serve_fixture.py")],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        line = process.stdout.readline()
        if not line:
            raise RuntimeError("Local fixture did not start")
        run(json.loads(line))
    finally:
        process.terminate()
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()


if __name__ == "__main__":
    main()
