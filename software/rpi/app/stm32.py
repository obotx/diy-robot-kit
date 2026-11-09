"""Two-controller serial gateway. All blocking I/O is owned by one worker."""

from __future__ import annotations

import copy
import logging
import os
import threading
import time
from uuid import uuid4

from .robot import Acceleration, ControlError, Enable, Home, HomeKeepalive, Motion, RobotController, WATCHDOG_SECONDS, ZERO, ArmMove, ArmConfigure, ClearFault
from .stm32_protocol import LinkError, SerialPeer, role_command
from .stm32_discovery import candidates, discover
from .robot import ServiceUnlock, ServiceJog, ServiceLock, ReferenceStep, BaseWheelJog
import math

LOG = logging.getLogger(__name__)
STATE_MAX_AGE = 0.35


def drain_serial_input(port, clock=time.monotonic):
    """Discard old-session bytes only before HELLO, including delayed USB data."""
    deadline = clock() + 0.5
    quiet_since = clock()
    discarded = 0
    while clock() < deadline:
        data = port.read(1)
        now = clock()
        if data:
            discarded += len(data)
            quiet_since = now
            if discarded > 8192:
                raise LinkError("serial_startup_input_overflow")
        elif now - quiet_since >= 0.05:
            return discarded
    raise LinkError("serial_startup_input_not_quiet")


def open_port(board, config, backend):
    import serial

    options = dict(baudrate=config.baudrate, timeout=0.01, write_timeout=0.05)
    if backend == "stm32-emulator":
        return serial.serial_for_url(board.port, **options)
    if os.name == "posix":
        options["exclusive"] = True
    port = serial.Serial(board.port, **options)
    try:
        # A reset_input_buffer alone misses bytes still in the ST-Link USB pipeline.
        discarded = drain_serial_input(port)
        if discarded:
            LOG.info("Discarded %s old-session bytes from %s before HELLO", discarded, board.port)
        return port
    except Exception:
        port.close()
        raise


class SerialGateway:
    def __init__(self, config, backend="stm32", opener=open_port, clock=time.monotonic, enumerator=candidates):
        config.check_ports(backend)
        self.config, self.backend = config, backend
        self.opener, self.clock = opener, clock
        self.enumerator = enumerator
        self.assignments = {}
        self.pinned_ids = None
        self.last_scan = 0.0
        self.lock = threading.Lock()
        self.wake = threading.Event()
        self.closing = threading.Event()
        self.peers = {}
        self.epoch = 0
        self.intent = (False, ZERO.copy(), 0.0, Acceleration())
        self.action = None
        self.action_sent = None
        self.action_result = None
        self.states = {}
        self.identities = {}
        self.updated = 0.0
        self.connected = False
        self.error = "connecting"
        self.last_failure = None
        self.thread = None

    def start(self):
        self.thread = threading.Thread(target=self._run, name="stm32-serial", daemon=True)
        self.thread.start()

    def status(self):
        with self.lock:
            fresh = self.connected and self.clock() - self.updated < STATE_MAX_AGE
            return {"connected": fresh, "epoch": self.epoch,
                    "error": self.error if not self.connected else (None if fresh else "telemetry_timeout"),
                    "identities": copy.deepcopy(self.identities),
                    "discovery": self.config.discovery,
                    "arm_only": self.config.arm_only,
                    "ports": {r: b.port for r, b in self.assignments.items()},
                    "action_result": self.action_result,
                    "last_failure": copy.deepcopy(self.last_failure),
                    "states": copy.deepcopy(self.states) if fresh else {}}

    def submit(self, epoch, enabled, command, deadline, acceleration=None, *, service=False):
        with self.lock:
            if enabled and (epoch != self.epoch or not self.connected):
                raise ControlError("hardware_disconnected")
            self.intent = (enabled, command.copy() if enabled else ZERO.copy(), deadline,
                           acceleration or Acceleration())
            if service and enabled:
                self.intent[1]["_service"] = True
            self.action = None
        self.wake.set()

    def submit_action(self, epoch, token, action, payload, deadline):
        with self.lock:
            if epoch != self.epoch or not self.connected:
                raise ControlError("hardware_disconnected")
            if self.action and self.action[0] != token:
                raise ControlError("busy")
            self.intent = (False, ZERO.copy(), 0.0, Acceleration())
            self.action = (token, action, payload.copy(), deadline)
            self.action_result = None
        self.wake.set()

    def renew_action(self, token, deadline):
        with self.lock:
            if self.action is None and self.action_result == (token, "complete"):
                return False
            if not self.action or self.action[0] != token or self.clock() >= self.action[3]:
                raise ControlError("action_expired")
            self.action = (*self.action[:3], deadline)
            return True

    def _desired(self):
        with self.lock:
            enabled, command, deadline, acceleration = self.intent
        if enabled and self.clock() >= deadline:
            raise LinkError("operator_lease_expired")
        return enabled, command, acceleration

    def _connect(self):
        boards = discover(self.config, self.opener, self.clock, self.closing, self.enumerator) if self.config.discovery else {
            role: getattr(self.config, role) for role in (("arm",) if self.config.arm_only else ("arm", "base"))}
        ids = {r: b.device_id for r, b in boards.items()}
        if self.pinned_ids is not None and ids != self.pinned_ids:
            raise LinkError("discovery_identity_changed_restart_required")
        session = uuid4().hex
        for role in boards:
            board = boards[role]
            peer = SerialPeer(self.opener(board, self.config, self.backend), role,
                              board.device_id, session, self.clock)
            self.peers[role] = peer
            identity = peer.hello()
            if role == "arm" and self.config.max_z_difference_mm is not None:
                if identity.max_z_difference_mm != self.config.max_z_difference_mm:
                    raise LinkError("z_limit_configuration_mismatch")
        # Connection recovery always starts with two acknowledged disabled states.
        self._cycle()
        with self.lock:
            self.assignments = boards
            self.pinned_ids = ids
            self.last_scan = self.clock()

    def _cycle(self):
        states = {}
        for role, peer in self.peers.items():
            with self.lock:
                action = self.action
            if action:
                token, verb, payload, deadline = action
                if self.clock() >= deadline:
                    raise LinkError("operator_lease_expired")
                if verb == "base_clear_fault":
                    with self.lock:
                        if not self.action or self.action[0] != token:
                            continue
                        if role == "base":
                            reply = peer.clear_base_fault()
                            self.states[role] = reply.model_dump()
                            self.action_result = (token, "complete")
                            self.action = None
                        else:
                            reply = peer.setpoint(False, ZERO)
                    states[role] = reply.model_dump()
                    continue
                if role == "arm":
                    # Serialize first action with cancellation, so a queued STOP
                    # cannot be overtaken by an old HOME/MOVE after lock release.
                    with self.lock:
                        if not self.action or self.action[0] != token:
                            continue
                        if self.clock() >= self.action[3]:
                            raise LinkError("operator_lease_expired")
                        first = self.action_sent != token
                        if first:
                            reply = peer.arm_action(verb, **payload)
                            self.action_sent = token
                    # Periodic replies must not hold the lock used by the web
                    # event loop (status, lease renewal and STOP). Keepalive
                    # cannot start an action, unlike the serialized first send.
                    if not first:
                        reply = peer.arm_action("keepalive")
                    with self.lock:
                        cancelled = not self.action or self.action[0] != token
                        if not cancelled and self.clock() >= self.action[3]:
                            raise LinkError("operator_lease_expired")
                        if not cancelled and reply.arm.active_axis is None:
                            reference_stage = {"level_down": 1, "level_up": 1, "level_center": 1, "home_z": 2, "lift_z": 3, "home_x": 4}.get(verb)
                            if reference_stage is not None and reply.arm.reference_stage != reference_stage:
                                raise LinkError("reference_ended_without_completion")
                            if verb == "home":
                                axes = {"Z": ("ZL", "ZR"), "ZL": ("ZL",), "X": ("X",)}[payload["axis"]]
                                if not all(reply.arm.homed_axes.get(axis, False) for axis in axes):
                                    raise LinkError("homing_ended_without_reference")
                            # Publish completion and its confirming state atomically.
                            self.states[role] = reply.model_dump()
                            self.action_result = (token, "complete")
                            self.action = None
                    if cancelled:
                        # One worker owns the wire: finish the in-flight reply,
                        # then disable before processing any subsequent action.
                        reply = peer.setpoint(False, ZERO)
                    states[role] = reply.model_dump()
                    continue
                states[role] = peer.setpoint(False, ZERO).model_dump()
                continue
            enabled, command, acceleration = self._desired()
            if "_wheel_test" in command:
                # Serialize the test send with STOP; arm always receives disabled state.
                with self.lock:
                    current = self.intent
                    if not current[0] or current[1].get("_wheel_test") != command["_wheel_test"]:
                        continue
                    if self.clock() >= current[2]:
                        raise LinkError("operator_lease_expired")
                    wheel, pwm = current[1]["_wheel_test"]
                    reply = peer.wheel_setpoint(wheel, pwm) if role == "base" else peer.setpoint(False, ZERO)
                states[role] = reply.model_dump()
                continue
            service = command.get("_service", False)
            if service and role == "arm":
                # Entry must be zero-speed and serialized against cancellation.
                with self.lock:
                    if not self.intent[0] or not self.intent[1].get("_service"):
                        continue
                    entering = not self.states.get("arm", {}).get("arm", {}).get("service_active", False)
                    if entering:
                        reply = peer.setpoint(True, ZERO, acceleration, service=True)
                if not entering:
                    reply = peer.setpoint(enabled, command, acceleration, service=True)
                states[role] = reply.model_dump()
            else:
                states[role] = peer.setpoint(enabled, command, acceleration).model_dump()
        with self.lock:
            self.states = states
            self.identities = {role: peer.identity.model_dump() for role, peer in self.peers.items()}
            self.updated = self.clock()
            self.connected = True
            self.error = None

    def _fail(self, reason):
        # Invalidate the control lease before touching either potentially broken link.
        with self.lock:
            # Reconnect retries must not overwrite the original motion failure.
            if reason != "shutdown" and (self.connected or self.last_failure is None):
                self.last_failure = {"reason": reason, "time_unix": time.time(),
                                     "action": self.action[1] if self.action else None}
                LOG.warning("STM32 stopped: %s (action=%s)", reason, self.last_failure["action"])
            self.epoch += 1
            self.intent = (False, ZERO.copy(), 0.0, Acceleration())
            self.action = None
            self.action_sent = None
            self.action_result = None
            self.connected = False
            self.states = {}
            self.error = reason
        for peer in self.peers.values():
            peer.stop_best_effort()
        for peer in self.peers.values():
            try:
                peer.close()
            except OSError:
                pass
        self.peers.clear()

    def _run(self):
        try:
            while not self.closing.is_set():
                try:
                    if not self.peers:
                        self._connect()
                    else:
                        if self.config.discovery and self.clock() - self.last_scan >= 1.0:
                            self.last_scan = self.clock()
                            if set(self.enumerator()) != {b.port for b in self.assignments.values()}:
                                raise LinkError("discovery_usb_topology_changed")
                        self._cycle()
                except Exception as exc:
                    reason = str(exc) if isinstance(exc, LinkError) else "serial_or_protocol_error"
                    LOG.warning("STM32 link disabled: %s (%s)", reason, exc)
                    self._fail(reason)
                    self.closing.wait(1.0)
                    continue
                self.wake.wait(0.08)
                self.wake.clear()
        finally:
            self._fail("shutdown")

    def close(self):
        with self.lock:
            self.intent = (False, ZERO.copy(), 0.0, Acceleration())
        self.closing.set()
        self.wake.set()
        if self.thread:
            self.thread.join(timeout=4.0)
            if self.thread.is_alive():
                LOG.error("Serial worker did not exit; controller watchdogs must stop outputs")


class STM32Robot(RobotController):
    def __init__(self, gateway, clock=time.monotonic):
        super().__init__(clock)
        self.gateway = gateway
        self.epoch = gateway.status()["epoch"]
        self.action_token = None
        self.action_axis = None
        self.action_kind = None
        self.completed_action_owner = None
        self.completed_action_at = 0.0
        self.wheel_test_started = None
        self.wheel_test_wheel = None
        self.wheel_test_armed = False
        self.service_until = 0.0
        self.service_client = None
        self.service_burst_at = None
        self.service_release_required = False

    def disconnect(self, client_id):
        if self.service_client == client_id:
            self.service_client = None
            self.wheel_test_armed = False
        super().disconnect(client_id)

    def stop(self, reason):
        self.wheel_test_started = None
        self.wheel_test_wheel = None
        self.wheel_test_armed = False
        self.service_until = 0.0
        self.service_burst_at = None
        self.service_release_required = False
        # Enable first sends a zero-velocity intent before the next browser
        # motion frame. Never reuse rejected acceleration in that intent.
        self.acceleration = Acceleration()
        self.completed_action_owner = self.owner if reason == "action_complete" else None
        self.completed_action_at = self.clock()
        self.action_token = self.action_axis = self.action_kind = None
        super().stop(reason)
        self.gateway.submit(self.epoch, False, ZERO, 0.0)

    def tick(self):
        dt = super().tick()
        status = self.gateway.status()
        if self.owner and (not status["connected"] or status["epoch"] != self.epoch):
            self.stop("hardware_fault")
        elif self.wheel_test_started is not None and self.clock() - self.wheel_test_started >= 2.0:
            self.stop("wheel_test_complete")
        elif self.action_token and status.get("action_result") == (self.action_token, "complete"):
            self.stop("action_complete")
        return dt

    def _ready(self, status):
        if not status["connected"]:
            return "hardware_disconnected"
        if not self.gateway.config.commissioned:
            return "commissioning_required"
        states = status["states"]
        expected = {"arm"} if self.gateway.config.arm_only else {"arm", "base"}
        if set(states) != expected or any(s["fault"] for s in states.values()):
            return "controller_not_ready"
        arm = states["arm"]["arm"]
        single = "zl_only" in status["identities"]["arm"]["capabilities"]
        if not arm["homed"] or any(arm[k] is None for k in (("z_left_mm",) if single else ("z_left_mm", "z_right_mm", "x_mm"))):
            return "homing_required"
        if not all(s["ready"] for s in states.values()):
            return "controller_not_ready"
        limit = self.gateway.config.max_z_difference_mm
        if limit is None or (not single and abs(arm["z_left_mm"] - arm["z_right_mm"]) > limit):
            return "z_difference_limit"
        return None

    def apply(self, client_id, message):
        self.tick()
        status = self.gateway.status()
        if isinstance(message, ServiceLock):
            if self.service_client not in (None, client_id):
                raise ControlError("not_service_owner")
            self.service_client = None
            self.stop("service_locked")
            return
        if isinstance(message, ServiceUnlock):
            caps = status["identities"].get("arm", {}).get("capabilities", [])
            if self.owner is not None:
                raise ControlError("release_before_service")
            if self.service_client not in (None, client_id):
                raise ControlError("service_owned_by_another_client")
            base_test = "base_wheel_test_v1" in status["identities"].get("base", {}).get("capabilities", [])
            if not status["connected"] or (not base_test and not (
                    self.gateway.config.commissioned or getattr(self.gateway.config, "service_only", False))):
                raise ControlError("service_configuration_required")
            if not base_test and ("service_jog_v2" not in caps or "x_microstep_128" not in caps or "zl_only" in caps):
                raise ControlError("service_requires_new_full_arm_firmware")
            self.wheel_test_armed = False
            self.service_client = client_id
            self.reason = "service_selected"
            return
        if isinstance(message, BaseWheelJog):
            if self.service_client != client_id:
                raise ControlError("not_service_owner")
            if self.owner not in (None, client_id) or self.owner and self.source != "wheel_test":
                raise ControlError("release_before_wheel_test")
            caps = status["identities"].get("base", {}).get("capabilities", [])
            if not status["connected"] or "base_wheel_test_v1" not in caps or self.gateway.config.arm_only:
                raise ControlError("base_wheel_test_not_supported")
            if status.get("states", {}).get("base", {}).get("fault"):
                raise ControlError("base_fault_reset_required")
            if message.pwm == 0:
                self.stop("wheel_test_armed")
                self.wheel_test_armed = True
                self.wheel_test_wheel = message.wheel
                return
            if not self.wheel_test_armed or self.wheel_test_wheel != message.wheel:
                raise ControlError("wheel_test_release_required")
            if self.wheel_test_started is None:
                self.wheel_test_started = self.clock()
            self.owner, self.source, self.reason = client_id, "wheel_test", "wheel_test"
            self.epoch = status["epoch"]
            self.last_command = self.clock()
            self.gateway.submit(self.epoch, True, ZERO | {"_wheel_test": (message.wheel, message.pwm)},
                                min(self.last_command + WATCHDOG_SECONDS, self.wheel_test_started + 2.0))
            return
        if isinstance(message, Enable) and message.enabled and self.service_client == client_id:
            if not (self.gateway.config.commissioned or getattr(self.gateway.config, "service_only", False)):
                raise ControlError("service_configuration_required")
            caps = status["identities"].get("arm", {}).get("capabilities", [])
            if "service_jog_v2" not in caps or "x_microstep_128" not in caps:
                raise ControlError("service_requires_new_full_arm_firmware")
            if self.owner is not None:
                raise ControlError("release_before_service")
            if not status["connected"] or any(s.get("fault") for s in status.get("states", {}).values()):
                raise ControlError("clear_fault_before_service_motion")
            self.owner, self.source, self.reason = client_id, "service", "service"
            self.epoch = status["epoch"]
            self.last_command = self.clock()
            self.service_until = True
            self.service_burst_at = None
            self.service_release_required = False
            self.command = ZERO.copy()
            self.gateway.submit(self.epoch, True, ZERO, self.last_command + WATCHDOG_SECONDS,
                                Acceleration(z_mps2=0.05, x_mps2=0.05), service=True)
            return
        if isinstance(message, ServiceJog):
            if self.source == "wheel_test":
                raise ControlError("release_before_service")
            if not self.service_until or self.owner != client_id:
                raise ControlError("not_service_owner")
            if math.hypot(message.vx_mps, message.vy_mps) > 0.2 + 1e-9:
                raise ControlError("base_speed_limit")
            if self.gateway.config.arm_only and (message.vx_mps or message.vy_mps or message.wz_radps):
                raise ControlError("base_not_connected")
            if message.gripper and "service_gripper_v1" not in status["identities"].get("arm", {}).get("capabilities", []):
                raise ControlError("service_gripper_requires_updated_arm")
            command = ZERO | message.model_dump(exclude={"type", "seq", "acceleration"})
            self.command = command
            self.acceleration = message.acceleration
            self.last_command = self.clock()
            self.gateway.submit(self.epoch, True, command, self.last_command + WATCHDOG_SECONDS,
                                message.acceleration, service=True)
            return
        if self.service_client is not None and isinstance(message, (Enable, Motion, Home, ReferenceStep, ArmMove, ArmConfigure)) and not (
                isinstance(message, Enable) and not message.enabled):
            raise ControlError("exit_service_first")
        if self.service_until and not (message.type == "stop" or
                                      isinstance(message, Enable) and not message.enabled):
            raise ControlError("exit_service_first")
        if isinstance(message, HomeKeepalive):
            # One in-flight keepalive may arrive before completion telemetry reaches
            # its owner. Acknowledge it without renewing any lease or sending I/O.
            if (self.owner is None and self.completed_action_owner == client_id
                    and status["connected"] and status["epoch"] == self.epoch
                    and self.clock() - self.completed_action_at < WATCHDOG_SECONDS):
                return
            if self.owner != client_id or not self.action_token:
                raise ControlError("not_action_owner")
            now = self.clock()
            renewed = self.gateway.renew_action(self.action_token, now + WATCHDOG_SECONDS)
            if renewed is False:
                self.tick()
            else:
                self.last_command = now
            return
        if isinstance(message, (Home, ReferenceStep, ArmMove, ArmConfigure, ClearFault)):
            if self.owner is not None:
                raise ControlError("release_before_action")
            if isinstance(message, ClearFault) and message.controller == "base":
                if not status["connected"]:
                    raise ControlError("hardware_disconnected")
                if "base_clear_fault_v1" not in status["identities"].get("base", {}).get("capabilities", []):
                    raise ControlError("base_clear_fault_requires_updated_firmware")
                if any(s.get("enabled") for s in status["states"].values()):
                    raise ControlError("release_before_action")
                self.epoch = status["epoch"]
                token = f"{client_id}:{message.seq}"
                self.gateway.submit_action(self.epoch, token, "base_clear_fault", {}, self.clock()+WATCHDOG_SECONDS)
                self.completed_action_owner = None
                self.action_token, self.action_axis, self.action_kind = token, None, "base_clear_fault"
                self.owner, self.source = client_id, "arm_action"
                self.last_command = self.clock()
                self.reason = "base_clear_fault"
                return
            identity = status["identities"].get("arm", {})
            caps = identity.get("capabilities", [])
            service_clear = isinstance(message, ClearFault) and getattr(self.gateway.config, "service_only", False)
            if not status["connected"] or not (self.gateway.config.commissioned or service_clear):
                raise ControlError("commissioning_required")
            if "arm_actions_v1" not in caps:
                raise ControlError("homing_not_supported_by_json_firmware")
            if "zl_only" in caps and getattr(message, "axis", "ZL") != "ZL":
                raise ControlError("inactive_axis")
            if isinstance(message, Home) and message.axis == "ZL" and "zl_only" not in caps:
                raise ControlError("use_paired_z_homing")
            self.epoch = status["epoch"]
            verb = "home" if isinstance(message, Home) else "move" if isinstance(message, ArmMove) else "configure" if isinstance(message, ArmConfigure) else "clear_fault"
            payload = message.model_dump(exclude={"type", "seq", "controller"}, exclude_none=True)
            if isinstance(message, ReferenceStep):
                if "reference_sequence_v1" not in caps:
                    raise ControlError("reference_requires_updated_arm")
                verb = message.step
                payload = {}
            token = f"{client_id}:{message.seq}"
            self.gateway.submit_action(self.epoch, token, verb, payload, self.clock()+WATCHDOG_SECONDS)
            self.completed_action_owner = None
            self.action_token, self.action_axis, self.action_kind = token, payload.get("axis"), verb
            self.owner, self.source = client_id, "arm_action"
            self.last_command = self.clock()
            self.reason = verb
            return
        if self.action_token and isinstance(message, (Motion, Enable)) and not (isinstance(message, Enable) and not message.enabled):
            raise ControlError("action_active")
        if isinstance(message, Enable) and message.enabled:
            error = self._ready(status)
            if error:
                raise ControlError(error)
            self.epoch = status["epoch"]
        if isinstance(message, Motion) and self.owner == client_id:
            error = self._ready(status)
            if error:
                self.stop("hardware_fault")
                raise ControlError(error)
            if message.gripper and "gripper_velocity" not in status["identities"]["arm"]["capabilities"]:
                self.stop("unsupported_command")
                raise ControlError("gripper_not_supported")
            arm = status["states"]["arm"]["arm"]
            velocities = role_command("arm", message.model_dump())
            single = "zl_only" in status["identities"]["arm"]["capabilities"]
            if (self.gateway.config.arm_only and (message.vx_mps or message.vy_mps or message.wz_radps)) or (single and (velocities["zr_mps"] or velocities["x_mps"])):
                self.stop("inactive_axis")
                raise ControlError("inactive_axis")
            if status["identities"]["arm"].get("firmware") == "arm-link-1" and (
                abs(velocities["zl_mps"]) > 0.05 or abs(velocities["zr_mps"]) > 0.05
                or abs(velocities["x_mps"]) > 0.1
            ):
                self.stop("firmware_speed_limit")
                raise ControlError("firmware_speed_limit")
            difference = 0 if single else arm["z_left_mm"] - arm["z_right_mm"]
            # Conservative gateway look-ahead, not a replacement for MCU braking limits.
            predicted = difference + (velocities["zl_mps"] - velocities["zr_mps"]) * 1000 * 0.4
            soft_skew = "z_skew_soft_limit_v1" in status["identities"]["arm"].get("capabilities", [])
            if not single and not soft_skew and abs(predicted) > self.gateway.config.max_z_difference_mm:
                self.stop("z_difference_limit")
                raise ControlError("z_difference_limit")
        super().apply(client_id, message)
        try:
            self.gateway.submit(self.epoch, self.owner is not None, self.command,
                                self.last_command + WATCHDOG_SECONDS, self.acceleration)
        except ControlError:
            self.stop("hardware_fault")
            raise

    def snapshot(self):
        self.tick()
        status = self.gateway.status()
        arm_state = status["states"].get("arm", {}).get("arm") or {}
        base_state = status["states"].get("base", {}).get("base") or {}
        ready_error = self._ready(status)
        caps = status["identities"].get("arm", {}).get("capabilities", [])
        actions = "arm_actions_v1" in caps
        supported = (["ZL"] if "zl_only" in caps else ["Z", "X"]) if actions else []
        action_blocked = ("hardware_disconnected" if not status["connected"] else
                          "homing_not_supported_by_json_firmware" if not actions else
                          "commissioning_required" if not self.gateway.config.commissioned else None)
        return {**self.control_snapshot(), "backend": self.gateway.backend,
                "service": {"supported": ("service_jog_v2" in caps and "x_microstep_128" in caps) or
                                "base_wheel_test_v1" in status["identities"].get("base", {}).get("capabilities", []),
                            "wheel_test_supported": "base_wheel_test_v1" in status["identities"].get("base", {}).get("capabilities", []),
                            "active": self.service_client is not None,
                            "client_id": self.service_client,
                            "running": bool(self.service_until),
                            "remaining_seconds": None,
                            "release_required": self.service_release_required or arm_state.get("service_release_required", False)},
                "hardware_connected": status["connected"] and self.gateway.backend == "stm32",
                "control_ready": ready_error is None, "control_blocked_reason": ready_error,
                "outputs_enabled": any(s["enabled"] for s in status["states"].values()),
                "controllers": status,
                "homing": {"supported_axes": supported,
                           "sequence_supported": "reference_sequence_v1" in caps,
                           "stage": arm_state.get("reference_stage", 0),
                           "level_active": arm_state.get("level_active", False),
                           "active_axis": self.action_axis if self.action_kind == "home" else None,
                           "blocked_reason": action_blocked},
                "arm_action": {"supported": actions, "active_axis": self.action_axis,
                               "kind": self.action_kind, "blocked_reason": action_blocked},
                "arm": {**{key: arm_state.get(key) for key in ("z_left_mm", "z_right_mm", "x_mm")},
                        "homed": arm_state.get("homed", False), "homed_axes": arm_state.get("homed_axes", {}), "travel_mm": 1000},
                "gripper": {"opening_pct": arm_state.get("gripper_pct")},
                "base": {key: base_state.get(key) for key in ("x_m", "y_m", "yaw_rad")}}
