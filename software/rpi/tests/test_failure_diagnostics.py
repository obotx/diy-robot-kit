import unittest
from types import SimpleNamespace
from unittest.mock import Mock

from app.stm32 import SerialGateway
from app.stm32_protocol import LinkError, SerialPeer
from app.robot import ZERO
from test_stm32 import Clock, FakePort, config


class FailureDiagnosticsTests(unittest.TestCase):
    def test_original_failure_survives_reconnect_retries_and_recovery(self):
        gateway = SerialGateway(config(), clock=Clock())
        gateway.connected = True
        gateway._fail("soft_limit_margin")
        gateway._fail("reply_timeout")
        self.assertEqual(gateway.status()["last_failure"]["reason"], "soft_limit_margin")
        gateway.connected = True
        gateway.error = None
        self.assertEqual(gateway.status()["last_failure"]["reason"], "soft_limit_margin")
        gateway._fail("local_watchdog")
        self.assertEqual(gateway.status()["last_failure"]["reason"], "local_watchdog")

    def test_setpoint_preserves_controller_reason(self):
        clock = Clock()
        port = FakePort("arm", clock)
        peer = SerialPeer(port, "arm", "EMULATOR-ARM", "a" * 32, clock)
        peer.hello()
        port.filter = lambda reply: reply | {"accepted": False, "fault": "soft_limit_margin"}
        with self.assertRaisesRegex(LinkError, "soft_limit_margin"):
            peer.setpoint(True, ZERO)

    def test_home_idle_without_reference_is_not_completion(self):
        for axis, homed, succeeds in [("ZL", {}, False), ("ZL", {"ZL": True}, True),
                                        ("Z", {"ZL": True}, False),
                                        ("Z", {"ZL": True, "ZR": True}, True)]:
            with self.subTest(axis=axis, homed=homed):
                gateway = SerialGateway(config(), clock=Clock())
                reply = Mock(arm=SimpleNamespace(active_axis=None, homed_axes=homed))
                reply.model_dump.return_value = {}
                gateway.peers = {"arm": Mock(arm_action=Mock(return_value=reply))}
                gateway.action = ("test", "home", {"axis": axis}, 1.0)
                if succeeds:
                    gateway._cycle()
                    self.assertEqual(gateway.action_result, ("test", "complete"))
                else:
                    with self.assertRaisesRegex(LinkError, "homing_ended_without_reference"):
                        gateway._cycle()
                    self.assertIsNone(gateway.action_result)


if __name__ == "__main__":
    unittest.main()
