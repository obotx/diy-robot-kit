import asyncio
import copy
import json
from pathlib import Path
import unittest
from unittest.mock import patch
from types import SimpleNamespace

from app.robot import ServiceUnlock, ServiceJog, Enable, Stop, ControlError, COMMAND, ClearFault, Home
from app.stm32 import STM32Robot
from app.service_access import ServiceAccess, digest


class Clock:
    now = 1.0
    def __call__(self):
        return self.now


class Gateway:
    def __init__(self):
        self.config = SimpleNamespace(arm_only=True, commissioned=True)
        self.state = dict(connected=True, epoch=1, identities={"arm": {"capabilities": ["service_jog_v1", "service_jog_v2", "x_microstep_128"]}})
        self.sent = []
    def status(self):
        return copy.deepcopy(self.state)
    def submit(self, *args, **kwargs):
        self.sent.append((args, kwargs))


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.gateway = Gateway()
        self.robot = STM32Robot(self.gateway, self.clock)

    def unlock(self, owner="pc"):
        self.robot.apply(owner, ServiceUnlock(type="service_unlock", seq=1,
            password="example-test-password", confirm_bypass=True))
        self.robot.apply(owner, Enable(type="enable", seq=2, enabled=True))

    def jog(self, owner="pc", **values):
        self.robot.apply(owner, ServiceJog(type="service_jog", seq=2,
            **(dict(zl_mps=0., zr_mps=0., x_mps=0.) | values)))

    def test_unlock_starts_zero_and_routes_simultaneous_axes(self):
        self.unlock()
        self.assertFalse(any(self.gateway.sent[-1][0][2].values()))
        self.jog(zl_mps=.01, zr_mps=-.01, x_mps=.01)
        self.assertTrue(self.gateway.sent[-1][1]["service"])
        self.assertEqual(self.robot.command["zr_mps"], -.01)

    def test_pair_service_routes_base_and_gripper(self):
        self.gateway.config.arm_only = False
        self.gateway.state["identities"]["arm"]["capabilities"].append("service_gripper_v1")
        self.unlock()
        self.jog(vx_mps=.1, wz_radps=.2, gripper=-.5, x_mps=.01)
        command = self.gateway.sent[-1][0][2]
        self.assertEqual(command["vx_mps"], .1)
        self.assertEqual(command["gripper"], -.5)

    def test_arm_only_cannot_silently_ignore_wheel_command(self):
        self.unlock()
        with self.assertRaisesRegex(ControlError, "base_not_connected"):
            self.jog(vx_mps=.1)

    def test_unowned_and_old_firmware_and_base_mode_rejected(self):
        with self.assertRaises(ControlError): self.jog()
        self.gateway.state["identities"]["arm"]["capabilities"] = []
        with self.assertRaises(ControlError): self.unlock()
        self.gateway.config.arm_only = False
        with self.assertRaises(ControlError): self.unlock()

    def test_motion_continues_beyond_two_seconds(self):
        self.unlock()
        for _ in range(25):
            self.jog(zl_mps=.01)
            self.clock.now += .1
        self.robot.tick()
        self.assertFalse(self.robot.service_release_required)
        self.assertEqual(self.robot.command["zl_mps"], .01)
        self.jog()
        self.jog(zl_mps=.01)
        self.assertEqual(self.robot.command["zl_mps"], .01)

    def test_unlock_persists_beyond_sixty_seconds_and_watchdog_remains(self):
        self.unlock()
        for _ in range(600):
            self.jog()
            self.clock.now += .1
        self.clock.now += .01
        self.robot.tick()
        self.assertEqual(self.robot.owner, "pc")
        self.clock.now += .601
        self.robot.tick()
        self.assertEqual(self.robot.service_until, 0)

    def test_disconnect_stop_epoch_and_normal_commands(self):
        self.unlock()
        with self.assertRaises(ControlError): self.jog(owner="other")
        with self.assertRaises(ControlError): self.robot.apply("pc", Enable(type="enable", seq=3, enabled=True))
        self.robot.disconnect("pc")
        self.assertFalse(self.robot.service_until)
        self.unlock()
        self.gateway.state["epoch"] += 1
        self.robot.tick()
        self.assertIsNone(self.robot.owner)
        self.unlock()
        self.robot.apply("other", Stop(type="stop", seq=4))
        self.assertIsNone(self.robot.owner)

    def test_input_speed_and_confirmation_validation(self):
        with self.assertRaises(ValueError):
            COMMAND.validate_python(dict(type="service_jog", seq=1, zl_mps=.191, zr_mps=0., x_mps=0.))
        COMMAND.validate_python(dict(type="service_jog", seq=1, zl_mps=.19, zr_mps=-.19, x_mps=.5))
        with self.assertRaises(ValueError):
            COMMAND.validate_python(dict(type="service_unlock", seq=1, password="secret", confirm_bypass=False))

    def test_disconnect_racing_burst_stop_does_not_break_tick(self):
        self.unlock()
        self.jog(zl_mps=.01)
        self.clock.now += 2.01
        self.robot.last_command = self.clock.now
        original_submit = self.gateway.submit
        def disconnected_submit(epoch, enabled, *args, **kwargs):
            if enabled:
                raise ControlError("hardware_disconnected")
            return original_submit(epoch, enabled, *args, **kwargs)
        self.gateway.submit = disconnected_submit
        self.gateway.state["connected"] = False
        self.robot.tick()
        self.assertIsNone(self.robot.owner)
        self.assertEqual(self.robot.reason, "hardware_fault")

    def test_service_only_is_not_normal_commissioning(self):
        from app.configure_arm import select_service
        from test_configure_zl import identity
        from app.stm32_config import STM32Config
        caps = [c for c in identity().capabilities if c != "zl_only"] + ["service_jog_v1"]
        config = select_service([("COM1", identity(capabilities=caps))])
        self.assertFalse(config.commissioned)
        self.assertTrue(config.service_only)
        STM32Config.model_validate(config.model_dump())
        self.gateway.config = config
        self.unlock()
        self.assertEqual(self.robot._ready(self.gateway.status()), "commissioning_required")

    def test_password_hash_and_global_throttling(self):
        salt = "ab" * 16
        record = dict(algorithm="pbkdf2-sha256-600000", salt=salt, digest=digest("test-password", salt))
        with patch.object(Path, "read_text", return_value=json.dumps(record)):
            access = ServiceAccess(clock=self.clock)
        self.assertFalse(asyncio.run(access.authorize("wrong")))
        self.assertFalse(asyncio.run(access.authorize("test-password")))
        self.clock.now += 2
        self.assertTrue(asyncio.run(access.authorize("test-password")))
        self.assertNotIn("test-password", json.dumps(access.record))

    def test_service_only_clear_does_not_commission_or_enable_motion(self):
        self.gateway.config.commissioned = False
        self.gateway.config.service_only = True
        self.gateway.state["identities"]["arm"]["capabilities"].append("arm_actions_v1")
        from unittest.mock import Mock
        self.gateway.submit_action = Mock()
        self.robot.apply("pc", ClearFault(type="clear_fault", seq=1))
        self.assertEqual(self.gateway.submit_action.call_args.args[2], "clear_fault")
        self.assertFalse(self.robot.service_until)
        self.assertFalse(any(self.robot.command.values()))
        self.robot.stop("operator")
        with self.assertRaisesRegex(ControlError, "commissioning_required"):
            self.robot.apply("pc", Home(type="home", seq=2, axis="X"))

    def test_fault_retains_mode_but_requires_explicit_reenable(self):
        from app.robot import ServiceLock, Acceleration
        self.unlock()
        self.robot.apply("pc", ServiceJog(type="service_jog", seq=3, zl_mps=.01, zr_mps=0., x_mps=.02,
            acceleration=Acceleration(z_mps2=.08, x_mps2=.4)))
        self.assertEqual(self.gateway.sent[-1][0][4].x_mps2, .4)
        self.gateway.state["epoch"] += 1
        self.robot.tick()
        self.assertEqual(self.robot.service_client, "pc")
        self.assertIsNone(self.robot.owner)
        with self.assertRaises(ControlError): self.jog(x_mps=.02)
        self.robot.apply("pc", Enable(type="enable", seq=4, enabled=True))
        self.assertFalse(any(self.gateway.sent[-1][0][2].values()))
        self.robot.apply("pc", ServiceLock(type="service_lock", seq=5))
        self.assertIsNone(self.robot.service_client)
        self.assertIsNone(self.robot.owner)

    def test_unlock_alone_does_not_enable_and_disconnect_revokes(self):
        self.robot.apply("pc", ServiceUnlock(type="service_unlock", seq=1,
            password="example-test-password", confirm_bypass=True))
        self.assertEqual(self.robot.service_client, "pc")
        self.assertIsNone(self.robot.owner)
        self.assertFalse(self.gateway.sent)
        self.robot.disconnect("pc")
        self.assertIsNone(self.robot.service_client)
