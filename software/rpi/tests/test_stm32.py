import json
import os
import threading
import time
import unittest
from unittest.mock import patch

from pydantic import ValidationError

from app.robot import Acceleration, ControlError, Enable, Motion, Stop, ZERO
from app.stm32 import SerialGateway, STM32Robot
from app.stm32_config import STM32Config, load_robot_settings
from app.stm32_emulator import BoardEmulator, EmulatorServer
from app.stm32_protocol import LinkError, SerialPeer, decode_frame, encode_frame, role_command


class Clock:
    now = 0.0

    def __call__(self):
        return self.now


def config(**changes):
    return STM32Config.model_validate({
        "arm": {"port": "COM-ARM", "device_id": "EMULATOR-ARM"},
        "base": {"port": "COM-BASE", "device_id": "EMULATOR-BASE"},
        "commissioned": True, "max_z_difference_mm": 100.0, **changes,
    })


def motion(**changes):
    return Motion(type="motion", seq=2, **(ZERO | changes))


class FakePort:
    def __init__(self, role, clock):
        self.board = BoardEmulator(role, clock)
        self.clock = clock
        self.input = bytearray()
        self.sent = []
        self.closed = False
        self.fail = False
        self.filter = lambda value: value

    def write(self, raw):
        if self.fail:
            raise OSError("unplugged")
        data = decode_frame(raw)
        self.sent.append(data)
        reply = self.filter(self.board.handle(data))
        self.input.extend(encode_frame(reply))
        return len(raw)

    def read(self, count):
        if self.input:
            result = bytes(self.input[:count])
            del self.input[:count]
            return result
        self.clock.now += 0.01
        return b""

    def close(self):
        self.closed = True


class ProtocolTests(unittest.TestCase):
    def test_independent_axes_share_one_command(self):
        self.peer.hello()
        self.peer.setpoint(True, ZERO | {"zl_mps": 0.02, "zr_mps": 0.03, "x_mps": -0.04})
        self.assertEqual(self.port.sent[-1]["command"],
                         {"zl_mps": 0.02, "zr_mps": 0.03, "x_mps": -0.04, "gripper": 0.0})

    def setUp(self):
        self.clock = Clock()
        self.port = FakePort("arm", self.clock)
        self.peer = SerialPeer(self.port, "arm", "EMULATOR-ARM", "a" * 32, self.clock)

    def test_handshake_and_complete_command_units(self):
        self.peer.hello()
        self.peer.setpoint(True, ZERO | {"z_mps": 0.02, "x_mps": -0.04, "gripper": 0.5})
        self.assertEqual(self.port.sent[-1]["command"], {"zl_mps": 0.02, "zr_mps": 0.02, "x_mps": -0.04, "gripper": 0.5})
        self.peer.setpoint(False, ZERO | {"x_mps": 0.1})
        self.assertFalse(self.port.board.robot.owner)
        self.assertFalse(any(self.port.sent[-1]["command"].values()))

    def test_mismatched_identity_session_sequence_protocol(self):
        for field, value in (("role", "base"), ("device_id", "OTHER"), ("session", "b" * 32), ("seq", 999), ("protocol", 2)):
            with self.subTest(field=field):
                self.setUp()
                self.port.filter = lambda reply: reply | {field: value}
                with self.assertRaises((LinkError, ValidationError)):
                    self.peer.hello()

    def test_missing_local_watchdog_capability_rejected(self):
        self.port.filter = lambda reply: reply | {"capabilities": []}
        with self.assertRaisesRegex(LinkError, "missing_capability"):
            self.peer.hello()

    def test_identity_must_confirm_disabled(self):
        self.port.filter = lambda reply: reply | {"enabled": True}
        with self.assertRaisesRegex(LinkError, "identity_reports_enabled"):
            self.peer.hello()

    def test_reboot_while_connected_rejected(self):
        self.peer.hello()
        self.port.board.boot_id = "new-boot"
        with self.assertRaisesRegex(LinkError, "controller_rebooted"):
            self.peer.setpoint(False, ZERO)

    def test_invalid_fault_and_nonfinite_telemetry_rejected(self):
        self.peer.hello()
        self.port.filter = lambda reply: reply | {"fault": "driver_alarm"}
        self.assertEqual(self.peer.setpoint(False, ZERO).fault, "driver_alarm")
        with self.assertRaises(LinkError):
            self.peer.setpoint(True, ZERO)
        for raw in (b'{"x":NaN}\n', b'{"x":1,"x":2}\n', b'[]\n', b'{}', b'x' * 2049 + b'\n'):
            with self.subTest(raw=raw[:30]), self.assertRaises(LinkError):
                decode_frame(raw)

    def test_absolute_deadline_and_partial_frame(self):
        self.port.write = lambda data: len(data)
        self.port.input = bytearray(b'{"type":')
        with self.assertRaisesRegex(LinkError, "reply_timeout"):
            self.peer.hello()
        self.assertLess(self.clock.now, 0.18)

    def test_local_watchdog_invalidates_session_and_stops(self):
        self.peer.hello()
        self.peer.setpoint(True, ZERO | {"x_mps": 0.01})
        self.clock.now += 0.251
        self.port.board.tick()
        self.assertIsNone(self.port.board.robot.owner)
        self.assertIsNone(self.port.board.session)
        with self.assertRaises(LinkError):
            self.peer.setpoint(True, ZERO)

    def test_invalid_command_disables_emulator(self):
        self.peer.hello()
        self.peer.setpoint(True, ZERO)
        bad = self.peer.request("setpoint", enabled=True, command={"x_mps": 8})
        with self.assertRaises(LinkError):
            self.port.board.handle(bad)
        self.assertIsNone(self.port.board.robot.owner)


class GatewayTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.ports = {}

        def opener(board, settings, backend):
            role = "arm" if board.device_id.endswith("ARM") else "base"
            port = FakePort(role, self.clock)
            self.ports[role] = port
            return port

        self.gateway = SerialGateway(config(), opener=opener, clock=self.clock)
        self.robot = STM32Robot(self.gateway, self.clock)
        self.gateway._connect()

    def tearDown(self):
        self.gateway._fail("test_cleanup")

    def enable(self):
        self.robot.apply("pc", Enable(type="enable", seq=1, enabled=True))

    def test_both_connected_initially_disabled_real_telemetry_only(self):
        state = self.robot.snapshot()
        self.assertTrue(state["hardware_connected"])
        self.assertTrue(state["control_ready"])
        self.assertFalse(state["enabled"])
        self.assertEqual(state["arm"]["z_left_mm"], 350)

    def test_axis_routing_and_stop(self):
        self.enable()
        self.robot.apply("pc", motion(zl_mps=0.01, x_mps=0.02, vy_mps=-0.1))
        self.gateway._cycle()
        self.assertEqual(self.ports["arm"].sent[-1]["command"]["zr_mps"], 0)
        self.assertEqual(self.ports["arm"].sent[-1]["command"]["zl_mps"], 0.01)
        self.assertEqual(self.ports["base"].sent[-1]["command"], {"vx_mps": 0.0, "vy_mps": -0.1, "wz_radps": 0.0})
        self.robot.apply("observer", Stop(type="stop", seq=3))
        self.gateway._cycle()
        self.assertTrue(all(not port.sent[-1]["enabled"] for port in self.ports.values()))

    def test_reenable_does_not_resend_rejected_acceleration(self):
        self.enable()
        self.robot.acceleration = Acceleration.model_construct(z_mps2=.11)
        self.robot.stop("hardware_fault")
        self.enable()
        self.gateway._cycle()
        self.assertEqual(self.ports["arm"].sent[-1]["acceleration"]["z_mps2"], .1)

    def test_missing_reference_is_explained_after_fault_clear(self):
        self.gateway.states["arm"]["ready"] = False
        self.gateway.states["arm"]["arm"]["homed"] = False
        self.assertEqual(self.robot.snapshot()["control_blocked_reason"], "homing_required")

    def test_acceleration_limits_routed_to_each_controller(self):
        self.enable()
        self.robot.apply("pc", motion(x_mps=0.01, acceleration=Acceleration(
            base_mps2=0.15, rotation_radps2=0.4, z_mps2=0.05, x_mps2=0.1, gripper_per_s=1.0)))
        self.gateway._cycle()
        self.assertEqual(self.ports["arm"].sent[-1]["acceleration"],
                         {"z_mps2": 0.05, "x_mps2": 0.1, "gripper_per_s": 1.0})
        self.assertEqual(self.ports["base"].sent[-1]["acceleration"],
                         {"base_mps2": 0.15, "rotation_radps2": 0.4})

    def test_disconnect_stops_both_and_reconnect_never_resumes(self):
        self.enable()
        self.robot.apply("pc", motion(vx_mps=0.1))
        self.gateway._cycle()
        self.ports["base"].fail = True
        with self.assertRaises(OSError):
            self.gateway._cycle()
        self.gateway._fail("unplugged")
        self.assertFalse(self.ports["arm"].sent[-1]["enabled"])
        self.gateway._connect()
        self.assertFalse(self.robot.snapshot()["enabled"])
        self.assertFalse(self.robot.snapshot()["outputs_enabled"])
        with self.assertRaisesRegex(ControlError, "not_owner"):
            self.robot.apply("pc", motion(vx_mps=0.1))

    def test_stale_telemetry_is_unknown_not_simulated(self):
        self.clock.now += 0.351
        state = self.robot.snapshot()
        self.assertFalse(state["hardware_connected"])
        self.assertIsNone(state["arm"]["z_left_mm"])
        self.assertIsNone(state["base"]["x_m"])

    def test_lease_expires_without_event_loop_ticks(self):
        self.enable()
        self.robot.apply("pc", motion(x_mps=0.01))
        self.clock.now += 0.601
        with self.assertRaisesRegex(LinkError, "operator_lease_expired"):
            self.gateway._desired()
        self.assertFalse(self.robot.snapshot()["enabled"])

    def test_uncommissioned_homing_and_gripper_guards(self):
        self.gateway.config = config(commissioned=False)
        with self.assertRaisesRegex(ControlError, "commissioning_required"):
            self.enable()
        self.gateway.config = config()
        self.gateway.states["arm"]["arm"]["homed"] = False
        with self.assertRaisesRegex(ControlError, "homing_required"):
            self.enable()
        self.gateway.states["arm"]["arm"]["homed"] = True
        self.gateway.identities["arm"]["capabilities"].remove("gripper_velocity")
        self.enable()
        with self.assertRaisesRegex(ControlError, "gripper_not_supported"):
            self.robot.apply("pc", motion(gripper=1.0))
        self.assertIsNone(self.robot.owner)

    def test_z_difference_lookahead_rejects_outward_motion(self):
        self.gateway.states["arm"]["arm"]["z_left_mm"] = 448.0
        self.enable()
        with self.assertRaisesRegex(ControlError, "z_difference_limit"):
            self.robot.apply("pc", motion(zl_mps=0.05))
        self.enable()
        self.robot.apply("pc", motion(zl_mps=-0.01))

    def test_soft_skew_firmware_keeps_control_for_outward_command(self):
        self.gateway.states["arm"]["arm"]["z_left_mm"] = 448.0
        self.gateway.identities["arm"]["capabilities"].append("z_skew_soft_limit_v1")
        self.enable()
        self.robot.apply("pc", motion(zl_mps=0.05))
        self.assertEqual(self.robot.owner, "pc")
        self.robot.apply("pc", motion(zl_mps=-0.01))
        self.assertEqual(self.robot.owner, "pc")

    def test_old_generation_cannot_enable_after_reconnect(self):
        old_epoch = self.gateway.epoch
        self.gateway._fail("test")
        self.gateway._connect()
        with self.assertRaises(ControlError):
            self.gateway.submit(old_epoch, True, ZERO, self.clock() + 0.4)


class SettingsTests(unittest.TestCase):
    def test_defaults_stay_in_simulation(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(load_robot_settings(), ("mock", None))

    def test_duplicate_boards_and_unknown_limit_rejected(self):
        for changes in ({"base": {"port": "COM-ARM", "device_id": "other"}}, {"max_z_difference_mm": None}):
            with self.assertRaises(ValidationError):
                config(**changes)

    def test_network_urls_never_allowed_in_hardware_mode(self):
        settings = config(arm={"port": "socket://127.0.0.1:9101", "device_id": "EMULATOR-ARM"})
        with self.assertRaises(ValueError):
            settings.check_ports("stm32")
        with self.assertRaises(ValueError):
            config().check_ports("stm32-emulator")


class SocketIntegrationTests(unittest.TestCase):
    def test_two_actual_pyserial_connections_and_clean_shutdown(self):
        with EmulatorServer("arm", 0) as arm, EmulatorServer("base", 0) as base:
            servers = (arm, base)
            threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in servers]
            for thread in threads:
                thread.start()
            settings = config(**{role: {"port": f"socket://127.0.0.1:{server.server_address[1]}",
                                        "device_id": "EMULATOR-" + role.upper()}
                                 for role, server in zip(("arm", "base"), servers)})
            gateway = SerialGateway(settings, "stm32-emulator")
            robot = STM32Robot(gateway)
            gateway.start()
            try:
                until = time.monotonic() + 3
                while not gateway.status()["connected"] and time.monotonic() < until:
                    time.sleep(0.02)
                self.assertTrue(gateway.status()["connected"], gateway.status())
                robot.apply("pc", Enable(type="enable", seq=1, enabled=True))
                for _ in range(5):
                    robot.apply("pc", motion(x_mps=0.01, vx_mps=0.05))
                    time.sleep(0.08)
                state = robot.snapshot()
                self.assertEqual(state["backend"], "stm32-emulator")
                self.assertFalse(state["hardware_connected"])
                self.assertGreater(state["arm"]["x_mm"], 300)
                self.assertGreater(state["base"]["x_m"], 0)
                robot.apply("pc", Stop(type="stop", seq=8))
                time.sleep(0.12)
                self.assertFalse(robot.snapshot()["outputs_enabled"])
            finally:
                gateway.close()
                self.assertFalse(gateway.thread.is_alive())
                for server in servers:
                    server.stopping.set()
                    server.shutdown()
                for thread in threads:
                    thread.join()


if __name__ == "__main__":
    unittest.main()
