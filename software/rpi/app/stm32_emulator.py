"""Local test peers for the serial contract. Never access GPIO or physical ports."""

from __future__ import annotations

import argparse
import math
import socket
import socketserver
import threading
import time
from uuid import uuid4

from .robot import Acceleration, MAX_LINEAR_MPS, Motion, RobotSimulator, ZERO
from .stm32_protocol import CAPABILITIES, DEVICE_WATCHDOG_MS, MAX_FRAME, LinkError, decode_frame, encode_frame, role_command, role_acceleration


class BoardEmulator:
    def __init__(self, role, clock=time.monotonic):
        self.role, self.clock = role, clock
        self.device_id = "EMULATOR-" + role.upper()
        self.boot_id = uuid4().hex
        self.session = None
        self.seq = -1
        self.last_setpoint = clock()
        self.robot = RobotSimulator(clock)
        self.fault = None
        self.limit = 100.0  # Test fixture only, never a real mechanical limit.

    def stop(self):
        self.robot.stop("emulator_stop")
        self.session = None

    def tick(self):
        if self.robot.owner and self.clock() - self.last_setpoint >= DEVICE_WATCHDOG_MS / 1000:
            self.fault = "local_watchdog"
            self.stop()
        self.robot.tick()
        if abs(self.robot.z_left_mm - self.robot.z_right_mm) > self.limit:
            self.fault = "z_difference_limit"
            self.stop()

    def handle(self, data):
        try:
            return self._handle(data)
        except (ValueError, KeyError, TypeError):
            self.stop()
            raise

    def _handle(self, data):
        self.tick()
        common = {"type", "protocol", "session", "seq"}
        if data.get("protocol") != 1 or type(data.get("protocol")) is not int:
            raise LinkError("protocol_mismatch")
        session, seq = data["session"], data["seq"]
        if (not isinstance(session, str) or len(session) != 32
                or any(c not in "0123456789abcdef" for c in session)
                or type(seq) is not int or not 0 <= seq <= 9007199254740991):
            raise LinkError("invalid_envelope")
        if data["type"] == "hello":
            if set(data) != common:
                raise LinkError("unexpected_fields")
            self.stop()
            self.fault = None
            self.session, self.seq = session, seq
            return {**self.envelope(), "type": "identity", "firmware": "python-emulator-1",
                    "capabilities": sorted(CAPABILITIES | ({"z_difference_limit", "gripper_velocity"} if self.role == "arm" else set())),
                    "watchdog_ms": DEVICE_WATCHDOG_MS, "enabled": False,
                    "max_z_difference_mm": self.limit if self.role == "arm" else None}
        if (data["type"] != "setpoint" or set(data) != common | {"enabled", "command", "acceleration"}
                or session != self.session or seq <= self.seq or type(data["enabled"]) is not bool):
            raise LinkError("invalid_setpoint_or_session")
        command = data["command"]
        if not isinstance(command, dict) or set(command) != set(role_command(self.role, ZERO)):
            raise LinkError("wrong_role_command")
        acceleration = data["acceleration"]
        if not isinstance(acceleration, dict) or set(acceleration) != set(role_acceleration(self.role, Acceleration())):
            raise LinkError("wrong_role_acceleration")
        motion = Motion.model_validate({"type": "motion", "seq": seq, **ZERO, **command,
                                       "acceleration": acceleration})
        if math.hypot(motion.vx_mps, motion.vy_mps) > MAX_LINEAR_MPS + 1e-9:
            raise LinkError("speed_limit")
        if not data["enabled"] and any(command.values()):
            raise LinkError("disabled_nonzero_command")
        self.seq = seq
        if not data["enabled"]:
            self.robot.stop("disabled")
        self.robot.command = motion.model_dump(exclude={"type", "seq", "acceleration"})
        self.robot.acceleration = motion.acceleration
        self.robot.owner = "gateway" if data["enabled"] else None
        self.robot.last_command = self.last_setpoint = self.clock()
        state = self.robot.snapshot()
        return {**self.envelope(), "type": "state", "accepted": True,
                "enabled": data["enabled"], "ready": True, "fault": self.fault,
                "arm": {"z_left_mm": state["arm"]["z_left_mm"], "z_right_mm": state["arm"]["z_right_mm"],
                        "x_mm": state["arm"]["x_mm"], "homed": True,
                        "gripper_pct": state["gripper"]["opening_pct"]} if self.role == "arm" else None,
                "base": state["base"] if self.role == "base" else None}

    def envelope(self):
        return {"protocol": 1, "session": self.session, "seq": self.seq, "role": self.role,
                "device_id": self.device_id, "boot_id": self.boot_id}


class EmulatorHandler(socketserver.BaseRequestHandler):
    def handle(self):
        if not self.server.owner.acquire(blocking=False):
            return
        board = self.server.board
        self.request.settimeout(0.02)
        buffer = bytearray()
        try:
            while not self.server.stopping.is_set():
                board.tick()
                try:
                    data = self.request.recv(512)
                except socket.timeout:
                    continue
                if not data:
                    return
                buffer.extend(data)
                while b"\n" in buffer:
                    line, _, remaining = buffer.partition(b"\n")
                    buffer = bytearray(remaining)
                    response = board.handle(decode_frame(bytes(line) + b"\n"))
                    self.request.sendall(encode_frame(response))
                if len(buffer) >= MAX_FRAME:
                    raise LinkError("frame_too_large")
        except (ValueError, OSError, KeyError, TypeError):
            pass
        finally:
            board.stop()
            self.server.owner.release()


class EmulatorServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, role, port):
        self.board = BoardEmulator(role)
        self.owner = threading.Lock()
        self.stopping = threading.Event()
        super().__init__(("127.0.0.1", port), EmulatorHandler)

    def server_close(self):
        self.stopping.set()
        super().server_close()


def main():
    parser = argparse.ArgumentParser(description="Two local STM32 protocol emulators; no hardware outputs")
    parser.add_argument("--arm-port", type=int, default=9101)
    parser.add_argument("--base-port", type=int, default=9102)
    args = parser.parse_args()
    with EmulatorServer("arm", args.arm_port) as arm, EmulatorServer("base", args.base_port) as base:
        servers = (arm, base)
        threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in servers]
        for thread in threads:
            thread.start()
        print(f"EMULATORS ONLY: arm 127.0.0.1:{args.arm_port}, base 127.0.0.1:{args.base_port}", flush=True)
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
        finally:
            for server in servers:
                server.stopping.set()
                server.shutdown()
            for thread in threads:
                thread.join()


if __name__ == "__main__":
    main()
