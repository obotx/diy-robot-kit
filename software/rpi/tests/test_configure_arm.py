import unittest

from app.configure_arm import select_arm, select_robot
from app.stm32_protocol import LinkError
from test_configure_zl import identity


class ConfigureArmTests(unittest.TestCase):
    def full(self, **changes):
        caps = [c for c in identity().capabilities if c != "zl_only"]
        return identity(**({"capabilities": caps} | changes))

    def test_full_arm_with_optional_base(self):
        for replies in ([("arm", self.full())],
                        [("base", identity("base")), ("arm", self.full())]):
            config = select_arm(replies, 100)
            self.assertTrue(config.commissioned)
            self.assertTrue(config.arm_only)
            self.assertIsNone(config.base)
            self.assertEqual(config.arm.device_id, "arm-TEST")

    def test_unified_robot_profile_preserves_firmware_limit(self):
        base = identity("base", capabilities=["full_state_v1", "local_watchdog",
            "acceleration_limits", "base_clear_fault_v1"])
        config = select_robot([("arm", self.full()), ("base", base)])
        self.assertTrue(config.commissioned)
        self.assertFalse(config.service_only)
        self.assertFalse(config.arm_only)
        self.assertEqual(config.max_z_difference_mm, 100)
        self.assertEqual(config.base.device_id, "base-TEST")

    def test_unified_robot_rejects_missing_or_old_base(self):
        for replies in ([("arm", self.full())],
                        [("arm", self.full()), ("base", identity("base"))]):
            with self.assertRaises(LinkError):
                select_robot(replies)

    def test_rejects_zl_enabled_missing_and_duplicate_boards(self):
        for replies in ([], [("a", identity())], [("a", self.full(enabled=True))],
                        [("a", self.full()), ("b", self.full())],
                        [("a", self.full(capabilities=["arm_actions_v1"]))]):
            with self.assertRaises(LinkError):
                select_arm(replies, 100)

    def test_limit_must_be_confirmed_and_match_firmware(self):
        for limit in (0, -1, 1001, 50, float("nan"), float("inf")):
            with self.assertRaises(LinkError):
                select_arm([("a", self.full())], limit)
