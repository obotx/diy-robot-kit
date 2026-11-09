from __future__ import annotations

import math
import logging
import time
from typing import Annotated, Callable, Literal

from pydantic import BaseModel, ConfigDict, Field, SecretStr, TypeAdapter, model_validator


WATCHDOG_SECONDS = 0.6
MAX_LINEAR_MPS = 0.2
MAX_Z_MPS = 0.19
HOMING_TIMEOUT_SECONDS = 120.0
LOG = logging.getLogger(__name__)


class Message(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)
    seq: int = Field(ge=0, le=9007199254740991)

    @model_validator(mode="after")
    def axis_limits(self):
        if getattr(self, "type", None) in ("arm_move", "arm_configure"):
            if self.axis != "X" and self.velocity_um_s > 190000:
                raise ValueError("Z speed exceeds 190 mm/s")
            acceleration = getattr(self, "acceleration_um_s2", None)
            if acceleration is not None and (acceleration > (500000 if self.axis == "X" else 100000)
                                             or acceleration < (10000 if self.axis == "X" else 5000)):
                raise ValueError("Axis acceleration outside firmware limits")
        return self


class Enable(Message):
    type: Literal["enable"]
    enabled: bool
    source: str = Field(default="pc", min_length=1, max_length=40)


class Stop(Message):
    type: Literal["stop"]


class Home(Message):
    type: Literal["home"]
    axis: Literal["ZL", "Z", "X"]


class HomeKeepalive(Message):
    type: Literal["home_keepalive"]


class ReferenceStep(Message):
    type: Literal["reference_step"]
    step: Literal["level_down", "level_up", "level_center", "home_z", "lift_z", "home_x"]


class ArmMove(Message):
    type: Literal["arm_move"]
    axis: Literal["ZL", "ZR", "X", "Z"]
    position_um: int = Field(ge=0, le=1000000)
    velocity_um_s: int = Field(ge=1, le=500000)
    acceleration_um_s2: int | None = Field(default=None, ge=5000, le=500000)


class ArmConfigure(Message):
    type: Literal["arm_configure"]
    axis: Literal["ZL", "ZR", "X", "Z"]
    min_um: int = Field(ge=0, le=999999)
    max_um: int = Field(ge=1, le=1000000)
    velocity_um_s: int = Field(ge=1, le=500000)


class ClearFault(Message):
    type: Literal["clear_fault"]
    controller: Literal["arm", "base"] = "arm"


class Acceleration(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)
    base_mps2: float = Field(default=0.3, ge=0.01, le=1.0)
    rotation_radps2: float = Field(default=0.6, ge=0.05, le=2.0)
    z_mps2: float = Field(default=0.1, ge=0.005, le=0.1)
    x_mps2: float = Field(default=0.2, ge=0.01, le=0.5)
    gripper_per_s: float = Field(default=2.0, ge=0.02, le=4.0)


class Motion(Message):
    type: Literal["motion"]
    vx_mps: float = Field(ge=-MAX_LINEAR_MPS, le=MAX_LINEAR_MPS)
    vy_mps: float = Field(ge=-MAX_LINEAR_MPS, le=MAX_LINEAR_MPS)
    wz_radps: float = Field(ge=-0.6, le=0.6)
    z_mps: float = Field(ge=-MAX_Z_MPS, le=MAX_Z_MPS)
    zl_mps: float = Field(default=0.0, ge=-MAX_Z_MPS, le=MAX_Z_MPS)
    zr_mps: float = Field(default=0.0, ge=-MAX_Z_MPS, le=MAX_Z_MPS)
    x_mps: float = Field(ge=-MAX_LINEAR_MPS, le=MAX_LINEAR_MPS)
    gripper: float = Field(ge=-1, le=1)
    acceleration: Acceleration = Field(default_factory=Acceleration)


class ServiceUnlock(Message):
    type: Literal["service_unlock"]
    password: SecretStr = Field(min_length=1, max_length=256)
    confirm_bypass: Literal[True]


class ServiceJog(Message):
    type: Literal["service_jog"]
    zl_mps: float = Field(ge=-0.19, le=0.19)
    zr_mps: float = Field(ge=-0.19, le=0.19)
    x_mps: float = Field(ge=-0.5, le=0.5)
    gripper: float = Field(default=0.0, ge=-1, le=1)
    vx_mps: float = Field(default=0.0, ge=-MAX_LINEAR_MPS, le=MAX_LINEAR_MPS)
    vy_mps: float = Field(default=0.0, ge=-MAX_LINEAR_MPS, le=MAX_LINEAR_MPS)
    wz_radps: float = Field(default=0.0, ge=-0.6, le=0.6)
    acceleration: Acceleration = Field(default_factory=Acceleration)


class BaseWheelJog(Message):
    type: Literal["base_wheel_jog"]
    wheel: Literal["FL", "FR", "RL", "RR"]
    pwm: float = Field(ge=-0.15, le=0.15)


class ServiceLock(Message):
    type: Literal["service_lock"]


COMMAND = TypeAdapter(Annotated[Enable | Stop | Motion | Home | HomeKeepalive | ReferenceStep | ArmMove | ArmConfigure | ClearFault | ServiceUnlock | ServiceJog | BaseWheelJog | ServiceLock, Field(discriminator="type")])
ZERO = dict(vx_mps=0.0, vy_mps=0.0, wz_radps=0.0, z_mps=0.0,
            zl_mps=0.0, zr_mps=0.0, x_mps=0.0, gripper=0.0)


class ControlError(ValueError):
    pass


class RobotController:
    """Single-event-loop ownership, validation and operator command lease."""

    def __init__(self, clock: Callable[[], float] = time.monotonic):
        self.clock = clock
        self.owner: str | None = None
        self.source: str | None = None
        self.last_command = clock()
        self.last_tick = clock()
        self.reason = "startup"
        self.command = ZERO.copy()
        self.acceleration = Acceleration()
        self.control_generation = 0

    def stop(self, reason: str):
        self.control_generation += 1
        if self.owner is not None:
            LOG.warning("Control stopped: reason=%s previous_reason=%s owner=%s command_age_ms=%d",
                        reason, self.reason, self.owner,
                        round(max(0.0, self.clock() - self.last_command) * 1000))
        self.command = ZERO.copy()
        self.owner = None
        self.source = None
        self.reason = reason

    def disconnect(self, client_id: str):
        if self.owner == client_id:
            self.stop("disconnect")

    def invalid(self, client_id: str):
        if self.owner == client_id:
            self.stop("invalid_command")

    def apply(self, client_id: str, message: Enable | Stop | Motion):
        self.tick()
        if isinstance(message, (ReferenceStep, ArmMove, ArmConfigure, ClearFault, ServiceUnlock, ServiceJog, BaseWheelJog, ServiceLock)):
            raise ControlError("arm_actions_not_supported")
        if isinstance(message, Stop):
            self.stop("operator")
            return
        if isinstance(message, Enable):
            if self.owner not in (None, client_id):
                raise ControlError("busy")
            if not message.enabled:
                self.stop("released")
            elif self.owner is None:
                self.owner = client_id
                self.source = message.source
                self.command = ZERO.copy()
                self.last_command = self.clock()
                self.reason = "enabled"
            # Repeated enable must not keep an old movement alive.
            return
        if self.owner != client_id:
            raise ControlError("not_owner")
        if isinstance(message, (Home, HomeKeepalive)):
            raise ControlError("homing_not_supported")
        if message.z_mps and (message.zl_mps or message.zr_mps):
            self.stop("invalid_command")
            raise ControlError("conflicting_z_commands")
        if math.hypot(message.vx_mps, message.vy_mps) > MAX_LINEAR_MPS + 1e-9:
            self.stop("invalid_command")
            raise ControlError("base_speed_limit")
        self.command = message.model_dump(exclude={"type", "seq", "acceleration"})
        self.acceleration = message.acceleration
        self.last_command = self.clock()

    def tick(self):
        now = self.clock()
        dt = max(0.0, min(now - self.last_tick, 0.1))
        self.last_tick = now
        if self.owner and now - self.last_command >= WATCHDOG_SECONDS:
            self.stop("watchdog")
        return dt

    def control_snapshot(self):
        return {
            "type": "telemetry", "protocol": 1,
            "enabled": self.owner is not None, "owner": self.owner,
            "source": self.source, "reason": self.reason,
            "watchdog_ms": int(WATCHDOG_SECONDS * 1000),
            "command_age_ms": round((self.clock() - self.last_command) * 1000) if self.owner else None,
            "command": self.command.copy(),
            "acceleration": self.acceleration.model_dump(),
            "limits": {"linear_mps": MAX_LINEAR_MPS, "rotation_radps": 0.6},
        }


class RobotSimulator(RobotController):
    """No commands are sent to GPIO or STM32."""

    def __init__(self, clock: Callable[[], float] = time.monotonic):
        super().__init__(clock)
        self.z_left_mm = 350.0
        self.z_right_mm = 350.0
        self.x_mm = 300.0
        self.gripper_pct = 70.0
        self.pose = dict(x_m=0.0, y_m=0.0, yaw_rad=0.0)
        self.velocity = ZERO.copy()
        self.homed_axes = {"ZL": False, "ZR": False, "X": False}
        self.homing_axis = None
        self.homing_started = 0.0

    def apply(self, client_id, message):
        self.tick()
        if isinstance(message, Home):
            if message.axis != "ZL":
                raise ControlError("simulator_home_zl_only")
            if self.owner is not None:
                raise ControlError("release_before_homing")
            self.owner = client_id
            self.source = "homing"
            self.command = ZERO.copy()
            self.velocity = ZERO.copy()
            self.homing_axis = message.axis
            self.homed_axes[message.axis] = False
            self.last_command = self.homing_started = self.clock()
            self.reason = "homing"
            return
        if isinstance(message, HomeKeepalive):
            if self.owner != client_id or not self.homing_axis:
                raise ControlError("not_homing_owner")
            self.last_command = self.clock()
            return
        if self.homing_axis and not isinstance(message, Stop):
            if isinstance(message, Enable) and not message.enabled:
                return super().apply(client_id, message)
            raise ControlError("homing_in_progress")
        super().apply(client_id, message)

    def stop(self, reason):
        super().stop(reason)
        self.velocity = ZERO.copy()
        self.homing_axis = None

    def tick(self):
        dt = super().tick()
        if self.homing_axis:
            if self.clock() - self.homing_started >= HOMING_TIMEOUT_SECONDS:
                self.stop("homing_timeout")
                return
            # Simulation only: move towards virtual reference switches at 20 mm/s.
            names = ("z_left_mm",)
            for name in names:
                setattr(self, name, max(0.0, getattr(self, name) - 20.0 * dt))
            if all(getattr(self, name) == 0.0 for name in names):
                self.homed_axes[self.homing_axis] = True
                self.stop("homing_complete")
            return
        target = self.command.copy()
        target["zl_mps"] = target["z_mps"] or target["zl_mps"]
        target["zr_mps"] = target["z_mps"] or target["zr_mps"]
        target["z_mps"] = 0.0
        a = self.acceleration
        distance = ZERO.copy()
        vector_delta = math.hypot(target["vx_mps"] - self.velocity["vx_mps"], target["vy_mps"] - self.velocity["vy_mps"])
        for key, limit in (("vx_mps", a.base_mps2), ("vy_mps", a.base_mps2),
                           ("wz_radps", a.rotation_radps2), ("zl_mps", a.z_mps2),
                           ("zr_mps", a.z_mps2), ("x_mps", a.x_mps2), ("gripper", a.gripper_per_s)):
            if key in ("vx_mps", "vy_mps") and vector_delta:
                limit *= abs(target[key] - self.velocity[key]) / vector_delta
            self.velocity[key], distance[key] = ramp(self.velocity[key], target[key], limit, dt)
        if self.command["z_mps"] and distance["zl_mps"] == distance["zr_mps"]:
            # Group motion preserves the existing height difference, including at a limit.
            delta = distance["zl_mps"] * 1000
            delta = max(-min(self.z_left_mm, self.z_right_mm),
                        min(1000.0 - max(self.z_left_mm, self.z_right_mm), delta))
            self.z_left_mm += delta
            self.z_right_mm += delta
        else:
            self.z_left_mm = max(0.0, min(1000.0, self.z_left_mm + distance["zl_mps"] * 1000))
            self.z_right_mm = max(0.0, min(1000.0, self.z_right_mm + distance["zr_mps"] * 1000))
        self.x_mm = max(0.0, min(1000.0, self.x_mm + distance["x_mps"] * 1000))
        self.gripper_pct = max(0.0, min(100.0, self.gripper_pct + distance["gripper"] * 50))
        angle = self.pose["yaw_rad"]
        self.pose["x_m"] += distance["vx_mps"] * math.cos(angle) - distance["vy_mps"] * math.sin(angle)
        self.pose["y_m"] += distance["vx_mps"] * math.sin(angle) + distance["vy_mps"] * math.cos(angle)
        self.pose["yaw_rad"] = (angle + distance["wz_radps"] + math.pi) % (2 * math.pi) - math.pi

    def snapshot(self):
        self.tick()
        return {
            **self.control_snapshot(), "backend": "mock", "base": self.pose.copy(),
            "simulated_velocity": self.velocity.copy(),
            "arm": {"z_left_mm": round(self.z_left_mm, 1), "z_right_mm": round(self.z_right_mm, 1),
                    "x_mm": round(self.x_mm, 1), "travel_mm": 1000,
                    "homed": all(self.homed_axes.values()), "homed_axes": self.homed_axes.copy()},
            "homing": {"supported_axes": ["ZL"], "active_axis": self.homing_axis,
                       "blocked_reason": None},
            "gripper": {"opening_pct": round(self.gripper_pct, 1)},
            "hardware_connected": False,
        }


def ramp(current, target, acceleration, dt):
    """Return the new velocity and integrated displacement, without overshoot."""
    if not dt or current == target or not acceleration:
        return current, current * dt
    ramp_time = min(dt, abs(target - current) / acceleration)
    end = target if ramp_time < dt else current + math.copysign(acceleration * ramp_time, target - current)
    return end, (current + end) * 0.5 * ramp_time + end * (dt - ramp_time)
