import unittest
import threading
from unittest.mock import AsyncMock
from types import SimpleNamespace
from fastapi import FastAPI
from fastapi.testclient import TestClient
from app.control import router
from app.robot import RobotController, ServiceUnlock


class Robot(RobotController):
    def __init__(self):
        super().__init__()
        self.unlocks = 0
        self.disconnected = threading.Event()
    def disconnect(self, client_id):
        super().disconnect(client_id)
        self.disconnected.set()
    def snapshot(self):
        return self.control_snapshot()
    def apply(self, client_id, message):
        if isinstance(message, ServiceUnlock):
            self.unlocks += 1
            self.owner = client_id
        else:
            super().apply(client_id, message)


class ServiceSocketTests(unittest.TestCase):
    def test_password_required_and_never_echoed_and_disconnect_stops(self):
        app = FastAPI()
        app.include_router(router)
        app.state.robot = robot = Robot()
        app.state.service_access = SimpleNamespace(record=True, authorize=AsyncMock(return_value=False))
        with TestClient(app) as client:
            with client.websocket_connect("/ws/control") as ws:
                ws.receive_json()
                payload = dict(type="service_unlock", seq=1, password="example-service-secret", confirm_bypass=True)
                ws.send_json(payload)
                while True:
                    data = ws.receive_json()
                    self.assertNotIn("example-service-secret", str(data))
                    if data["type"] == "error": break
                self.assertEqual(robot.unlocks, 0)
                app.state.service_access.authorize.return_value = True
                ws.send_json(payload | {"seq": 2})
                while ws.receive_json()["type"] != "ack": pass
                self.assertEqual(robot.unlocks, 1)
            self.assertTrue(robot.disconnected.wait(1))
            self.assertIsNone(robot.owner)

    def test_global_stop_cancels_inflight_password_check(self):
        app = FastAPI()
        app.include_router(router)
        app.state.robot = robot = Robot()
        async def authorize(_):
            robot.stop("operator")
            return True
        app.state.service_access = SimpleNamespace(record=True, authorize=authorize)
        with TestClient(app) as client:
            with client.websocket_connect("/ws/control") as ws:
                ws.receive_json()
                ws.send_json(dict(type="service_unlock", seq=1, password="example-service-secret", confirm_bypass=True))
                while True:
                    data = ws.receive_json()
                    if data["type"] == "error": break
                self.assertEqual(data["code"], "service_unlock_cancelled")
                self.assertEqual(robot.unlocks, 0)
