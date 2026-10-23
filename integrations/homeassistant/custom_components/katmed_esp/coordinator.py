"""One shared bounded local poll; transport failures make all entities unavailable."""
from datetime import timedelta
import logging
from homeassistant.exceptions import ConfigEntryAuthFailed
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed
from .client import CannotConnect, InvalidAuth
from .protocol import EventCursor, InvalidSnapshot
from .const import DOMAIN

class KatmedCoordinator(DataUpdateCoordinator):
    def __init__(self, hass, entry, client):
        super().__init__(hass, logging.getLogger(__name__), name=DOMAIN,
                         config_entry=entry, update_interval=timedelta(seconds=30))
        self.client = client
        self.cursor = EventCursor()
        self.event_generation = 0
        self.device_id = entry.unique_id

    async def _async_update_data(self):
        try:
            snapshot = await self.client.read()
            if snapshot.device_id != self.device_id:
                raise InvalidSnapshot("Device changed")
            if self.cursor.accept(snapshot):
                self.event_generation += 1
            return snapshot
        except InvalidAuth:
            raise ConfigEntryAuthFailed("Web authentication required") from None
        except (CannotConnect, InvalidSnapshot):
            raise UpdateFailed("Device unavailable or unsupported response") from None
