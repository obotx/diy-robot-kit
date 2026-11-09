"""Explicit standalone ASCII ZL adapter. Never used on the paired JSON link."""
from __future__ import annotations

import threading
import time
import os

import serial
from pydantic import Field, field_validator

from .robot import RobotController, Home, HomeKeepalive, Stop, Enable, ControlError, WATCHDOG_SECONDS
from .stm32_protocol import StrictModel


class BenchConfig(StrictModel):
    port: str = Field(min_length=1, max_length=256)
    standalone_confirmed: bool = False

    @field_validator("port")
    @classmethod
    def local_only(cls, value):
        if "://" in value:
            raise ValueError("Bench requires a local serial port")
        return value


def parse_reply(line, prefix):
    parts = line.split()
    if not parts or parts[0] != prefix:
        raise ValueError("Unexpected ASCII response")
    result = {}
    for part in parts[1:]:
        key, value = part.split("=", 1)
        if key in result:
            raise ValueError("Duplicate ASCII field")
        result[key] = tuple(int(v) for v in value.split(",")) if "," in value else int(value)
    return result


def exchange(port, command, prefix="STAT"):
    port.write((command + "\n").encode("ascii"))
    deadline = time.monotonic() + 0.25
    line = bytearray()
    while time.monotonic() < deadline:
        char = port.read(1)
        if not char:
            continue
        line.extend(char)
        if len(line) > 1024:
            raise ValueError("Oversized ASCII response")
        if char == b"\n":
            value = line.decode("ascii").strip()
            line.clear()
            if value == "OK" or not value:
                continue
            return parse_reply(value, prefix)
    raise TimeoutError("Bench response timeout")


def profile_ok(io):
    return (io.get("bench") == 1 and io.get("standalone_zl") == 1
            and io.get("skew_um") == 100000)


def ready(io, state):
    return (profile_ok(io) and io.get("power") == 1
            and all(io.get(axis) == (0, 0, 0) for axis in ("zl", "zr", "x"))
            and state.get("en") == 0 and state.get("mode") == 0 and state.get("fault") == 0)


class BenchGateway:
    def __init__(self, config, opener=None):
        self.config = config
        self.opener = opener or (lambda: serial.Serial(config.port, 115200, timeout=0.01,
                                                      write_timeout=0.05, exclusive=os.name != "nt"))
        self.lock = threading.Lock()
        self.quit = threading.Event()
        self.intent = None
        self.deadline = 0.0
        self.data = {"connected": False, "error": None, "phase": "idle", "state": {}, "ready": False}
        self.updated = 0.0
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start()

    def close(self):
        self.cancel()
        self.quit.set()
        self.thread.join(timeout=3)

    def snapshot(self):
        with self.lock:
            data = dict(self.data)
            if time.monotonic() - self.updated > 0.65:
                data.update(connected=False, ready=False)
            return data

    def request(self, token):
        with self.lock:
            if not self.config.standalone_confirmed or not self.data["ready"] or self.intent:
                raise ControlError("bench_not_ready")
            if time.monotonic() - self.updated > 0.65:
                raise ControlError("bench_stale")
            self.intent = token
            self.deadline = time.monotonic() + WATCHDOG_SECONDS
            self.data = {**self.data, "phase": "pending", "ready": False}

    def renew(self, token):
        with self.lock:
            if self.intent == token and time.monotonic() < self.deadline:
                self.deadline = time.monotonic() + WATCHDOG_SECONDS

    def cancel(self):
        with self.lock:
            self.intent = None

    def run(self):
        port = None
        active = None
        started = 0.0
        try:
            port = self.opener()
            io = exchange(port, "INPUTS", "IO")
            if not profile_ok(io):
                raise ValueError("Wrong firmware: standalone ZL ASCII profile required")
            exchange(port, "STOP")
            exchange(port, "EN 0")
            while not self.quit.is_set():
                with self.lock:
                    token = self.intent if time.monotonic() < self.deadline else None
                if active and token != active:
                    exchange(port, "STOP")
                    exchange(port, "EN 0")
                    active = None
                phase = "idle"
                if token and not active:
                    io = exchange(port, "INPUTS", "IO")
                    state = exchange(port, "STATUS")
                    if not ready(io, state):
                        raise ValueError("Homing preconditions failed")
                    # Linearize HOME with cancellation; STOP follows a command already in flight.
                    with self.lock:
                        if self.intent != token or time.monotonic() >= self.deadline:
                            continue
                        state = exchange(port, "HOME ZL")
                        active = token
                        started = time.monotonic()
                elif active:
                    if time.monotonic() - started > 120:
                        raise TimeoutError("Homing timeout")
                    state = exchange(port, "HB")
                else:
                    io = exchange(port, "INPUTS", "IO")
                    state = exchange(port, "STATUS")
                    if state.get("en") != 0:
                        raise ValueError("Unexpected enabled bench")
                if active:
                    if state.get("fault") != 0:
                        raise ValueError("Homing firmware fault")
                    phase = "homing"
                    if state.get("en") == 0 and state.get("mode") == 0:
                        if state.get("h_zl") != 1 or state.get("zl") != 0:
                            raise ValueError("Homing stopped without confirmed reference")
                        phase = "complete"
                        with self.lock:
                            if self.intent == active:
                                self.intent = None
                        active = None
                    elif state.get("en") != 1 or state.get("mode") != 3:
                        raise ValueError("Unexpected mode during homing")
                with self.lock:
                    if self.intent and not active and self.intent != token:
                        phase = "pending"
                    elif not self.intent and phase == "idle" and self.data["phase"] == "complete":
                        phase = "complete"
                    self.data = dict(connected=True, error=None, phase=phase, state=state,
                                     ready=not active and not self.intent and ready(io, state))
                    self.updated = time.monotonic()
                self.quit.wait(0.05)
        except Exception as exc:
            with self.lock:
                self.data = dict(connected=False, error=str(exc), phase="fault", state={}, ready=False)
        finally:
            if port:
                for command in ("STOP", "EN 0"):
                    try:
                        exchange(port, command)
                    except Exception:
                        pass
                port.close()


class ArmBenchRobot(RobotController):
    def __init__(self, gateway):
        super().__init__()
        self.gateway = gateway

    def stop(self, reason):
        self.gateway.cancel()
        super().stop(reason)

    def apply(self, client_id, message):
        self.tick()
        if isinstance(message, Stop) or (isinstance(message, Enable) and not message.enabled):
            return super().apply(client_id, message)
        if isinstance(message, Home):
            if message.axis != "ZL":
                raise ControlError("bench_home_zl_only")
            if self.owner is not None:
                raise ControlError("release_before_homing")
            self.gateway.request(client_id)
            self.owner = client_id
            self.source = "homing"
            self.last_command = self.clock()
            self.reason = "homing"
        elif isinstance(message, HomeKeepalive) and self.owner == client_id:
            self.last_command = self.clock()
            self.gateway.renew(client_id)
        else:
            raise ControlError("bench_home_zl_only")

    def tick(self):
        dt = super().tick()
        data = self.gateway.snapshot()
        if self.owner and (not data["connected"] or data["phase"] in ("idle", "complete", "fault")):
            self.stop("homing_complete" if data["phase"] == "complete" else "bench_stopped")
        return dt

    def snapshot(self):
        self.tick()
        data = self.gateway.snapshot()
        state = data["state"] if data["connected"] else {}
        return {**self.control_snapshot(), "backend": "arm-bench", "control_ready": False,
                "control_blocked_reason": "bench_home_zl_only", "hardware_connected": data["connected"],
                "controllers": {"connected": data["connected"], "error": data["error"]},
                "base": dict(x_m=None, y_m=None, yaw_rad=None),
                "arm": dict(z_left_mm=state.get("zl", 0) / 1000 if "zl" in state else None,
                            z_right_mm=None, x_mm=None, homed=False, travel_mm=1000,
                            homed_axes={"ZL": state.get("h_zl") == 1}),
                "gripper": {"opening_pct": None},
                "homing": {"supported_axes": ["ZL"], "active_axis": "ZL" if self.owner else None,
                           "blocked_reason": None if data["ready"] and self.gateway.config.standalone_confirmed
                           else "bench_not_ready", "phase": data["phase"]}}
