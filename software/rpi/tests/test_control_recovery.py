import unittest
import time
from fastapi import FastAPI
from fastapi.testclient import TestClient
from app.control import router
from app.control_session import ControlSession, ControlSessions
from app.robot import RobotSimulator, ZERO
from app.stm32 import STM32Robot
from test_service import Gateway
from types import SimpleNamespace
from unittest.mock import AsyncMock


def receive(ws, predicate):
    for _ in range(100):
        data = ws.receive_json()
        if predicate(data):
            return data
    raise AssertionError("Missing expected WebSocket response")


class RecoveryTests(unittest.TestCase):
    def test_service_grant_recovers_without_password_and_stop_revokes_recovery(self):
        class ServiceRobot(STM32Robot):
            def snapshot(self):
                self.tick()
                return self.control_snapshot()
        app = FastAPI()
        app.include_router(router)
        robot = app.state.robot = ServiceRobot(Gateway())
        authorize = AsyncMock(return_value=True)
        app.state.service_access = SimpleNamespace(record=True, authorize=authorize)
        with TestClient(app) as client:
            with client.websocket_connect('/ws/control') as ws:
                ws.receive_json()
                ws.send_json(dict(type='session_resume', token=None))
                welcome = receive(ws, lambda d: d['type'] == 'welcome')
                owner, lease = welcome['client_id'], welcome['lease']
                ws.send_json(dict(type='service_unlock', seq=1, lease=lease,
                                  password='test-password', confirm_bypass=True))
                receive(ws, lambda d: d.get('seq') == 1)
                ws.send_json(dict(type='enable', enabled=True, seq=2, lease=lease))
                receive(ws, lambda d: d.get('seq') == 2)
            for _ in range(100):
                if not app.state.control_sessions.items[welcome['resume_token']].connected: break
                time.sleep(.01)
            with client.websocket_connect('/ws/control') as ws:
                ws.receive_json()
                ws.send_json(dict(type='session_resume', token=welcome['resume_token'], restore_control=False))
                resumed = receive(ws, lambda d: d['type'] == 'welcome')
                self.assertEqual(resumed['client_id'], owner)
                self.assertFalse(resumed['wanted'])
                self.assertEqual(robot.service_client, owner)
                self.assertIsNone(robot.owner)
                self.assertEqual(authorize.await_count, 1)
                robot.stop('operator')
                ws.send_json(dict(type='session_ready', lease=resumed['lease']))
                error = receive(ws, lambda d: d['type'] == 'error')
                self.assertEqual(error['code'], 'recovery_cancelled')
                self.assertIsNone(robot.owner)
                ws.send_json(dict(type='service_lock', seq=1, lease=resumed['lease']))
                receive(ws, lambda d: d.get('seq') == 1)
                self.assertIsNone(robot.service_client)
                self.assertFalse(app.state.control_sessions.items[resumed['resume_token']].service)

    def test_service_selection_outlives_timeout_but_not_motion(self):
        now = [0.0]
        sessions = ControlSessions(lambda: now[0])
        session = ControlSession('service-owner', service=True, wanted=True)
        sessions.add(session)
        sessions.detach(session, 2)
        now[0] = 3600
        sessions.add(ControlSession('observer'))
        recovered = sessions.resume(session.token, 7)
        self.assertIs(recovered, session)
        self.assertTrue(recovered.service)
        self.assertFalse(recovered.wanted)
        self.assertEqual(recovered.generation, 7)
        recovered.service = False
        sessions.detach(recovered, 7)
        now[0] += 31
        self.assertIsNone(sessions.resume(recovered.token, 7))

    def test_token_expiry_rotation_and_connected_takeover(self):
        now = [0.0]
        sessions = ControlSessions(lambda: now[0])
        session = ControlSession("owner")
        sessions.add(session)
        token = session.token
        self.assertIsNone(sessions.resume(token, 0))
        sessions.detach(session, 5)
        self.assertIsNone(sessions.resume(token, 6))
        self.assertIs(sessions.resume(token, 5), session)
        self.assertNotEqual(session.token, token)
        self.assertIsNone(sessions.resume(token, 5))
        sessions.detach(session, 5)
        now[0] = 31
        self.assertIsNone(sessions.resume(session.token, 5))

    def test_watchdog_stale_frames_and_reconnect(self):
        now = [0.0]
        app = FastAPI()
        app.include_router(router)
        robot = app.state.robot = RobotSimulator(lambda: now[0])
        with TestClient(app) as client:
            with client.websocket_connect('/ws/control') as ws:
                ws.receive_json()
                ws.send_json({'type': 'session_resume', 'token': None})
                welcome = receive(ws, lambda d: d['type'] == 'welcome')
                lease = welcome['lease']
                owner = welcome['client_id']
                token = welcome['resume_token']
                ws.send_json(dict(type='enable', enabled=True, seq=1, lease=lease))
                receive(ws, lambda d: d.get('seq') == 1)
                ws.send_json(dict(type='motion', seq=2, lease=lease, **(ZERO | {'x_mps': .1})))
                receive(ws, lambda d: d.get('seq') == 2)
                now[0] = .5
                robot.tick()
                self.assertEqual(robot.owner, owner)
                now[0] = .601
                robot.tick()
                paused = receive(ws, lambda d: d.get('control_session', {}).get('paused'))
                self.assertIsNone(robot.owner)
                self.assertEqual(robot.command, ZERO)
                new_lease = paused['control_session']['lease']
                self.assertNotEqual(lease, new_lease)
                ws.send_json(dict(type='motion', seq=3, lease=lease, **(ZERO | {'x_mps': .1})))
                ws.send_json(dict(type='session_ready', lease=new_lease))
                receive(ws, lambda d: d.get('owner') == owner)
                self.assertEqual(robot.command, ZERO)
                ws.send_json(dict(type='motion', seq=4, lease=lease, **(ZERO | {'x_mps': .1})))
                ws.send_json(dict(type='motion', seq=5, lease=new_lease, **ZERO))
                receive(ws, lambda d: d.get('seq') == 5)
                self.assertEqual(robot.command, ZERO)
            for _ in range(100):
                if robot.owner is None:
                    break
                time.sleep(.01)
            self.assertIsNone(robot.owner)
            with client.websocket_connect('/ws/control') as ws:
                ws.receive_json()
                ws.send_json(dict(type='session_resume', token=token))
                resumed = receive(ws, lambda d: d['type'] == 'welcome')
                self.assertEqual(resumed['client_id'], owner)
                self.assertTrue(resumed['wanted'])
                self.assertTrue(resumed['paused'])
                self.assertIsNone(robot.owner)
                ws.send_json(dict(type='session_ready', lease=resumed['lease']))
                receive(ws, lambda d: d.get('owner') == owner)
                self.assertEqual(robot.command, ZERO)
