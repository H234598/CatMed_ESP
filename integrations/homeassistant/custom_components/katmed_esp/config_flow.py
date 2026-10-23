"""Explicit local IPv4/TLS-trust setup; never changes device provisioning."""
import ssl
from functools import partial
import aiohttp
import voluptuous as vol
from homeassistant import config_entries
from homeassistant.helpers import selector
from .client import KatmedClient, CannotConnect, InvalidAuth
from .const import DOMAIN, CONF_CA_FILE, CONF_TLS_NAME
from .protocol import endpoint

class KatmedConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input=None):
        errors = {}
        if user_input is not None:
            try:
                snapshot = await self._validate_input(user_input)
                await self.async_set_unique_id(snapshot.device_id)
                self._abort_if_unique_id_configured()
                return self.async_create_entry(title="Katmed ESP", data=user_input)
            except InvalidAuth:
                errors["base"] = "invalid_auth"
            except (OSError, ssl.SSLError, ValueError, CannotConnect):
                errors["base"] = "cannot_connect"
        return self.async_show_form(step_id="user", data_schema=vol.Schema({
            vol.Required("host"): str,
            vol.Required("password"): selector.TextSelector(selector.TextSelectorConfig(type=selector.TextSelectorType.PASSWORD)),
            vol.Optional(CONF_CA_FILE, default=""): str,
            vol.Optional(CONF_TLS_NAME, default=""): str,
        }), errors=errors)

    async def _validate_input(self, data):
        endpoint(data["host"])
        context = await self.hass.async_add_executor_job(partial(
            ssl.create_default_context, cafile=data.get(CONF_CA_FILE) or None))
        async with aiohttp.ClientSession(cookie_jar=aiohttp.DummyCookieJar(), trust_env=False) as session:
            return await KatmedClient(session, data["host"], data["password"], context,
                                      data.get(CONF_TLS_NAME)).read()

    async def async_step_reauth(self, entry_data):
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(self, user_input=None):
        errors = {}
        if user_input is not None:
            entry = self._get_reauth_entry()
            data = {**entry.data, "password": user_input["password"]}
            try:
                snapshot = await self._validate_input(data)
                await self.async_set_unique_id(snapshot.device_id)
                self._abort_if_unique_id_mismatch()
                return self.async_update_reload_and_abort(entry, data_updates=user_input)
            except InvalidAuth:
                errors["base"] = "invalid_auth"
            except (OSError, ssl.SSLError, ValueError, CannotConnect):
                errors["base"] = "cannot_connect"
        return self.async_show_form(step_id="reauth_confirm", data_schema=vol.Schema({
            vol.Required("password"): selector.TextSelector(selector.TextSelectorConfig(type=selector.TextSelectorType.PASSWORD)),
        }), errors=errors)
