"""Existing HTTPS password session; no medical commands or raw error logging."""
import asyncio
import json
import re
import aiohttp
from .protocol import Snapshot, InvalidSnapshot, endpoint

class CannotConnect(Exception):
    """Transport or unsupported peer."""

class InvalidAuth(Exception):
    """Existing web login is required."""

class KatmedClient:
    def __init__(self, session, host, password, ssl_context, tls_server_name=None):
        self.session = session
        self.base = endpoint(host)
        self._password = password
        self._ssl = ssl_context
        if tls_server_name and (len(tls_server_name) > 253 or any(
            re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label) is None
            for label in tls_server_name.split(".")
        )):
            raise ValueError("Invalid TLS server name")
        self._tls_server_name = tls_server_name or host
        self._token = None

    async def _request(self, method, path, body=None):
        headers = {"Origin": self.base}
        if self._token:
            headers["Cookie"] = f"katmed_session={self._token}"
        try:
            async with self.session.request(
                method, self.base + path, json=body, headers=headers,
                ssl=self._ssl, server_hostname=self._tls_server_name, allow_redirects=False,
                timeout=aiohttp.ClientTimeout(total=10),
            ) as response:
                if response.status in (401, 403):
                    raise InvalidAuth
                if response.status != 200 or response.content_type != "application/json":
                    raise CannotConnect
                raw = bytearray()
                while True:
                    chunk = await response.content.read(4097 - len(raw))
                    if not chunk:
                        break
                    raw.extend(chunk)
                    if len(raw) > 4096:
                        raise CannotConnect
                def unique_object(pairs):
                    value = {}
                    for key, item in pairs:
                        if key in value:
                            raise ValueError("Duplicate field")
                        value[key] = item
                    return value
                value = json.loads(raw, object_pairs_hook=unique_object)
                if path == "/api/login":
                    cookie = response.cookies.get("katmed_session")
                    token = cookie.value if cookie else ""
                    if not isinstance(value, dict) or value.get("authenticated") is not True or value.get("setup_required") is not False:
                        raise InvalidAuth
                    if len(token) != 64 or any(c not in "0123456789abcdefABCDEF" for c in token):
                        raise InvalidAuth
                    self._token = token
                return value
        except (aiohttp.ClientError, asyncio.TimeoutError, ValueError, UnicodeError):
            raise CannotConnect from None

    async def read(self):
        if not self._token:
            await self._request("POST", "/api/login", {"method": "password", "password": self._password})
        try:
            value = await self._request("GET", "/api/ha/status")
        except InvalidAuth:
            # A single session renewal; never retry a medical operation.
            self._token = None
            await self._request("POST", "/api/login", {"method": "password", "password": self._password})
            value = await self._request("GET", "/api/ha/status")
        try:
            return Snapshot.parse(value)
        except InvalidSnapshot:
            raise CannotConnect from None
