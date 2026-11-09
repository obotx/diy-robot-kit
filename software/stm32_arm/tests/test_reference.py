import unittest
from test_link import Port
from app.robot import ZERO
from app.stm32_protocol import SerialPeer, LinkError


class ReferenceTests(unittest.TestCase):
    def setUp(self):
        self.port = Port()
        self.peer = SerialPeer(self.port, "arm", "ARM-TEST", "a" * 32)
        self.peer.hello()

    def tearDown(self):
        self.port.close()

    def complete(self, expected):
        for _ in range(1500):
            self.port.command("plant 100")
            state = self.peer.arm_action("keepalive")
            if state.arm.active_axis is None:
                self.assertEqual(state.arm.reference_stage, expected)
                self.assertFalse(state.enabled)
                return state
        self.fail("reference action did not complete")

    def test_sequence_and_no_automatic_next(self):
        self.port.command("unhome")
        self.peer.arm_action("level_down")
        self.assertEqual(self.port.command("advance 100")["target"], [-5000,5000,0])
        self.port.command("levelband 501000 503000")
        self.complete(1)
        self.assertEqual(self.peer.setpoint(False, ZERO).arm.reference_stage, 1)
        self.peer.arm_action("home_z")
        state = self.complete(2)
        self.assertTrue(state.arm.homed_axes["ZL"])
        self.assertTrue(state.arm.homed_axes["ZR"])
        self.assertFalse(state.arm.homed_axes["X"])
        self.peer.arm_action("lift_z")
        state = self.complete(3)
        self.assertAlmostEqual(state.arm.z_left_mm, 200, delta=.01)
        self.assertAlmostEqual(state.arm.z_right_mm, 200, delta=.01)
        self.peer.arm_action("home_x")
        self.assertLessEqual(self.port.command("advance 100")["target"][2], 0)
        self.assertTrue(self.complete(4).arm.homed)

    def test_opposite_direction_and_watchdog(self):
        self.peer.arm_action("level_up")
        self.assertEqual(self.port.command("advance 100")["target"], [5000,-5000,0])
        self.assertFalse(self.port.command("advance 150")["enabled"])

    def test_wrong_order_rejected(self):
        with self.assertRaisesRegex(LinkError, "reference_order"):
            self.peer.arm_action("home_x")

    def test_level_sensor_requires_centering_before_z_homing(self):
        self.port.command("unhome")
        self.port.command("level 1")
        with self.assertRaisesRegex(LinkError, "reference_order"):
            self.peer.arm_action("home_z")

    def test_level_skip_rechecks_sensor(self):
        self.port.command("level 1")
        self.port.command("level 0")
        with self.assertRaisesRegex(LinkError, "level_not_detected"):
            self.peer.arm_action("level_center")

    def test_cancel_does_not_report_completed_level(self):
        self.peer.arm_action("level_down")
        self.port.command("advance 100")
        state = self.peer.setpoint(False, ZERO)
        self.assertEqual(state.arm.reference_stage, 0)
        self.assertIsNone(state.arm.active_axis)

    def test_sensor_bounce_resumes_selected_direction(self):
        self.peer.arm_action("level_down")
        self.port.command("advance 30")
        self.port.command("level 1")
        self.port.command("advance 10")
        self.port.command("level 0")
        state = self.port.command("advance 10")
        self.assertEqual(state["target"], [-5000,5000,0])

    def test_hardware_fault_never_completes_level(self):
        self.peer.arm_action("level_down")
        self.port.command("fault")
        self.port.command("advance 10")
        with self.assertRaisesRegex(LinkError, "latched_motion_fault"):
            self.peer.arm_action("keepalive")

    def test_center_from_both_sides_and_inside(self):
        for step, lower, upper in (("level_down",501000,503000),
                                   ("level_up",497000,499000),
                                   ("level_center",499000,501000)):
            with self.subTest(step=step):
                self.port.close()
                self.setUp()
                self.port.command(f"levelband {lower} {upper}")
                self.peer.arm_action(step)
                self.complete(1)
                position = self.port.command("state")["position"]
                self.assertAlmostEqual(position[1], (lower+upper)/2, delta=150)
                self.assertAlmostEqual(position[0]+position[1], 1000000, delta=20)
                self.assertEqual(position[2], 500000)

    def test_stuck_sensor_does_not_report_level_complete(self):
        self.port.command("level 1")
        self.peer.arm_action("level_center")
        for _ in range(20):
            self.port.command("plant 100")
            state = self.peer.arm_action("keepalive")
            self.assertEqual(state.arm.reference_stage, 0)
            self.assertIsNotNone(state.arm.active_axis)
        state = self.peer.setpoint(False, ZERO)
        self.assertEqual(state.arm.reference_stage, 0)

    def test_service_gripper_and_zero_entry(self):
        self.peer.setpoint(True, ZERO, service=True)
        self.assertTrue(self.peer.setpoint(True, ZERO | {"gripper": .5}, service=True).accepted)
        self.assertTrue(self.peer.setpoint(True, ZERO | {"gripper": -.5}, service=True).accepted)
        self.peer.setpoint(False, ZERO)
        with self.assertRaisesRegex(LinkError, "zero_entry"):
            self.peer.setpoint(True, ZERO | {"gripper": .5}, service=True)
