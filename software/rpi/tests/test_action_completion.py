import unittest

from app.robot import Home, HomeKeepalive, Stop, Enable, Motion, ZERO, ControlError
from app.stm32 import STM32Robot, SerialGateway
from app.stm32_config import STM32Config, BoardConfig
from test_stm32 import Clock


class ActionCompletionTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        config = STM32Config(arm_only=True, commissioned=True, max_z_difference_mm=100.0,
                             arm=BoardConfig(port="fake", device_id="ARM-TEST"))
        self.gateway = SerialGateway(config, clock=self.clock)
        self.gateway.connected = True
        self.gateway.updated = self.clock()
        self.gateway.identities = {"arm": {"capabilities": ["arm_actions_v1", "zl_only"]}}
        self.gateway.states = {"arm": {"ready": True, "fault": None, "arm": {
            "homed": True, "z_left_mm": 0.0, "z_right_mm": None, "x_mm": None}}}
        self.robot = STM32Robot(self.gateway, self.clock)
        self.robot.apply("pc", Home(type="home", seq=1, axis="ZL"))

    def complete(self):
        self.gateway.action_result = (self.robot.action_token, "complete")
        self.gateway.action = None

    def heartbeat(self, who="pc", seq=2):
        self.robot.apply(who, HomeKeepalive(type="home_keepalive", seq=seq))

    def test_late_heartbeat_after_completion_is_noop_then_jog_works(self):
        self.complete()
        last = self.robot.last_command
        self.heartbeat()
        self.assertIsNone(self.robot.owner)
        self.assertEqual(self.robot.reason, "action_complete")
        self.assertEqual(self.robot.last_command, last)
        self.assertIsNone(self.gateway.action)
        self.assertFalse(self.gateway.intent[0])
        self.robot.apply("pc", Enable(type="enable", seq=3, enabled=True))
        self.robot.apply("pc", Motion(type="motion", seq=4, **(ZERO | {"zl_mps": 0.02})))
        self.assertEqual(self.robot.command["zl_mps"], 0.02)

    def test_wrong_client_and_expired_grace_rejected(self):
        self.complete()
        self.robot.tick()
        with self.assertRaisesRegex(ControlError, "not_action_owner"):
            self.heartbeat("observer")
        self.clock.now = 0.2
        self.gateway.updated = self.clock()
        self.heartbeat()
        self.clock.now = 0.61
        self.gateway.updated = self.clock()
        with self.assertRaisesRegex(ControlError, "not_action_owner"):
            self.heartbeat(seq=3)

    def test_stop_does_not_grant_completion_grace(self):
        self.robot.apply("pc", Stop(type="stop", seq=2))
        with self.assertRaisesRegex(ControlError, "not_action_owner"):
            self.heartbeat(seq=3)

    def test_worker_completion_between_tick_and_renew_is_noop(self):
        original = self.gateway.renew_action
        def race(token, deadline):
            self.complete()
            return original(token, deadline)
        self.gateway.renew_action = race
        self.heartbeat()
        self.assertIsNone(self.robot.owner)
        self.assertEqual(self.robot.reason, "action_complete")

    def test_expired_active_lease_is_not_revived(self):
        self.gateway.action = (*self.gateway.action[:3], -1)
        with self.assertRaisesRegex(ControlError, "action_expired"):
            self.heartbeat()

    def test_late_heartbeat_does_not_claim_other_clients_action(self):
        self.complete()
        self.robot.tick()
        self.robot.apply("other", Home(type="home", seq=1, axis="ZL"))
        deadline = self.gateway.action[3]
        with self.assertRaisesRegex(ControlError, "not_action_owner"):
            self.heartbeat()
        self.assertEqual(self.robot.owner, "other")
        self.assertEqual(self.gateway.action[3], deadline)
