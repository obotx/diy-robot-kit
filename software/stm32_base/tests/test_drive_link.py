import unittest
import test_link
from test_link import Port, SerialPeer, ZERO, LinkError


class DriveLinkTests(unittest.TestCase):
    def test_enabled_contract_stop_and_watchdog(self):
        port = Port()
        self.addCleanup(port.close)
        port.command('drive')  # Explicit host-only calibration, never board default.
        peer = SerialPeer(port, 'base', 'BASE-TEST', 'a' * 32)
        peer.hello()
        self.assertTrue(peer.setpoint(False, ZERO).ready)
        state = peer.setpoint(True, dict(ZERO, vx_mps=.1))
        self.assertTrue(state.accepted and state.enabled and state.ready)
        self.assertIsNone(state.base.x_m)
        self.assertFalse(peer.setpoint(False, ZERO).enabled)
        peer.setpoint(True, ZERO)
        port.command('time 250')
        with self.assertRaisesRegex(LinkError, 'local_watchdog'):
            peer.setpoint(True, ZERO)
        peer.hello()
        self.assertFalse(peer.setpoint(False, ZERO).enabled)

    def test_current_gateway_two_endpoints(self):
        # Reuse the existing compiled ARM fixture; do not rebuild ARM firmware.
        fixture = test_link.GatewayTests()
        fixture.setUp()
        try:
            fixture.gateway._connect()
            fixture.gateway._cycle()
            state = fixture.robot.snapshot()
            self.assertTrue(state['hardware_connected'])
            self.assertFalse(state['outputs_enabled'])
            self.assertFalse(state['control_ready'])
            self.assertTrue(all(v is None for v in state['base'].values()))
        finally:
            fixture.cleanup()
