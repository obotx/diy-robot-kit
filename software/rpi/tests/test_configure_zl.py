import unittest
from unittest.mock import patch
from tempfile import TemporaryDirectory
from pathlib import Path

from app.configure_zl import select_zl, main
from app.stm32_config import STM32Config
from app.stm32_protocol import Identity, LinkError


def identity(role="arm", **changes):
    return Identity.model_validate(dict(type="identity", protocol=1, session="a" * 32,
        seq=1, role=role, device_id=role + "-TEST", boot_id="boot", enabled=False,
        firmware="arm-link-2" if role == "arm" else "base-link-1",
        capabilities=["full_state_v1", "local_watchdog", "acceleration_limits",
                      "arm_actions_v1", "zl_only", "z_difference_limit"],
        watchdog_ms=250, max_z_difference_mm=100.0) | changes)


class ConfigureZLTests(unittest.TestCase):
    def test_sole_zl_or_pair_creates_pinned_arm_only_config(self):
        for replies in ([('/dev/ttyACM1', identity())],
                        [('/dev/ttyACM0', identity('base')), ('/dev/ttyACM1', identity())]):
            config = select_zl(replies)
            self.assertTrue(config.arm_only)
            self.assertTrue(config.commissioned)
            self.assertIsNone(config.base)
            self.assertEqual(config.arm.device_id, "arm-TEST")
            self.assertEqual(config.max_z_difference_mm, 100.0)

    def test_wrong_profiles_and_duplicates_rejected(self):
        for replies in ([], [('x', identity('base'))],
                        [('a', identity()), ('b', identity())],
                        [('a', identity(enabled=True))],
                        [('a', identity(firmware="arm-link-1"))],
                        [('a', identity(capabilities=["arm_actions_v1"]))],
                        [('a', identity(max_z_difference_mm=2.0))]):
            with self.assertRaises(LinkError):
                select_zl(replies)

    def test_write_and_backup_only_after_successful_detection(self):
        with TemporaryDirectory(dir=Path(__file__).resolve().parents[1]) as directory:
            path = Path(directory) / "config.json"
            path.write_text("old configuration")
            argv = ["configure_zl", "--confirmed-isolated-zl", "--output", str(path)]
            with patch("sys.argv", argv), patch("app.configure_zl.detect", side_effect=LinkError("missing")):
                with self.assertRaises(LinkError):
                    main()
            self.assertEqual(path.read_text(), "old configuration")
            config = select_zl([("/dev/ttyACM1", identity())])
            with patch("sys.argv", argv), patch("app.configure_zl.detect", return_value=config), \
                 patch("app.configure_zl.stable_port", return_value="/dev/serial/by-id/test-arm"):
                main()
            result = STM32Config.model_validate_json(path.read_text())
            self.assertEqual(result.arm.port, "/dev/serial/by-id/test-arm")
            backup = list(Path(directory).glob("*.bak-*"))
            self.assertEqual(len(backup), 1)
            self.assertEqual(backup[0].read_text(), "old configuration")
