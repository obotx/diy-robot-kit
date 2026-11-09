from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol

from .config import AppConfig


@dataclass(frozen=True)
class OutputState:
    name: str
    pin: int
    state: bool


class HardwareController(Protocol):
    def list_outputs(self) -> list[OutputState]:
        ...

    def set_output(self, name: str, state: bool) -> OutputState:
        ...

    def toggle_output(self, name: str) -> OutputState:
        ...

    def close(self) -> None:
        ...


class BaseOutputController:
    def __init__(self, pins: dict[str, int]) -> None:
        self._pins = pins
        self._states = {name: False for name in pins}

    def list_outputs(self) -> list[OutputState]:
        return [
            OutputState(name=name, pin=pin, state=self._states[name])
            for name, pin in self._pins.items()
        ]

    def set_output(self, name: str, state: bool) -> OutputState:
        self._require_output(name)
        self._write_output(name, state)
        self._states[name] = state
        return OutputState(name=name, pin=self._pins[name], state=state)

    def toggle_output(self, name: str) -> OutputState:
        self._require_output(name)
        return self.set_output(name, not self._states[name])

    def close(self) -> None:
        pass

    def _write_output(self, name: str, state: bool) -> None:
        pass

    def _require_output(self, name: str) -> None:
        if name not in self._pins:
            known = ", ".join(sorted(self._pins))
            raise UnknownOutputError(f"Unknown output '{name}'. Known outputs: {known}")


class MockOutputController(BaseOutputController):
    pass


class GpioOutputController(BaseOutputController):
    def __init__(self, pins: dict[str, int], active_high: bool) -> None:
        super().__init__(pins)
        try:
            from gpiozero import DigitalOutputDevice
        except ImportError as exc:
            raise RuntimeError("gpiozero is required for GPIO hardware backend") from exc

        self._devices = {
            name: DigitalOutputDevice(pin, active_high=active_high, initial_value=False)
            for name, pin in pins.items()
        }

    def _write_output(self, name: str, state: bool) -> None:
        if state:
            self._devices[name].on()
        else:
            self._devices[name].off()

    def close(self) -> None:
        for device in self._devices.values():
            device.close()


class UnknownOutputError(ValueError):
    pass


def create_hardware_controller(config: AppConfig) -> HardwareController:
    if config.hardware_backend == "mock":
        return MockOutputController(config.relay_pins)
    if config.hardware_backend == "gpio":
        return GpioOutputController(config.relay_pins, config.active_high)
    raise ValueError(f"Unsupported hardware backend: {config.hardware_backend}")
