"""Read-only local Katmed integration; requires agreed firmware snapshot endpoint."""
import ssl
from functools import partial
import aiohttp
from homeassistant.exceptions import ConfigEntryNotReady
from homeassistant.const import EVENT_HOMEASSISTANT_STOP
from .client import KatmedClient
from .const import DOMAIN, PLATFORMS, CONF_CA_FILE, CONF_TLS_NAME
from .coordinator import KatmedCoordinator

async def async_setup_entry(hass, entry):
    try:
        context = await hass.async_add_executor_job(partial(ssl.create_default_context, cafile=entry.data.get(CONF_CA_FILE) or None))
    except (OSError, ssl.SSLError):
        raise ConfigEntryNotReady("TLS trust configuration unavailable") from None
    session = aiohttp.ClientSession(cookie_jar=aiohttp.DummyCookieJar(), trust_env=False)
    client = KatmedClient(session, entry.data["host"], entry.data["password"], context, entry.data.get(CONF_TLS_NAME))
    coordinator = KatmedCoordinator(hass, entry, client)
    try:
        await coordinator.async_config_entry_first_refresh()
        hass.data.setdefault(DOMAIN, {})[entry.entry_id] = (coordinator, session)
        await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
        async def close_on_stop(_event):
            await session.close()
        entry.async_on_unload(hass.bus.async_listen_once(EVENT_HOMEASSISTANT_STOP, close_on_stop))
    except BaseException:
        hass.data.get(DOMAIN, {}).pop(entry.entry_id, None)
        await session.close()
        raise
    return True

async def async_unload_entry(hass, entry):
    if not await hass.config_entries.async_unload_platforms(entry, PLATFORMS):
        return False
    _, session = hass.data[DOMAIN].pop(entry.entry_id)
    await session.close()
    return True
