"""Read-only SmartShunt Instant Readout, isolated from the motion event loop."""
import asyncio
import math
import os
from pathlib import Path
import threading
import time

from fastapi import APIRouter, Request
from pydantic import BaseModel, ConfigDict, Field, SecretStr, field_validator, model_validator


def config_path():
    return Path(os.getenv("OBOTX_BATTERY_CONFIG", str(Path.home() / ".config/obotx/battery.json")))


class BatteryConfig(BaseModel):
    model_config = ConfigDict(extra="forbid")
    address: str = Field(pattern=r"^(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$")
    advertisement_key: SecretStr
    adapter: str = Field(default="hci0", pattern=r"^hci[0-9]+$")
    low_pct: float = Field(default=20, gt=5, le=100, allow_inf_nan=False)
    critical_pct: float = Field(default=5, gt=0, le=20, allow_inf_nan=False)
    stale_seconds: float = Field(default=15, ge=5, le=60, allow_inf_nan=False)

    @field_validator("advertisement_key")
    @classmethod
    def key_format(cls, value):
        key = value.get_secret_value()
        if len(key) != 32 or any(c not in "0123456789abcdefABCDEF" for c in key):
            raise ValueError("Instant Readout key must contain 32 hexadecimal characters")
        return value

    @model_validator(mode="after")
    def thresholds(self):
        if self.critical_pct >= self.low_pct:
            raise ValueError("Critical threshold must be lower than recharge threshold")
        return self


class BatteryMonitor:
    def __init__(self, path=None, clock=time.monotonic, scanner_factory=None):
        self.clock = clock
        self.config = None
        self.error = "not_configured"
        self.reading = None
        self.received_at = None
        self.last_packet = None
        self.level = "normal"
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.thread = None
        self.scanner_factory = scanner_factory
        try:
            self.config = BatteryConfig.model_validate_json((path or config_path()).read_text(encoding="utf-8"))
            self.error = "waiting"
        except FileNotFoundError:
            pass
        except Exception:
            # Never log validation inputs: they can contain the encryption key.
            self.error = "configuration_error"

    def start(self):
        if self.config and self.thread is None:
            self.thread = threading.Thread(target=lambda: asyncio.run(self._scan()), daemon=True,
                                           name="smartshunt-ble")
            self.thread.start()

    async def _scan(self):
        try:
            from bleak import BleakScanner
            from victron_ble.devices.battery_monitor import BatteryMonitor as Parser
            parser = Parser(self.config.advertisement_key.get_secret_value())
        except ImportError:
            with self.lock:
                self.error = "dependency_missing"
            return
        factory = self.scanner_factory or BleakScanner
        while not self.stop_event.is_set():
            try:
                async with factory(detection_callback=lambda device, adv: self.receive(device, adv, parser),
                                   bluez={"adapter": self.config.adapter}):
                    # Reopen periodically to recover even when an adapter disappears silently.
                    deadline = time.monotonic() + 30
                    while not self.stop_event.is_set() and time.monotonic() < deadline:
                        await asyncio.sleep(0.25)
            except Exception:
                with self.lock:
                    self.error = "bluetooth_unavailable"
                # Retry without blocking robot control or requiring a server restart.
                for _ in range(20):
                    if self.stop_event.is_set():
                        return
                    await asyncio.sleep(0.25)

    def receive(self, device, advertisement, parser):
        if device.address.upper() != self.config.address.upper():
            return
        raw = advertisement.manufacturer_data.get(0x02E1)
        # Victron product advertisement 0x10, Battery Monitor record 0x02.
        if not raw or len(raw) < 23 or raw[0] != 0x10 or raw[4] != 0x02:
            return
        try:
            data = parser.parse(raw)
            values = {"soc_pct": data.get_soc(), "voltage_v": data.get_voltage(),
                      "current_a": data.get_current(), "remaining_minutes": data.get_remaining_mins()}
            if any(value is not None and not math.isfinite(value) for value in values.values()):
                raise ValueError("nonfinite")
            soc = values["soc_pct"]
            if soc is not None and not 0 <= soc <= 100:
                raise ValueError("soc_out_of_range")
            with self.lock:
                # Duplicate BlueZ callbacks must not keep a frozen sample fresh.
                if raw == self.last_packet:
                    return
                self.last_packet = raw
                self.reading, self.received_at, self.error = values, self.clock(), None
                if soc is not None:
                    if soc <= self.config.critical_pct:
                        self.level = "critical"
                    elif self.level == "critical" and soc < self.config.critical_pct + 2:
                        pass
                    elif soc <= self.config.low_pct:
                        self.level = "low"
                    elif self.level in ("low", "critical") and soc < self.config.low_pct + 2:
                        self.level = "low"
                    else:
                        self.level = "normal"
        except Exception:
            with self.lock:
                self.error = "decode_error"

    def snapshot(self):
        with self.lock:
            age = None if self.received_at is None else max(0, self.clock() - self.received_at)
            fresh = bool(self.config and age is not None and age <= self.config.stale_seconds and not self.error)
            values = self.reading if fresh else {}
            status = ("live" if values.get("soc_pct") is not None else "soc_unknown") if fresh else (
                self.error or "stale")
            return {"source": "victron_ble", "status": status, "fresh": fresh,
                    "soc_pct": values.get("soc_pct"), "voltage_v": values.get("voltage_v"),
                    "remaining_minutes": values.get("remaining_minutes"),
                    "current_a": values.get("current_a"), "age_seconds": None if age is None else round(age, 1),
                    "level": self.level if fresh and values.get("soc_pct") is not None else "unknown",
                    "last_known_level": self.level if self.reading else None,
                    "low_pct": self.config.low_pct if self.config else 20,
                    "critical_pct": self.config.critical_pct if self.config else 5}

    def close(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=5)


router = APIRouter()


@router.get("/api/battery")
def battery_status(request: Request):
    return request.app.state.battery.snapshot()
