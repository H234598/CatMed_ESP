"""Bounded, read-only HA snapshot contract (firmware agreement pending)."""
from dataclasses import dataclass
from datetime import datetime, timezone
import ipaddress
import re

class InvalidSnapshot(ValueError):
    """The peer did not supply the complete supported contract."""


def endpoint(host: str) -> str:
    """Existing firmware authorizes literal local IPv4 Host/Origin only."""
    return f"https://{ipaddress.IPv4Address(host)}"


def timestamp(value):
    if value is None:
        return None
    if not isinstance(value, str) or len(value) > 35 or not value.endswith("Z"):
        raise InvalidSnapshot("Invalid timestamp")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if parsed.tzinfo is None or parsed.utcoffset().total_seconds() != 0:
            raise ValueError
        return parsed.astimezone(timezone.utc)
    except (ValueError, OverflowError):
        raise InvalidSnapshot("Invalid timestamp") from None


@dataclass(frozen=True)
class Snapshot:
    device_id: str
    boot_id: str
    alarm_active: bool
    alarm_severity: str
    boot_notice: bool
    next_due: datetime | None
    last_dose: datetime | None
    last_dose_known: bool
    emergency_button_sequence: int

    @classmethod
    def parse(cls, data):
        fields = set(cls.__dataclass_fields__) | {"schema_version"}
        if not isinstance(data, dict) or set(data) != fields:
            raise InvalidSnapshot("Unsupported snapshot")
        if type(data["schema_version"]) is not int or data["schema_version"] != 1:
            raise InvalidSnapshot("Unsupported version")
        for key in ("alarm_active", "boot_notice", "last_dose_known"):
            if type(data[key]) is not bool:
                raise InvalidSnapshot("Invalid boolean")
        device = data["device_id"]
        boot = data["boot_id"]
        if not isinstance(device, str) or re.fullmatch(r"[0-9a-f]{12}", device) is None:
            raise InvalidSnapshot("Invalid identity")
        if not isinstance(boot, str) or re.fullmatch(r"[1-9][0-9]{0,19}", boot) is None or int(boot) > 2**64 - 1:
            raise InvalidSnapshot("Invalid boot domain")
        seq = data["emergency_button_sequence"]
        if type(seq) is not int or not 0 <= seq <= 2**53 - 1:
            raise InvalidSnapshot("Invalid sequence")
        severity = data["alarm_severity"]
        if severity not in ("none", "blue", "yellow", "red"):
            raise InvalidSnapshot("Invalid severity")
        if data["alarm_active"] != (severity != "none") or (data["boot_notice"] and data["alarm_active"]):
            raise InvalidSnapshot("Inconsistent alarm")
        last = timestamp(data["last_dose"])
        if last is not None and not data["last_dose_known"]:
            raise InvalidSnapshot("Inconsistent last dose")
        return cls(data["device_id"], data["boot_id"], data["alarm_active"], severity,
                   data["boot_notice"], timestamp(data["next_due"]), last,
                   data["last_dose_known"], seq)


class EventCursor:
    """No event on first observation, restart, boot change, or duplicate."""
    def __init__(self):
        self.previous = None

    def accept(self, current: Snapshot) -> bool:
        old = self.previous
        if old is not None and old.device_id != current.device_id:
            raise InvalidSnapshot("Device changed")
        if old is not None and old.boot_id == current.boot_id and current.emergency_button_sequence < old.emergency_button_sequence:
            raise InvalidSnapshot("Sequence regressed")
        self.previous = current
        return bool(old is not None and old.boot_id == current.boot_id
                    and current.emergency_button_sequence > old.emergency_button_sequence)
