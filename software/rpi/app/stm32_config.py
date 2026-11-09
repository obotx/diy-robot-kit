from __future__ import annotations

import json
import os
from pathlib import Path
from urllib.parse import urlsplit

from pydantic import Field, model_validator

from .stm32_protocol import StrictModel


class BoardConfig(StrictModel):
    port: str = Field(min_length=1, max_length=256)
    device_id: str = Field(min_length=1, max_length=80)


class STM32Config(StrictModel):
    arm: BoardConfig | None = None
    base: BoardConfig | None = None
    discovery: bool = False
    arm_only: bool = False
    service_only: bool = False
    baudrate: int = Field(default=115200, ge=9600, le=1000000)
    commissioned: bool = False
    max_z_difference_mm: float | None = Field(default=None, gt=0, le=1000)

    @model_validator(mode="after")
    def distinct_boards(self):
        if self.service_only and (self.commissioned or self.discovery):
            raise ValueError("Service-only mode requires an uncommissioned, pinned configuration")
        if self.discovery:
            if self.arm or self.base or self.commissioned or self.arm_only:
                raise ValueError("Discovery is uncommissioned and does not accept manual board assignments")
            return self
        if self.arm_only:
            if not self.arm or self.base:
                raise ValueError("Arm-only mode requires arm and no base")
            if self.commissioned and self.max_z_difference_mm is None:
                raise ValueError("Commissioning requires the confirmed Z difference limit")
            return self
        if not self.arm or not self.base:
            raise ValueError("Manual mode requires both boards")
        if self.arm.port == self.base.port or self.arm.device_id == self.base.device_id:
            raise ValueError("Arm and base must have distinct ports and device IDs")
        if self.commissioned and self.max_z_difference_mm is None:
            raise ValueError("Commissioning requires the confirmed Z difference limit")
        return self

    def check_ports(self, backend):
        if self.discovery:
            if backend != "stm32":
                raise ValueError("Discovery requires local STM32 hardware")
            return
        for board in (self.arm, self.base):
            if board is None:
                continue
            if backend == "stm32-emulator":
                url = urlsplit(board.port)
                if (url.scheme != "socket" or url.hostname != "127.0.0.1" or not url.port
                        or url.username or url.password or url.path or url.query or url.fragment):
                    raise ValueError("Emulator ports must be socket://127.0.0.1:PORT")
            elif "://" in board.port:
                raise ValueError("Hardware mode accepts local serial ports only")


def load_robot_settings():
    backend = os.getenv("OBOTX_ROBOT_BACKEND", "mock")
    if backend == "mock":
        return backend, None
    if backend == "stm32-auto":
        return "stm32", STM32Config(discovery=True)
    if backend == "arm-bench":
        from .arm_bench import BenchConfig
        path = os.environ["OBOTX_ARM_BENCH_CONFIG"]
        return backend, BenchConfig.model_validate_json(Path(path).read_text(encoding="utf-8"))
    if backend not in {"stm32", "stm32-emulator"}:
        raise ValueError("OBOTX_ROBOT_BACKEND must be mock, stm32-auto, stm32, stm32-emulator or arm-bench")
    path = os.getenv("OBOTX_STM32_CONFIG")
    if not path:
        raise ValueError("OBOTX_STM32_CONFIG must point to a controller configuration JSON file")
    config = STM32Config.model_validate(json.loads(Path(path).read_text(encoding="utf-8")))
    config.check_ports(backend)
    return backend, config
