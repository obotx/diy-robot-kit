import unittest
import test_link
from test_link import Port, SerialPeer, ZERO, LinkError, State, encode_frame
from app.robot import BaseWheelJog, ServiceUnlock, Stop, Enable, ClearFault, ControlError, COMMAND


class WheelServiceTests(unittest.TestCase):
    def setUp(self):
        self.port = Port()
        self.addCleanup(self.port.close)
        self.port.command('bench')
        self.peer = SerialPeer(self.port, 'base', 'BASE-TEST', 'b' * 32)
        self.peer.hello()

    def test_four_wheels_stop_and_invalid_wheel(self):
        for wheel in ('FL', 'FR', 'RL', 'RR'):
            state = self.peer.wheel_setpoint(wheel, -.1)
            self.assertTrue(state.enabled and state.base.wheel_test_active)
            self.assertFalse(state.ready)
            self.assertEqual(state.base.encoder_counts, [0] * 4)
            self.assertFalse(self.peer.setpoint(False, ZERO).enabled)
        with self.assertRaises(LinkError):
            self.peer.wheel_setpoint('INVALID', .1)
        self.assertFalse(self.port.command('state')['session_valid'])

    def test_gateway_clear_base_preserves_service_selection(self):
        fixture = test_link.GatewayTests()
        fixture.setUp()
        self.addCleanup(fixture.cleanup)
        fixture.gateway._connect()
        fixture.ports['base'].command('bench')
        robot = fixture.robot
        robot.apply('web', ServiceUnlock(type='service_unlock', seq=1,
                    password='test-password', confirm_bypass=True))
        robot.apply('web', ClearFault(type='clear_fault', controller='base', seq=2))
        fixture.gateway._cycle()
        robot.tick()
        self.assertEqual(robot.service_client, 'web')
        self.assertIsNone(robot.owner)
        self.assertEqual(robot.reason, 'action_complete')
        states = fixture.gateway.status()['states']
        self.assertTrue(all(not state['enabled'] for state in states.values()))
        self.assertIsNone(states['base']['fault'])

    def test_clear_base_fault_stays_disabled(self):
        self.peer.wheel_setpoint('FR', .1)
        for now in range(200, 3000, 200):
            self.port.command(f'time {now}')
            self.peer.wheel_setpoint('FR', .1)
        self.port.command('time 3000')
        with self.assertRaises(LinkError):
            self.peer.wheel_setpoint('FR', .1)
        self.peer.hello()
        state = self.peer.clear_base_fault()
        self.assertFalse(state.enabled)
        self.assertIsNone(state.fault)
        self.assertFalse(self.peer.setpoint(False, ZERO).enabled)

    def test_clear_rejects_enabled_drive(self):
        self.peer.wheel_setpoint('FL', .1)
        with self.assertRaisesRegex(LinkError, 'release_before_clear'):
            self.peer.clear_base_fault()

    def test_limit_mode_change_replay_and_watchdog(self):
        for pwm in (.1501, -.1501):
            self.peer.hello()
            with self.assertRaises(LinkError): self.peer.wheel_setpoint('FR', pwm)
        self.peer.hello()
        self.peer.wheel_setpoint('FR', .1)
        with self.assertRaises(LinkError): self.peer.wheel_setpoint('RR', .1)
        self.peer.hello()
        self.peer.wheel_setpoint('FR', .1)
        with self.assertRaises(LinkError): self.peer.setpoint(True, ZERO)
        self.peer.hello()
        self.peer.wheel_setpoint('FR', .1)
        self.port.command('time 250')
        with self.assertRaisesRegex(LinkError, 'local_watchdog'): self.peer.wheel_setpoint('FR', .1)
        self.peer.hello()
        self.assertFalse(self.peer.setpoint(False, ZERO).enabled)

    def test_firmware_burst_limit_cannot_be_renewed(self):
        self.peer.wheel_setpoint('FR', .1)
        for now in range(200, 3000, 200):
            self.port.command(f'time {now}')
            self.peer.wheel_setpoint('FR', .1)
        self.port.command('time 3000')
        with self.assertRaisesRegex(LinkError, 'wheel_test_timeout'):
            self.peer.wheel_setpoint('FR', .1)
        self.peer.hello()
        self.assertFalse(self.peer.setpoint(False, ZERO).enabled)

    def test_gateway_routes_only_selected_base_and_requires_release(self):
        fixture = test_link.GatewayTests()
        fixture.setUp()
        self.addCleanup(fixture.cleanup)
        fixture.gateway._connect()
        fixture.ports['base'].command('bench')
        robot = fixture.robot
        unlock = ServiceUnlock(type='service_unlock', seq=1, password='test-password', confirm_bypass=True)
        robot.apply('web', unlock)
        def jog(pwm, wheel='FL'):
            robot.apply('web', BaseWheelJog(type='base_wheel_jog', seq=2, wheel=wheel, pwm=pwm))
        with self.assertRaisesRegex(ControlError, 'release_required'): jog(.1)
        jog(0.)
        jog(.1)
        fixture.gateway._cycle()
        state = fixture.gateway.status()['states']
        self.assertFalse(state['arm']['enabled'])
        self.assertTrue(state['base']['enabled'])
        self.assertFalse(state['base']['ready'])
        with self.assertRaises(ControlError):
            robot.apply('other', BaseWheelJog(type='base_wheel_jog', seq=3, wheel='RR', pwm=.1))
        with self.assertRaises(ControlError): jog(.1, 'RL')
        robot.apply('web', Stop(type='stop', seq=4))
        fixture.gateway._cycle()
        self.assertTrue(all(not s['enabled'] for s in fixture.gateway.status()['states'].values()))
        with self.assertRaisesRegex(ControlError, 'release_required'): jog(.1)
        with self.assertRaises(ControlError): robot.apply('web', Enable(type='enable', seq=5, enabled=True))

    def test_web_schema_rejects_missing_wheel_and_excess_pwm(self):
        for wheel, pwm in [('INVALID', .1), ('FR', .16), ('FR', float('nan'))]:
            with self.assertRaises(ValueError):
                COMMAND.validate_python(dict(type='base_wheel_jog', seq=1, wheel=wheel, pwm=pwm))

    def test_server_burst_expiry_and_disconnect_require_new_zero(self):
        fixture = test_link.GatewayTests()
        fixture.setUp()
        self.addCleanup(fixture.cleanup)
        fixture.gateway._connect()
        robot = fixture.robot
        clock = [robot.clock()]
        robot.clock = lambda: clock[0]
        robot.apply('web', ServiceUnlock(type='service_unlock', seq=1,
                    password='test-password', confirm_bypass=True))
        def jog(pwm):
            robot.apply('web', BaseWheelJog(type='base_wheel_jog', seq=2, wheel='FR', pwm=pwm))
        jog(0.)
        jog(.1)
        start = clock[0]
        for n in range(1, 20):
            clock[0] = start + n * .1
            jog(.1)
        clock[0] = start + 2.01
        with self.assertRaisesRegex(ControlError, 'release_required'): jog(.1)
        self.assertIsNone(robot.owner)
        self.assertFalse(fixture.gateway.intent[0])
        jog(0.)
        jog(.1)
        robot.disconnect('web')
        self.assertIsNone(robot.service_client)
        self.assertFalse(fixture.gateway.intent[0])
        robot.apply('web', ServiceUnlock(type='service_unlock', seq=3,
                    password='test-password', confirm_bypass=True))
        with self.assertRaisesRegex(ControlError, 'release_required'): jog(.1)
