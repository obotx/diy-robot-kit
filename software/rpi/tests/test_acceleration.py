import math
import unittest

from pydantic import ValidationError

from app.robot import Acceleration, Enable, Motion, RobotSimulator, ZERO, ramp


class AccelerationTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.robot = RobotSimulator(lambda: self.now)
        self.robot.apply("test", Enable(type="enable", seq=1, enabled=True))

    def motion(self, **changes):
        self.robot.apply("test", Motion(type="motion", seq=2, **(ZERO | changes)))

    def test_x_acceleration_integrates_and_zero_command_brakes(self):
        self.motion(x_mps=0.1)
        self.now = 0.1
        self.robot.tick()
        self.assertAlmostEqual(self.robot.velocity["x_mps"], 0.02)
        self.assertAlmostEqual(self.robot.x_mm, 301)
        self.motion()
        self.now = 0.2
        self.robot.tick()
        self.assertAlmostEqual(self.robot.velocity["x_mps"], 0)
        self.assertAlmostEqual(self.robot.x_mm, 302)

    def test_separate_axis_settings_reach_simulator(self):
        self.motion(x_mps=0.1, zl_mps=0.05, gripper=1.0,
                    acceleration=Acceleration(x_mps2=0.1, z_mps2=0.05, gripper_per_s=1.0))
        self.now = 0.1
        state = self.robot.snapshot()
        self.assertAlmostEqual(self.robot.x_mm, 300.5)
        self.assertAlmostEqual(self.robot.z_left_mm, 350.25)
        self.assertAlmostEqual(self.robot.z_right_mm, 350)
        self.assertAlmostEqual(self.robot.gripper_pct, 70.25)
        self.assertEqual(state["acceleration"]["x_mps2"], 0.1)

    def test_base_vector_acceleration_not_per_axis(self):
        self.motion(vx_mps=0.1, vy_mps=0.1)
        self.now = 0.1
        self.robot.tick()
        self.assertAlmostEqual(math.hypot(self.robot.velocity["vx_mps"], self.robot.velocity["vy_mps"]), 0.03)

    def test_stop_clears_velocity_immediately_in_simulator(self):
        self.motion(x_mps=0.1)
        self.now = 0.1
        self.robot.tick()
        position = self.robot.x_mm
        self.robot.stop("operator")
        self.now = 0.2
        self.robot.tick()
        self.assertEqual(self.robot.velocity, ZERO)
        self.assertEqual(self.robot.x_mm, position)

    def test_ramp_never_overshoots_and_reverses_through_zero(self):
        self.assertEqual(ramp(0, 1, 2, 1), (1, 0.75))
        end, distance = ramp(1, -1, 2, 0.5)
        self.assertEqual(end, 0)
        self.assertEqual(distance, 0.25)

    def test_invalid_acceleration_rejected(self):
        for value in (0, -1, 100, float("nan"), float("inf"), True):
            with self.subTest(value=value), self.assertRaises(ValidationError):
                Acceleration(x_mps2=value)


if __name__ == "__main__":
    unittest.main()
