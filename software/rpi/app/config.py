from __future__ import annotations

import os
import platform
from dataclasses import dataclass


DEFAULT_RELAY_PINS = {
    "relay_1": 17,
    "relay_2": 27,
    "relay_3": 22,
    "relay_4": 23,
}


@dataclass(frozen=True)
class AppConfig:
    relay_pins: dict[str, int]
    active_high: bool
    hardware_backend: str


def load_config() -> AppConfig:
    return AppConfig(
        relay_pins=_parse_relay_pins(os.getenv("OBOTX_RELAY_PINS")),
        active_high=_parse_bool(os.getenv("OBOTX_ACTIVE_HIGH", "true")),
        hardware_backend=_select_hardware_backend(os.getenv("OBOTX_HARDWARE")),
    )


def _parse_relay_pins(value: str | None) -> dict[str, int]:
    if not value:
        return dict(DEFAULT_RELAY_PINS)

    result: dict[str, int] = {}
    for item in value.split(","):
        name, separator, pin = item.partition(":")
        if not separator or not name.strip() or not pin.strip():
            raise ValueError(
                "OBOTX_RELAY_PINS must use format name:pin,name2:pin2"
            )
        result[name.strip()] = int(pin.strip())
    return result


def _parse_bool(value: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    raise ValueError(f"Invalid boolean value: {value}")


def _select_hardware_backend(value: str | None) -> str:
    if value:
        normalized = value.strip().lower()
        if normalized not in {"auto", "gpio", "mock"}:
            raise ValueError("OBOTX_HARDWARE must be auto, gpio, or mock")
        if normalized != "auto":
            return normalized

    machine = platform.machine().lower()
    system = platform.system().lower()
    if system == "linux" and machine in {"armv7l", "aarch64"}:
        return "gpio"
    return "mock"
