"""Observed local emergency-menu clicks only; no HA command/medical acknowledgment."""
from homeassistant.components.event import EventEntity, EventDeviceClass
from homeassistant.core import callback
from .const import DOMAIN
from .entity import KatmedEntity

async def async_setup_entry(hass, entry, async_add_entities):
    coordinator, _ = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([KatmedEmergencyButton(coordinator)])

class KatmedEmergencyButton(KatmedEntity, EventEntity):
    _attr_name = "Emergency alarm button"
    _attr_device_class = EventDeviceClass.BUTTON
    _attr_event_types = ["pressed"]

    def __init__(self, coordinator):
        super().__init__(coordinator, "emergency_alarm_button")
        self.generation = coordinator.event_generation

    @callback
    def _handle_coordinator_update(self):
        if self.coordinator.last_update_success and self.coordinator.event_generation > self.generation:
            self.generation = self.coordinator.event_generation
            self._trigger_event("pressed")
        self.async_write_ha_state()
