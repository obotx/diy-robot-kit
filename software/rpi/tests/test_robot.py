import json
import os
import unittest
from unittest.mock import patch

from fastapi.testclient import TestClient
from pydantic import ValidationError
from starlette.websockets import WebSocketDisconnect

from app.main import app
from app.robot import COMMAND, ZERO, ControlError, Enable, Motion, RobotSimulator


class FakeClock:
    now = 0.0

    def __call__(self):
        return self.now


def move(seq=2, **changes):
    return Motion(type="motion", seq=seq, **(ZERO | changes))


class RobotTests(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        self.robot = RobotSimulator(self.clock)

    def enable(self, owner="browser"):
        self.robot.apply(owner, Enable(type="enable", seq=1, enabled=True))

    def test_disabled_until_claimed_and_only_one_owner(self):
        with self.assertRaises(ControlError):
            self.robot.apply("browser", move(vx_mps=0.1))
        self.enable()
        with self.assertRaises(ControlError):
            self.enable("pc")
        self.assertEqual(self.robot.owner, "browser")

    def test_grouped_z_moves_both_and_release_stops(self):
        self.enable()
        self.robot.apply("browser", move(z_mps=0.05))
        self.clock.now += 0.1
        state = self.robot.snapshot()
        self.assertEqual(state["arm"]["z_left_mm"], 350.5)
        self.assertEqual(state["arm"]["z_left_mm"], state["arm"]["z_right_mm"])
        self.robot.apply("browser", move(seq=3))
        self.clock.now += 0.1
        self.assertEqual(self.robot.snapshot()["arm"]["z_left_mm"], 351)
        self.clock.now += 0.1
        self.assertEqual(self.robot.snapshot()["arm"]["z_left_mm"], 351)

    def test_watchdog_stops_and_requires_new_enable(self):
        self.enable()
        self.robot.apply("browser", move(vx_mps=0.1, x_mps=0.1, gripper=1.0))
        self.clock.now += 0.601
        state = self.robot.snapshot()
        self.assertFalse(state["enabled"])
        self.assertEqual(state["command"], ZERO)
        self.assertEqual(state["reason"], "watchdog")
        with self.assertRaises(ControlError):
            self.robot.apply("browser", move(seq=3, vx_mps=0.1))

    def test_repeated_enable_cannot_refresh_stale_motion(self):
        self.enable()
        self.robot.apply("browser", move(vx_mps=0.1))
        self.clock.now += 0.3
        self.enable()
        self.clock.now += 0.31
        self.assertFalse(self.robot.snapshot()["enabled"])

    def test_watchdog_stop_remains_in_log_after_later_release(self):
        self.enable()
        with self.assertLogs("app.robot", level="WARNING") as log:
            self.clock.now += .601
            self.robot.tick()
            self.robot.apply("browser", Enable(type="enable", seq=2, enabled=False,
                                               source="browser:window_blur"))
        self.assertEqual(self.robot.reason, "released")
        self.assertEqual(len(log.output), 1)
        self.assertIn("reason=watchdog", log.output[0])

    def test_observer_disconnect_does_not_stop_owner(self):
        self.enable()
        self.robot.disconnect("pc")
        self.assertEqual(self.robot.owner, "browser")
        self.robot.disconnect("browser")
        self.assertIsNone(self.robot.owner)

    def test_limits_and_nonfinite_rejected(self):
        for changes in ({"vx_mps": 9}, {"z_mps": float("nan")}, {"x_mps": float("inf")}, {"gripper": True}):
            with self.subTest(changes=changes), self.assertRaises(ValidationError):
                COMMAND.validate_json(json.dumps({"type": "motion", "seq": 2, **ZERO, **changes}))
        with self.assertRaises(ValidationError):
            COMMAND.validate_json('{"type":"motion","seq":2,"vx_mps":0.1}')

    def test_vector_limit_stops_owner(self):
        self.enable()
        with self.assertRaises(ControlError):
            self.robot.apply("browser", move(vx_mps=0.2, vy_mps=0.2))
        self.assertIsNone(self.robot.owner)

    def test_simulated_travel_is_clamped(self):
        self.enable()
        self.robot.z_left_mm = 999.9
        self.robot.z_right_mm = 999.9
        self.robot.x_mm = 999.9
        self.robot.gripper_pct = 99.9
        self.robot.apply("browser", move(z_mps=0.05, x_mps=0.1, gripper=1.0))
        self.clock.now += 0.1
        state = self.robot.snapshot()
        self.assertEqual(state["arm"]["z_left_mm"], 1000)
        self.assertEqual(state["arm"]["x_mm"], 1000)
        self.assertEqual(state["gripper"]["opening_pct"], 100)

    def test_independent_z_does_not_move_other_support(self):
        self.enable()
        self.robot.apply("browser", move(zl_mps=0.05))
        self.clock.now += 0.1
        state = self.robot.snapshot()
        self.assertEqual(state["arm"]["z_left_mm"], 350.5)
        self.assertEqual(state["arm"]["z_right_mm"], 350)
        self.robot.apply("browser", move(seq=3, zr_mps=-0.05))
        self.clock.now += 0.1
        state = self.robot.snapshot()
        self.assertEqual(state["arm"]["z_left_mm"], 351)
        self.assertEqual(state["arm"]["z_right_mm"], 349.5)

    def test_grouped_z_preserves_tilt_at_limits(self):
        self.enable()
        self.robot.z_left_mm = 999.8
        self.robot.z_right_mm = 950
        self.robot.apply("browser", move(z_mps=0.05))
        self.clock.now += 0.1
        state = self.robot.snapshot()
        self.assertEqual(state["arm"]["z_left_mm"], 1000)
        self.assertEqual(state["arm"]["z_right_mm"], 950.2)
        self.clock.now += 0.1
        self.assertEqual(self.robot.snapshot()["arm"]["z_right_mm"], 950.2)

    def test_conflicting_group_and_individual_z_stop_owner(self):
        self.enable()
        with self.assertRaisesRegex(ControlError, "conflicting_z_commands"):
            self.robot.apply("browser", move(z_mps=0.02, zl_mps=0.01))
        self.assertIsNone(self.robot.owner)
        self.assertEqual(self.robot.command, ZERO)

    def test_old_motion_messages_default_independent_z_to_zero(self):
        data = {"type": "motion", "seq": 1, **ZERO}
        del data["zl_mps"]
        del data["zr_mps"]
        command = COMMAND.validate_json(json.dumps(data))
        self.assertEqual(command.zl_mps, 0)
        self.assertEqual(command.zr_mps, 0)


def receive_kind(ws, kind):
    for _ in range(30):
        message = ws.receive_json()
        if message["type"] == kind:
            return message
    raise AssertionError(f"Missing {kind}")


class WebSocketTests(unittest.TestCase):
    def setUp(self):
        self.env = patch.dict(os.environ, {"OBOTX_HARDWARE": "mock"})
        self.env.start()
        self.client = TestClient(app).__enter__()

    def tearDown(self):
        self.client.__exit__(None, None, None)
        self.env.stop()

    def test_pc_control_observer_stop_and_disconnect(self):
        with self.client.websocket_connect("/ws/control") as pc:
            welcome = receive_kind(pc, "welcome")
            pc.send_json({"type": "enable", "enabled": True, "seq": 1, "source": "pc-test"})
            self.assertEqual(receive_kind(pc, "ack")["seq"], 1)
            pc.send_json(move(vx_mps=0.1).model_dump())
            receive_kind(pc, "ack")
            state = self.client.get("/api/robot").json()
            self.assertEqual(state["owner"], welcome["client_id"])
            self.assertEqual(state["command"]["vx_mps"], 0.1)
            with self.client.websocket_connect("/ws/control") as observer:
                receive_kind(observer, "welcome")
                observer.send_json({"type": "enable", "enabled": True, "seq": 1})
                self.assertEqual(receive_kind(observer, "error")["code"], "busy")
                self.assertTrue(self.client.get("/api/robot").json()["enabled"])
                observer.send_json({"type": "stop", "seq": 2})
                receive_kind(observer, "ack")
                self.assertFalse(self.client.get("/api/robot").json()["enabled"])

    def test_stale_sequence_stops_owner(self):
        with self.client.websocket_connect("/ws/control") as ws:
            receive_kind(ws, "welcome")
            ws.send_json({"type": "enable", "seq": 1, "enabled": True})
            receive_kind(ws, "ack")
            ws.send_json(move(seq=1).model_dump())
            self.assertEqual(receive_kind(ws, "error")["code"], "stale_sequence")
            self.assertFalse(self.client.get("/api/robot").json()["enabled"])

    def test_malformed_message_stops_owner(self):
        with self.client.websocket_connect("/ws/control") as ws:
            receive_kind(ws, "welcome")
            ws.send_json({"type": "enable", "seq": 1, "enabled": True})
            receive_kind(ws, "ack")
            ws.send_text("not-json")
            self.assertEqual(receive_kind(ws, "error")["code"], "invalid_command")
            self.assertFalse(self.client.get("/api/robot").json()["enabled"])

    def test_browser_origin_check(self):
        with self.assertRaises(WebSocketDisconnect):
            with self.client.websocket_connect("/ws/control", headers={"origin": "http://another-host"}):
                pass
        with self.client.websocket_connect("/ws/control", headers={"origin": "http://testserver"}) as ws:
            self.assertEqual(receive_kind(ws, "welcome")["protocol"], 1)

    def test_legacy_status_and_new_page(self):
        self.assertEqual(self.client.get("/api/status").status_code, 200)
        self.assertIn("camera-grid", self.client.get("/").text)

    def test_home_zl_pc_lease_and_observer_stop(self):
        with self.client.websocket_connect("/ws/control") as ws:
            receive_kind(ws, "welcome")
            ws.send_json({"type": "home", "seq": 1, "axis": "ZL"})
            receive_kind(ws, "ack")
            self.assertEqual(self.client.get("/api/robot").json()["homing"]["active_axis"], "ZL")
            ws.send_json({"type": "home_keepalive", "seq": 2})
            receive_kind(ws, "ack")
            with self.client.websocket_connect("/ws/control") as other:
                receive_kind(other, "welcome")
                other.send_json({"type": "home_keepalive", "seq": 1})
                self.assertEqual(receive_kind(other, "error")["code"], "not_homing_owner")
                self.assertTrue(self.client.get("/api/robot").json()["enabled"])
                other.send_json({"type": "stop", "seq": 2})
                receive_kind(other, "ack")
                self.assertIsNone(self.client.get("/api/robot").json()["homing"]["active_axis"])


if __name__ == "__main__":
    unittest.main()
