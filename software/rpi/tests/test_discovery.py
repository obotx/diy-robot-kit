import os
import threading
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from pydantic import ValidationError
from app.stm32 import SerialGateway, STM32Robot
from app.stm32_config import STM32Config, load_robot_settings
from app.stm32_discovery import candidates, discover
from app.stm32_protocol import LinkError, SerialPeer
from app.robot import Enable, ControlError
from test_stm32 import Clock, FakePort


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.config = STM32Config(discovery=True)
        self.paths = {"COM0": "base", "COM1": "arm"}
        self.opened = []
        self.change = lambda port: None

    def opener(self, board, config, backend):
        port = FakePort(self.paths[board.port], self.clock)
        self.change(port)
        self.opened.append(port)
        return port

    def discover(self):
        return discover(self.config, self.opener, self.clock, threading.Event(), lambda: list(self.paths))

    def test_role_from_firmware_not_port_order(self):
        boards = self.discover()
        self.assertEqual(boards["arm"].port, "COM1")
        self.assertEqual(boards["base"].port, "COM0")
        self.assertTrue(all(p.closed for p in self.opened))
        for port in self.opened:
            self.assertEqual(port.sent[0]["type"], "hello")
            self.assertFalse(any(frame.get("enabled", False) for frame in port.sent))

    def test_missing_and_duplicate_roles_fail_closed(self):
        for paths in ({}, {"COM0": "base"}, {"COM0": "arm", "COM1": "arm"}):
            self.paths = paths
            with self.assertRaises(LinkError):
                self.discover()

    def test_same_identity_for_two_roles_rejected(self):
        self.change = lambda p: setattr(p, "filter", lambda reply: reply | {"device_id": "same"})
        with self.assertRaisesRegex(LinkError, "ambiguous"):
            self.discover()

    def test_bad_firmware_blocks_pair_and_closes_ports(self):
        self.change = lambda p: setattr(p, "filter", lambda reply: reply | {"protocol": 99})
        with self.assertRaisesRegex(LinkError, "probe_failed"):
            self.discover()
        self.assertTrue(all(p.closed for p in self.opened))

    def test_supported_usb_only(self):
        ports = [SimpleNamespace(device="COM1", vid=1155, pid=14155),
                 SimpleNamespace(device="COM2", vid=123, pid=14155),
                 SimpleNamespace(device="COM3", vid=None, pid=None)]
        with patch("app.stm32_discovery.list_ports.comports", return_value=ports):
            self.assertEqual(candidates(), ["COM1"])

    def test_gateway_reopens_after_probe_and_reassigns_changed_port_numbers(self):
        gateway = SerialGateway(self.config, opener=self.opener, clock=self.clock, enumerator=lambda: list(self.paths))
        self.addCleanup(lambda: gateway._fail("test_done"))
        gateway._connect()
        self.assertTrue(gateway.status()["connected"])
        self.assertEqual(len(self.opened), 4)
        self.assertEqual(gateway.status()["ports"]["arm"], "COM1")
        robot = STM32Robot(gateway, self.clock)
        with self.assertRaisesRegex(ControlError, "commissioning_required"):
            robot.apply("pc", Enable(type="enable", seq=1, enabled=True))
        gateway._fail("unplugged")
        self.paths = {"COM8": "arm", "COM9": "base"}
        gateway._connect()
        self.assertEqual(gateway.status()["ports"]["arm"], "COM8")
        self.assertFalse(any(s["enabled"] for s in gateway.status()["states"].values()))

    def test_replacement_board_requires_restart(self):
        gateway = SerialGateway(self.config, opener=self.opener, clock=self.clock, enumerator=lambda: list(self.paths))
        self.addCleanup(lambda: gateway._fail("test_done"))
        gateway._connect()
        gateway._fail("unplugged")
        self.change = lambda p: setattr(p, "filter", lambda reply: reply | {"device_id": "replacement-" + p.board.role})
        with self.assertRaisesRegex(LinkError, "identity_changed"):
            gateway._connect()
        self.assertFalse(gateway.status()["connected"])

    def test_unknown_role_hello_pins_later_identity(self):
        port = FakePort("base", self.clock)
        peer = SerialPeer(port, None, None, "a" * 32, self.clock)
        self.assertEqual(peer.hello().role, "base")
        port.filter = lambda reply: reply | {"device_id": "replacement"}
        from app.robot import ZERO
        with self.assertRaisesRegex(LinkError, "identity_or_sequence"):
            peer.setpoint(False, ZERO)

    def test_auto_settings_ignore_old_manual_file_and_never_commission(self):
        with patch.dict(os.environ, {"OBOTX_ROBOT_BACKEND": "stm32-auto", "OBOTX_STM32_CONFIG": "obsolete-file"}):
            backend, config = load_robot_settings()
        self.assertEqual(backend, "stm32")
        self.assertTrue(config.discovery)
        self.assertFalse(config.commissioned)
        with self.assertRaises(ValidationError):
            STM32Config(discovery=True, commissioned=True)

    def test_bound_scan_and_cancellation(self):
        self.paths = {f"COM{i}": "arm" for i in range(9)}
        with self.assertRaisesRegex(LinkError, "too_many"):
            self.discover()
        self.assertEqual(self.opened, [])
        closing = threading.Event()
        closing.set()
        with self.assertRaisesRegex(LinkError, "shutdown"):
            discover(self.config, self.opener, self.clock, closing, lambda: ["COM0"])

    def test_worker_waits_for_hotplug_and_invalidates_changed_topology(self):
        self.paths = {}
        gateway = SerialGateway(self.config, opener=self.opener, clock=self.clock, enumerator=lambda: list(self.paths))
        gateway.start()
        self.addCleanup(gateway.close)

        def wait_for(predicate):
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if predicate():
                    return
                time.sleep(0.02)
            self.fail(str(gateway.status()))

        wait_for(lambda: "discovery_waiting" in (gateway.status()["error"] or ""))
        self.paths.update({"COM0": "base", "COM1": "arm"})
        wait_for(lambda: gateway.status()["connected"])
        epoch = gateway.status()["epoch"]
        self.paths.pop("COM1")
        self.clock.now += 1.1
        wait_for(lambda: gateway.status()["epoch"] > epoch)
        self.assertFalse(gateway.status()["connected"])
        self.paths["COM8"] = "arm"
        wait_for(lambda: gateway.status()["connected"])
        self.assertEqual(gateway.status()["ports"]["arm"], "COM8")
        self.assertTrue(all(not s["enabled"] for s in gateway.status()["states"].values()))

    def test_app_without_boards_stays_hardware_not_simulation(self):
        from fastapi.testclient import TestClient
        from app.main import app
        with patch.dict(os.environ, {"OBOTX_ROBOT_BACKEND": "stm32-auto", "OBOTX_HARDWARE": "mock"}), \
             patch("app.stm32_discovery.list_ports.comports", return_value=[]), TestClient(app) as client:
            state = client.get("/api/robot").json()
            self.assertEqual(state["backend"], "stm32")
            self.assertTrue(state["controllers"]["discovery"])
            self.assertFalse(state["hardware_connected"])
            self.assertFalse(state["control_ready"])
