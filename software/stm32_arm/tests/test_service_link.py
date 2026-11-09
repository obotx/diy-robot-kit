import unittest
from test_link import Port
from app.robot import Acceleration, ZERO
from app.stm32_protocol import SerialPeer, LinkError
from app.stm32 import SerialGateway, STM32Robot
from app.stm32_config import STM32Config, BoardConfig
from app.robot import ServiceUnlock, ServiceJog, Stop, Enable


class ServiceLinkTests(unittest.TestCase):
    def setUp(self):
        self.port = Port()
        self.peer = SerialPeer(self.port, "arm", "ARM-TEST", "a" * 32)
        self.peer.hello()
        self.accel = Acceleration(z_mps2=0.05, x_mps2=0.05)

    def tearDown(self):
        self.port.close()

    def jog(self, **values):
        return self.peer.setpoint(True, ZERO | values, self.accel, service=True)

    def test_zero_entry_then_all_axes_without_homing_and_limits(self):
        self.port.command("unhome")
        self.port.command("limit")
        state = self.jog()
        self.assertTrue(state.arm.service_active)
        self.assertFalse(state.arm.homed)
        self.assertFalse(state.ready)
        self.assertTrue(state.arm.limit_switches["X_MAX"])
        self.jog(zl_mps=.01, zr_mps=-.01, x_mps=.01)
        state = self.port.command("advance 100")
        self.assertTrue(state["enabled"])
        self.assertEqual(state["target"], [10000, -10000, 10000])
        self.assertEqual(state["velocity"], [5000, -5000, 5000])

    def test_entry_with_motion_rejected(self):
        with self.assertRaisesRegex(LinkError, "zero_entry"):
            self.jog(zl_mps=.001)
        self.assertFalse(self.port.command("advance 1")["enabled"])

    def test_selected_acceleration_and_release_ramp(self):
        self.accel = Acceleration(z_mps2=.1, x_mps2=.4)
        self.jog()
        self.jog(zl_mps=.02, x_mps=.08)
        self.assertEqual(self.port.command("advance 100")["velocity"], [10000, 0, 40000])
        self.jog()
        self.assertEqual(self.port.command("advance 50")["velocity"], [5000, 0, 20000])
        self.assertEqual(self.port.command("advance 50")["velocity"], [0, 0, 0])

    def test_speed_and_acceleration_not_bypassable(self):
        self.jog()
        self.jog(zl_mps=.19, zr_mps=-.19, x_mps=.5)
        self.assertEqual(self.port.command("advance 1")["target"], [190000, -190000, 500000])
        with self.assertRaisesRegex(LinkError, "range_or_acceleration_limit"):
            self.jog(x_mps=.501)
        self.assertFalse(self.port.command("advance 1")["enabled"])

    def test_motion_continues_beyond_two_seconds(self):
        self.jog()
        for _ in range(22):
            self.jog(zl_mps=.01)
            self.port.command("advance 100")
        state = self.jog(zl_mps=.01)
        self.assertFalse(state.arm.service_release_required)
        self.assertGreater(self.port.command("advance 1")["velocity"][0], 0)
        self.jog()
        self.jog(zl_mps=.01)
        self.assertGreater(self.port.command("advance 100")["velocity"][0], 0)

    def test_mcu_session_remains_enabled_with_heartbeats(self):
        self.jog()
        for _ in range(599):
            self.port.command("advance 100")
            self.jog()
        state = self.port.command("advance 100")
        self.assertTrue(state["enabled"])
        self.assertFalse(state["expired"])

    def test_watchdog_and_driver_fault_still_stop(self):
        self.jog()
        self.jog(x_mps=.01)
        self.assertFalse(self.port.command("advance 250")["enabled"])
        self.peer.hello()
        self.port.command("fault")
        with self.assertRaisesRegex(LinkError, "latched_motion_fault"):
            self.jog()

    def test_stop_and_reconnect_never_restore_service_or_homed(self):
        self.jog()
        state = self.peer.setpoint(False, ZERO)
        self.assertFalse(state.arm.service_active)
        self.assertFalse(state.arm.homed)
        with self.assertRaisesRegex(LinkError, "not_homed"):
            self.peer.setpoint(True, ZERO)
        self.peer.hello()
        state = self.peer.setpoint(False, ZERO)
        self.assertFalse(state.arm.service_active)

    def test_normal_command_cannot_inherit_bypass(self):
        self.jog()
        with self.assertRaisesRegex(LinkError, "exit_service_first"):
            self.peer.setpoint(True, ZERO | {"x_mps": .1})

    def test_gateway_entry_is_zero_then_jog_then_stop(self):
        config = STM32Config(arm_only=True, service_only=True,
            arm=BoardConfig(port="test", device_id="ARM-TEST"), max_z_difference_mm=220.)
        gateway = SerialGateway(config, opener=lambda *args: Port())
        robot = STM32Robot(gateway)
        try:
            gateway._connect()
            robot.apply("pc", ServiceUnlock(type="service_unlock", seq=1,
                password="example-password", confirm_bypass=True))
            robot.apply("pc", Enable(type="enable", seq=2, enabled=True))
            robot.apply("pc", ServiceJog(type="service_jog", seq=2, zl_mps=.01, zr_mps=-.01, x_mps=.01))
            gateway._cycle()
            self.assertEqual(gateway.peers["arm"].port.command("advance 1")["target"], [0, 0, 0])
            gateway._cycle()
            self.assertEqual(gateway.peers["arm"].port.command("advance 1")["target"], [10000, -10000, 10000])
            robot.apply("pc", Stop(type="stop", seq=3))
            gateway._cycle()
            self.assertFalse(gateway.states["arm"]["arm"]["service_active"])
        finally:
            gateway.close()
            gateway._fail("test_cleanup")
