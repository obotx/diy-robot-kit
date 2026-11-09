"""ObotX serial protocol v1. Not compatible with the arm debug ASCII console."""

from __future__ import annotations

import json
import time
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field

from .robot import Acceleration, ZERO

MAX_FRAME = 2048
REPLY_TIMEOUT = 0.15
DEVICE_WATCHDOG_MS = 250
CAPABILITIES = {"full_state_v1", "local_watchdog", "acceleration_limits"}


class LinkError(ValueError):
    pass


class StrictModel(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)


class Envelope(StrictModel):
    protocol: int = Field(ge=1, le=1)
    session: str = Field(pattern=r"^[a-f0-9]{32}$")
    seq: int = Field(ge=0, le=9007199254740991)
    role: Literal["arm", "base"]
    device_id: str = Field(min_length=1, max_length=80)
    boot_id: str = Field(min_length=1, max_length=80)


class Identity(Envelope):
    type: Literal["identity"]
    firmware: str = Field(min_length=1, max_length=80)
    capabilities: list[str] = Field(max_length=16)
    watchdog_ms: int = Field(ge=DEVICE_WATCHDOG_MS, le=DEVICE_WATCHDOG_MS)
    enabled: bool
    max_z_difference_mm: float | None = Field(default=None, gt=0, le=1000)


class ArmState(StrictModel):
    z_left_mm: float | None = Field(ge=0, le=1000)
    z_right_mm: float | None = Field(ge=0, le=1000)
    x_mm: float | None = Field(ge=0, le=1000)
    homed: bool
    gripper_pct: float | None = Field(ge=0, le=100)
    mode: int = Field(default=0, ge=0, le=3)
    active_axis: Literal["ZL", "ZR", "X", "Z"] | None = None
    homed_axes: dict[str, bool] = Field(default_factory=dict)
    fault_code: int = Field(default=0, ge=0, le=11)
    service_active: bool = False
    service_release_required: bool = False
    limit_switches: dict[str, bool] = Field(default_factory=dict)
    level_active: bool = False
    reference_stage: int = Field(default=0, ge=0, le=4)


class BaseState(StrictModel):
    wheel_test_active: bool = False
    encoder_counts: list[int] | None = Field(default=None, min_length=4, max_length=4)
    x_m: float | None
    y_m: float | None
    yaw_rad: float | None


class State(Envelope):
    type: Literal["state"]
    accepted: bool
    enabled: bool
    ready: bool
    fault: str | None = Field(max_length=160)
    arm: ArmState | None = None
    base: BaseState | None = None


def encode_frame(data):
    raw = json.dumps(data, separators=(",", ":"), allow_nan=False).encode("ascii") + b"\n"
    if len(raw) > MAX_FRAME:
        raise LinkError("frame_too_large")
    return raw


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise LinkError("duplicate_field")
        result[key] = value
    return result


def decode_frame(raw):
    if len(raw) > MAX_FRAME or not raw.endswith(b"\n"):
        raise LinkError("invalid_frame_length")
    try:
        data = json.loads(raw.decode("ascii"), object_pairs_hook=_unique_object,
                          parse_constant=lambda value: (_ for _ in ()).throw(LinkError("nonfinite")))
    except (ValueError, UnicodeError) as exc:
        raise LinkError("invalid_json") from exc
    if not isinstance(data, dict):
        raise LinkError("expected_object")
    return data


def role_command(role, command):
    if role == "base":
        return {key: command[key] for key in ("vx_mps", "vy_mps", "wz_radps")}
    return {"zl_mps": command["z_mps"] or command["zl_mps"],
            "zr_mps": command["z_mps"] or command["zr_mps"],
            "x_mps": command["x_mps"], "gripper": command["gripper"]}


def role_acceleration(role, acceleration):
    keys = ("base_mps2", "rotation_radps2") if role == "base" else ("z_mps2", "x_mps2", "gripper_per_s")
    return {key: getattr(acceleration, key) for key in keys}


class SerialPeer:
    """One in-flight exchange, bounded framing and absolute response deadline."""

    def __init__(self, port, role, device_id, session, clock=time.monotonic):
        self.port = port
        self.role = role
        self.device_id = device_id
        self.session = session
        self.clock = clock
        self.seq = 0
        self.identity = None

    def request(self, kind, **payload):
        self.seq += 1
        return {"type": kind, "protocol": 1, "session": self.session,
                "seq": self.seq, **payload}

    def write(self, data):
        raw = encode_frame(data)
        if self.port.write(raw) != len(raw):
            raise LinkError("short_write")

    def exchange(self, data, model):
        deadline = self.clock() + REPLY_TIMEOUT
        self.write(data)
        raw = bytearray()
        while self.clock() < deadline:
            byte = self.port.read(1)
            if not byte:
                continue
            raw.extend(byte)
            if len(raw) > MAX_FRAME:
                raise LinkError("frame_too_large")
            if byte == b"\n":
                reply = model.model_validate(decode_frame(bytes(raw)))
                if (reply.session != self.session or reply.seq != data["seq"]
                        or (self.role is not None and reply.role != self.role)
                        or (self.device_id is not None and reply.device_id != self.device_id)):
                    raise LinkError("identity_or_sequence_mismatch")
                if self.identity and reply.boot_id != self.identity.boot_id:
                    raise LinkError("controller_rebooted")
                return reply
        raise LinkError("reply_timeout")

    def hello(self):
        self.identity = self.exchange(self.request("hello"), Identity)
        self.role = self.identity.role
        self.device_id = self.identity.device_id
        if self.identity.enabled:
            raise LinkError("identity_reports_enabled")
        needed = CAPABILITIES | ({"z_difference_limit"} if self.role == "arm" else set())
        if not needed.issubset(self.identity.capabilities):
            raise LinkError("missing_capability")
        return self.identity

    def setpoint(self, enabled, command, acceleration=None, *, service=False):
        if self.role is None or self.identity is None:
            raise LinkError("identity_required")
        if service and (self.role != "arm" or "service_jog_v1" not in self.identity.capabilities):
            raise LinkError("service_not_supported")
        data = self.request("service_setpoint" if service else "setpoint", enabled=enabled,
                            command=role_command(self.role, command if enabled else ZERO),
                            acceleration=role_acceleration(self.role, acceleration or Acceleration()))
        reply = self.exchange(data, State)
        if not reply.accepted or (enabled and reply.fault) or reply.enabled != enabled:
            raise LinkError(reply.fault or "controller_rejected_or_faulted")
        if (self.role == "arm") != (reply.arm is not None) or (self.role == "base") != (reply.base is not None):
            raise LinkError("wrong_telemetry_role")
        if service and enabled and (not reply.arm.service_active or reply.arm.homed):
            raise LinkError("service_state_mismatch")
        if enabled and not service and not reply.ready:
            raise LinkError("controller_not_ready")
        return reply

    def wheel_setpoint(self, wheel, pwm):
        if self.role != "base" or self.identity is None or "base_wheel_test_v1" not in self.identity.capabilities:
            raise LinkError("base_wheel_test_not_supported")
        reply = self.exchange(self.request("wheel_setpoint", enabled=True,
            command={"wheel": wheel, "pwm": pwm},
            acceleration=role_acceleration("base", Acceleration())), State)
        if not reply.accepted or reply.fault or not reply.enabled:
            raise LinkError(reply.fault or "wheel_test_rejected")
        if reply.arm is not None or reply.base is None or not reply.base.wheel_test_active:
            raise LinkError("wheel_test_state_mismatch")
        return reply

    def clear_base_fault(self):
        if self.role != "base" or self.identity is None or "base_clear_fault_v1" not in self.identity.capabilities:
            raise LinkError("base_clear_fault_requires_updated_firmware")
        reply = self.exchange(self.request("clear_fault"), State)
        if not reply.accepted or reply.enabled or reply.fault or reply.base is None or reply.arm is not None:
            raise LinkError(reply.fault or "base_clear_fault_rejected")
        return reply

    def arm_action(self, action, **payload):
        if self.role != "arm" or self.identity is None or "arm_actions_v1" not in self.identity.capabilities:
            raise LinkError("arm_actions_not_supported")
        reply = self.exchange(self.request("arm_action", action=action, **payload), State)
        if not reply.accepted or reply.arm is None or reply.base is not None:
            raise LinkError(reply.fault or "arm_action_rejected")
        if action not in {"status", "stop", "clear_fault"} and reply.fault:
            raise LinkError(reply.fault)
        return reply

    def stop_best_effort(self):
        # No wait for ACK: try the other controller even if this link has failed.
        try:
            if self.role is None:
                return
            self.write(self.request("setpoint", enabled=False, command=role_command(self.role, ZERO),
                                    acceleration=role_acceleration(self.role, Acceleration())))
        except (OSError, ValueError):
            pass

    def close(self):
        self.port.close()
