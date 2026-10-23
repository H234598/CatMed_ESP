"""Read-only alarm and boot-notice state, with inherited availability."""
from homeassistant.components.binary_sensor import BinarySensorEntity
from .const import DOMAIN
from .entity import KatmedEntity

async def async_setup_entry(hass, entry, async_add_entities):
    coordinator, _ = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([KatmedBinary(coordinator, key, name) for key, name in (
        ("alarm_active", "Alarm"), ("boot_notice", "Boot notice"),
        ("last_dose_known", "Last dose known"))])

class KatmedBinary(KatmedEntity, BinarySensorEntity):
    def __init__(self, coordinator, key, name):
        super().__init__(coordinator, key)
        self.key = key
        self._attr_name = name

    @property
    def is_on(self):
        return getattr(self.coordinator.data, self.key)
