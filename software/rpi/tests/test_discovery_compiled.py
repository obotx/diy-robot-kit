"""Opt-in discovery contract against existing compiled ARM/base endpoint fixtures."""
import importlib.util
import os
from pathlib import Path
import unittest

from app.stm32 import SerialGateway
from app.stm32_config import STM32Config


@unittest.skipUnless(os.getenv("ARM_LINK_FIXTURE") and os.getenv("BASE_LINK_FIXTURE"),
                     "Run stm32_base/tools/test-link.ps1 first in the same shell")
class CompiledDiscoveryTests(unittest.TestCase):
    def test_auto_discovery_with_production_c_and_reversed_ports(self):
        path = Path(__file__).resolve().parents[2] / "stm32_base/tests/test_link.py"
        spec = importlib.util.spec_from_file_location("compiled_base_fixture", path)
        fixture = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fixture)
        paths = {"VCP-0": "base", "VCP-1": "arm"}
        opened = []

        def opener(board, config, backend):
            port = fixture.Port(paths[board.port])
            opened.append(port)
            return port

        gateway = SerialGateway(STM32Config(discovery=True), opener=opener,
                                enumerator=lambda: list(paths))
        try:
            gateway._connect()
            state = gateway.status()
            self.assertTrue(state["connected"])
            self.assertEqual(state["ports"], {"base": "VCP-0", "arm": "VCP-1"})
            self.assertEqual(state["identities"]["arm"]["firmware"], "arm-link-1")
            self.assertEqual(state["identities"]["base"]["firmware"], "base-link-1")
            self.assertTrue(all(not s["enabled"] for s in state["states"].values()))
            gateway._fail("unplugged")
            paths.clear()
            paths.update({"VCP-8": "arm", "VCP-4": "base"})
            gateway._connect()
            self.assertEqual(gateway.status()["ports"]["arm"], "VCP-8")
            self.assertTrue(all(not s["enabled"] for s in gateway.status()["states"].values()))
        finally:
            gateway._fail("test_done")
            for port in opened:
                port.close()
