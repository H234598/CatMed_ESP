"""No guessed dates: unknown UTC remains unavailable as a timestamp value."""
from homeassistant.components.sensor import SensorEntity, SensorDeviceClass
from .const import DOMAIN
from .entity import KatmedEntity

async def async_setup_entry(hass, entry, async_add_entities):
    coordinator, _ = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([KatmedSensor(coordinator, key, name) for key, name in (
        ("alarm_severity", "Alarm severity"), ("next_due", "Next due"),
        ("last_dose", "Last dose"))])

class KatmedSensor(KatmedEntity, SensorEntity):
    def __init__(self, coordinator, key, name):
        super().__init__(coordinator, key)
        self.key = key
        self._attr_name = name
        if key != "alarm_severity":
            self._attr_device_class = SensorDeviceClass.TIMESTAMP
        else:
            self._attr_device_class = SensorDeviceClass.ENUM
            self._attr_options = ["none", "blue", "yellow", "red"]

    @property
    def native_value(self):
        return getattr(self.coordinator.data, self.key)
