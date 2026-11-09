import threading
import unittest
from types import SimpleNamespace
from unittest.mock import Mock

from app.stm32 import SerialGateway
from app.robot import ZERO
from test_stm32 import Clock, config


class ActionIOTests(unittest.TestCase):
    def test_service_pair_sends_service_only_to_arm_and_stop_to_both(self):
        gateway = SerialGateway(config(), clock=Clock())
        gateway.connected = True
        arm, base = Mock(), Mock()
        arm.setpoint.return_value.model_dump.return_value = {"arm": {"service_active": True}}
        base.setpoint.return_value.model_dump.return_value = {"base": {}}
        gateway.peers = {"arm": arm, "base": base}
        command = ZERO | {"gripper": .5, "vx_mps": .1}
        gateway.submit(0, True, command, 1, service=True)
        gateway._cycle()
        self.assertEqual(arm.setpoint.call_args.args[:2], (True, ZERO))
        self.assertTrue(arm.setpoint.call_args.kwargs["service"])
        self.assertNotIn("service", base.setpoint.call_args.kwargs)
        self.assertEqual(base.setpoint.call_args.args[1]["vx_mps"], .1)
        gateway._cycle()
        self.assertEqual(arm.setpoint.call_args.args[1]["gripper"], .5)
        gateway.submit(0, False, ZERO, 0)
        gateway._cycle()
        for peer in (arm, base):
            self.assertFalse(peer.setpoint.call_args.args[0])
            self.assertEqual(peer.setpoint.call_args.args[1], ZERO)

    def test_pending_keepalive_allows_renewal_status_and_stop(self):
        gateway = SerialGateway(config(), clock=Clock())
        gateway.connected = True
        gateway.action = ("home-1", "home", {"axis": "ZL"}, .4)
        gateway.action_sent = "home-1"
        entered, release = threading.Event(), threading.Event()
        reply = Mock(arm=SimpleNamespace(active_axis=None, homed_axes={"ZL": True}))
        reply.model_dump.return_value = {"enabled": True}
        stopped = Mock()
        stopped.model_dump.return_value = {"enabled": False}
        def pending(*args):
            entered.set()
            if not release.wait(2):
                raise RuntimeError("test reply not released")
            return reply
        peer = Mock(arm_action=pending, setpoint=Mock(return_value=stopped))
        gateway.peers = {"arm": peer}
        errors = []
        def cycle():
            try:
                gateway._cycle()
            except Exception as exc:
                errors.append(exc)
        worker = threading.Thread(target=cycle)
        worker.start()
        try:
            self.assertTrue(entered.wait(1))
            acquired = gateway.lock.acquire(timeout=.1)
            if acquired:
                gateway.lock.release()
            self.assertTrue(acquired, "UART reply blocks the web event loop via the state lock")
            self.assertTrue(gateway.renew_action("home-1", .6))
            gateway.status()
            gateway.submit(gateway.epoch, False, ZERO, 0)
        finally:
            release.set()
            worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(errors, [])
        self.assertIsNone(gateway.action_result, "Late HOME reply must not report cancelled motion as complete")
        peer.setpoint.assert_called_once_with(False, ZERO)
        self.assertFalse(gateway.states["arm"]["enabled"])
