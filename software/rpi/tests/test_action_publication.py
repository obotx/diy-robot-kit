import unittest
from types import SimpleNamespace
from unittest.mock import Mock

from app.stm32 import SerialGateway
from test_stm32 import config


class ActionPublicationTests(unittest.TestCase):
    def test_completion_has_new_stage_before_base_reply(self):
        gateway = SerialGateway(config(), clock=lambda: 0)
        gateway.connected = True
        gateway.states = {"arm": {"arm": {"reference_stage": 0}}}
        gateway.action = ("client:1", "level_center", {}, 1)
        completed = {"enabled": False, "arm": {"reference_stage": 1, "active_axis": None}}
        reply = Mock()
        reply.arm = SimpleNamespace(active_axis=None, reference_stage=1)
        reply.model_dump.return_value = completed
        arm = Mock()
        arm.arm_action.return_value = reply
        base = Mock()
        def read_base(*args):
            # A telemetry read can happen here while the worker waits for BASE.
            status = gateway.status()
            self.assertEqual(status["action_result"], ("client:1", "complete"))
            self.assertEqual(status["states"]["arm"]["arm"]["reference_stage"], 1)
            result = Mock()
            result.model_dump.return_value = {"enabled": False, "base": {}}
            return result
        base.setpoint.side_effect = read_base
        gateway.peers = {"arm": arm, "base": base}
        gateway._cycle()
        self.assertEqual(base.setpoint.call_count, 1)
