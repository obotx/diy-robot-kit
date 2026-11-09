"""Production C endpoint + actual SerialPeer, SerialGateway and STM32Robot.

Only UART/clock/flash are host fixtures. No Python protocol emulator is used.
"""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'software/rpi'))
from app.robot import Acceleration, ControlError, Enable, Stop, ZERO
from app.stm32 import SerialGateway, STM32Robot
from app.stm32_config import STM32Config
from app.stm32_protocol import Identity, LinkError, SerialPeer, State, encode_frame


class Port:
    """Bounded pipe transport: a missing C reply must not hang the test runner."""
    def __init__(self, role='base'):
        self.process = subprocess.Popen([os.environ[f'{role.upper()}_LINK_FIXTURE']],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.lines = queue.Queue()
        self.pending = bytearray()
        self.sent = []
        self.fail = False
        self.closed = False
        self.reader = threading.Thread(target=self._reader, daemon=True)
        self.reader.start()

    def _reader(self):
        for line in iter(self.process.stdout.readline, b''):
            self.lines.put(line)

    def write(self, raw):
        if self.fail:
            raise OSError('fixture cable unplugged')
        self.sent.append(raw)
        self.process.stdin.write(raw)
        self.process.stdin.flush()
        return len(raw)

    def read(self, count):
        if not self.pending:
            try:
                self.pending.extend(self.lines.get(timeout=0.01))
            except queue.Empty:
                return b''
        result = bytes(self.pending[:count])
        del self.pending[:count]
        return result

    def raw(self, raw):
        self.write(raw)
        line = bytearray()
        until = time.monotonic() + 2
        while time.monotonic() < until:
            line.extend(self.read(1))
            if line.endswith(b'\n'):
                return None if line == b'NONE\n' else json.loads(line)
        raise AssertionError(f'C fixture reply timeout (exit={self.process.poll()})')

    def command(self, command):
        return self.raw(b'@' + command.encode() + b'\n')

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.process.stdin.close()
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.reader.join(timeout=2)
        self.process.stdout.close()
        self.process.stderr.close()


class LinkTests(unittest.TestCase):
    def setUp(self):
        self.port = Port()
        self.addCleanup(self.port.close)
        self.peer = SerialPeer(self.port, 'base', 'BASE-TEST', 'a' * 32)
        self.identity = self.peer.hello()

    def packet(self, **changes):
        packet = self.peer.request('setpoint', enabled=False,
            command=dict(vx_mps=0.0, vy_mps=0.0, wz_radps=0.0),
            acceleration=dict(base_mps2=0.3, rotation_radps2=0.6))
        packet.update(changes)
        return packet

    def send(self, packet):
        reply = self.port.raw(encode_frame(packet))
        if reply is not None:
            State.model_validate(reply)
        return reply

    def test_identity_and_gateway_disabled_poll(self):
        self.assertEqual(self.identity.role, 'base')
        self.assertEqual(self.identity.firmware, 'base-mdd10a-5')
        self.assertEqual(self.identity.watchdog_ms, 250)
        self.assertIsNone(self.identity.max_z_difference_mm)
        for accel in [Acceleration(), Acceleration(base_mps2=0.01, rotation_radps2=0.05),
                      Acceleration(base_mps2=1.0, rotation_radps2=2.0)]:
            reply = self.peer.setpoint(False, ZERO, accel)
            self.assertTrue(reply.accepted)
            self.assertFalse(reply.enabled)
            self.assertFalse(reply.ready)
            self.assertIsNone(reply.fault)
            self.assertIsNone(reply.arm)
            self.assertTrue(all(getattr(reply.base, k) is None for k in ("x_m", "y_m", "yaw_rad")))

    def test_enable_is_rejected_even_with_zero_command(self):
        for command in [ZERO, dict(ZERO, vx_mps=0.1), dict(ZERO, wz_radps=-0.2)]:
            self.peer.hello()
            with self.assertRaises(LinkError):
                self.peer.setpoint(True, command)
            self.assertFalse(self.port.command('state')['session_valid'])
        self.peer.hello()
        reply = self.send(self.packet(enabled=True))
        self.assertEqual(reply['fault'], 'motion_not_supported')
        self.assertFalse(reply['accepted'])
        self.assertFalse(reply['enabled'])
        self.assertFalse(reply['ready'])

    def test_watchdog_at_250_requires_new_hello(self):
        self.peer.setpoint(False, ZERO)
        self.assertTrue(self.port.command('time 249')['session_valid'])
        state = self.port.command('time 250')
        self.assertFalse(state['session_valid'])
        self.assertTrue(state['expired'])
        self.assertEqual(self.send(self.packet())['fault'], 'local_watchdog')
        self.peer.hello()
        self.assertTrue(self.peer.setpoint(False, ZERO).accepted)

    def test_isr_alone_invalidates_session_without_parser(self):
        self.peer.setpoint(False, ZERO)
        self.assertTrue(self.port.command('isr 249')['session_valid'])
        state = self.port.command('isr 250')
        self.assertFalse(state['session_valid'])
        self.assertTrue(state['expired'])
        self.assertFalse(self.send(self.packet())['accepted'])

    def test_watchdog_wraparound_and_refresh(self):
        self.port.command('time 4294967200')
        self.peer.setpoint(False, ZERO)
        self.assertTrue(self.port.command('isr 153')['session_valid'])
        self.assertFalse(self.port.command('isr 154')['session_valid'])
        self.peer.hello()
        self.peer.setpoint(False, ZERO)
        self.port.command('time 350')
        self.peer.setpoint(False, ZERO)
        self.assertTrue(self.port.command('isr 599')['session_valid'])
        self.assertFalse(self.port.command('isr 600')['session_valid'])

    def test_hello_never_refreshes_watchdog_and_replaces_session(self):
        self.peer.setpoint(False, ZERO)
        self.port.command('time 100')
        self.peer.hello()
        state = self.port.command('state')
        self.assertEqual(state['last_ms'], 0)
        self.assertFalse(state['armed'])
        new = SerialPeer(self.port, 'base', 'BASE-TEST', 'b' * 32)
        new.hello()
        self.assertTrue(new.setpoint(False, ZERO).accepted)
        self.assertEqual(self.send(self.packet())['fault'], 'invalid_session')

    def test_replay_lower_sequence_and_gaps(self):
        first = self.packet(seq=50)
        self.assertTrue(self.send(first)['accepted'])
        self.port.command('time 200')
        self.assertEqual(self.send(first)['fault'], 'invalid_sequence')
        state = self.port.command('state')
        self.assertEqual(state['last_ms'], 0)
        self.assertFalse(state['session_valid'])
        self.peer.hello()
        self.assertTrue(self.send(self.packet(seq=9007199254740991))['accepted'])
        self.assertEqual(self.send(self.packet(seq=9007199254740990))['fault'], 'invalid_sequence')

    def test_device_id_stable_boot_changes_on_reset(self):
        self.peer.setpoint(False, ZERO)
        self.port.command('reset')
        with self.assertRaisesRegex(LinkError, 'controller_rebooted'):
            self.peer.setpoint(False, ZERO)
        fresh = SerialPeer(self.port, 'base', 'BASE-TEST', 'b' * 32)
        identity = fresh.hello()
        self.assertEqual(identity.device_id, self.identity.device_id)
        self.assertNotEqual(identity.boot_id, self.identity.boot_id)
        self.assertTrue(fresh.setpoint(False, ZERO).accepted)

    def test_invalid_semantics_do_not_refresh_or_preserve_session(self):
        mutations = [
            lambda p: p.update(enabled=1), lambda p: p.update(enabled='false'),
            lambda p: p.update(type='status'), lambda p: p.update(session='b' * 32),
            lambda p: p['command'].update(vx_mps=0.01),
            lambda p: p['command'].update(vx_mps=0.3, vy_mps=0.3),
            lambda p: p['command'].update(wz_radps=0.600001),
            lambda p: p['command'].update(vx_mps=True),
            lambda p: p['command'].update(vy_mps='0.0'),
            lambda p: p['command'].pop('vx_mps'),
            lambda p: p['command'].update(x_mps=0),
            lambda p: p['acceleration'].pop('base_mps2'),
            lambda p: p['acceleration'].update(base_mps2=0),
            lambda p: p['acceleration'].update(base_mps2=1.000001),
            lambda p: p['acceleration'].update(rotation_radps2=0.049),
            lambda p: p['acceleration'].update(rotation_radps2=2.001),
            lambda p: p['acceleration'].update(rotation_radps2=True),
            lambda p: p.update(command=[]), lambda p: p.update(acceleration=None),
            lambda p: p.update(role='arm'),
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.peer.hello()
                self.peer.setpoint(False, ZERO)
                packet = self.packet()
                mutation(packet)
                reply = self.send(packet)
                self.assertTrue(reply is None or not reply['accepted'])
                self.assertFalse(self.port.command('state')['session_valid'])

    def test_strict_json_and_envelope(self):
        packet = encode_frame(self.packet(seq=2))
        variants = [
            packet.replace(b'"seq":2', b'"seq":2,"seq":3'),
            packet.replace(b'"vx_mps":0.0', b'"vx_mps":0,"vx_mps":0'),
            packet.replace(b'"base_mps2":0.3', b'"base_mps2":0.3,"base_mps2":0.4'),
            packet.replace(b'"vx_mps"', b'"vx_\\u006dps"'),
            packet.replace(b'"seq":2', b'"seq":9007199254740992'),
            packet.replace(b'"seq":2', b'"seq":2.0'),
            packet.replace(b'"seq":2', b'"seq":2e0'),
            packet.replace(b'"seq":2', b'"seq":-1'),
            packet.replace(b'"protocol":1', b'"protocol":1.0'),
            packet.replace(b'"protocol":1', b'"protocol":true'),
            packet.replace(b'"protocol":1', b'"protocol":2'),
            packet.replace(b'a' * 32, b'A' * 32),
            packet.replace(b'a' * 32, b'a' * 31),
            packet.replace(b'"vx_mps":0.0', b'"vx_mps":NaN'),
            packet.replace(b'"vx_mps":0.0', b'"vx_mps":1e999'),
            packet.replace(b'"vx_mps":0.0', b'"vx_mps":1e-999'),
            packet.replace(b'"vx_mps":0.0', b'"vx_mps":+1'),
            packet.replace(b'"vx_mps":0.0', b'"vx_mps":01'),
            packet.replace(b'setpoint', b'set\x00point'),
            packet.replace(b'setpoint', b'set\xffpoint'),
            packet[:-2] + b',}\n', packet[:-1] + b'{}\n', b'[]\n', b'HB\n',
        ]
        for raw in variants:
            with self.subTest(raw=raw):
                self.peer.hello()
                reply = self.port.raw(raw)
                self.assertTrue(reply is None or not reply['accepted'])
                self.assertFalse(self.port.command('state')['session_valid'])

    def test_frame_boundary_fragmentation_concatenation(self):
        packet = encode_frame(self.packet())
        self.assertTrue(self.port.raw(b' ' * (2048 - len(packet)) + packet)['accepted'])
        packet = encode_frame(self.packet())
        self.assertIsNone(self.port.raw(b' ' * (2049 - len(packet)) + packet))
        self.assertFalse(self.port.command('state')['session_valid'])
        self.peer.hello()
        first, second = self.packet(), self.packet()
        for byte in encode_frame(first):
            self.port.write(bytes([byte]))
        line = self.port.lines.get(timeout=2)
        self.assertEqual(State.model_validate_json(line).seq, first['seq'])
        third = self.packet()
        self.port.write(encode_frame(second) + encode_frame(third))
        for expected in (second, third):
            line = self.port.lines.get(timeout=2)
            self.assertEqual(State.model_validate_json(line).seq, expected['seq'])

    def test_partial_frame_timeout_and_reply_overflow(self):
        self.port.command('partial')
        self.assertEqual(self.port.command('time 249')['rx_len'], 1)
        state = self.port.command('time 250')
        self.assertEqual(state['rx_len'], 0)
        self.assertTrue(state['dropping'])
        self.assertIsNone(self.port.raw(b'\n'))
        self.peer.hello()
        self.assertTrue(self.peer.setpoint(False, ZERO).accepted)
        self.port.command('tiny')
        self.assertIsNone(self.send(self.packet()))
        self.assertFalse(self.port.command('state')['session_valid'])


class GatewayTests(unittest.TestCase):
    def setUp(self):
        self.ports = {}
        self.opened = []
        def opener(board, config, backend):
            role = 'base' if board.port == 'COM-BASE' else 'arm'
            port = Port(role)
            self.ports[role] = port
            self.opened.append(port)
            # Remove synthetic homing supplied by the existing arm test fixture.
            if role == 'arm':
                port.command('unhome')
            return port
        self.config = STM32Config.model_validate(dict(
            arm=dict(port='COM-ARM', device_id='ARM-TEST'),
            base=dict(port='COM-BASE', device_id='BASE-TEST'),
            commissioned=False, max_z_difference_mm=None))
        self.gateway = SerialGateway(self.config, opener=opener)
        self.robot = STM32Robot(self.gateway)
        self.addCleanup(self.cleanup)

    def cleanup(self):
        self.gateway.close()
        for port in self.opened:
            port.close()

    def test_two_compiled_controllers_and_web_telemetry(self):
        self.gateway._connect()
        for _ in range(10):
            self.gateway._cycle()
        state = self.robot.snapshot()
        self.assertTrue(state['hardware_connected'])
        self.assertFalse(state['control_ready'])
        self.assertFalse(state['outputs_enabled'])
        self.assertTrue(all(value is None for value in state['base'].values()))
        self.assertIsNone(state['arm']['x_mm'])
        self.assertFalse(state['arm']['homed'])
        for role in ('arm', 'base'):
            controller = state['controllers']['states'][role]
            self.assertFalse(controller['ready'])
            self.assertFalse(controller['enabled'])
            self.assertTrue(controller['accepted'])
        with self.assertRaisesRegex(ControlError, 'commissioning_required'):
            self.robot.apply('web', Enable(type='enable', seq=1, enabled=True))
        self.config.commissioned = True
        self.config.max_z_difference_mm = 2.0
        with self.assertRaisesRegex(ControlError, 'controller_not_ready'):
            self.robot.apply('web', Enable(type='enable', seq=2, enabled=True))
        self.robot.apply('web', Stop(type='stop', seq=3))
        self.gateway._cycle()
        for port in self.ports.values():
            self.assertFalse(json.loads(port.sent[-1])['enabled'])

    def test_base_reject_causes_pair_failure_and_disabled_reconnect(self):
        self.gateway._connect()
        base = self.gateway.peers['base']
        packet = base.request('setpoint', enabled=True,
            command=dict(vx_mps=0.0, vy_mps=0.0, wz_radps=0.0),
            acceleration=dict(base_mps2=0.3, rotation_radps2=0.6))
        self.assertFalse(base.exchange(packet, State).accepted)
        with self.assertRaises(LinkError):
            self.gateway._cycle()
        self.gateway._fail('controller_rejected_or_faulted')
        for port in self.ports.values():
            self.assertFalse(json.loads(port.sent[-1])['enabled'])
        self.assertFalse(self.robot.snapshot()['hardware_connected'])
        self.gateway._connect()
        self.assertTrue(self.robot.snapshot()['hardware_connected'])
        self.assertFalse(self.robot.snapshot()['outputs_enabled'])

    def test_unplug_and_restart_pair_never_enables(self):
        self.gateway._connect()
        old_epoch = self.gateway.epoch
        self.ports['base'].fail = True
        with self.assertRaises(OSError):
            self.gateway._cycle()
        self.gateway._fail('unplugged')
        self.assertFalse(json.loads(self.ports['arm'].sent[-1])['enabled'])
        self.assertTrue(all(v is None for v in self.robot.snapshot()['base'].values()))
        self.gateway._connect()
        with self.assertRaises(ControlError):
            self.gateway.submit(old_epoch, True, ZERO, time.monotonic() + 1)
        self.assertFalse(self.robot.snapshot()['outputs_enabled'])
        self.ports['base'].command('reset')
        with self.assertRaisesRegex(LinkError, 'controller_rebooted'):
            self.gateway._cycle()

    def test_base_local_timeout_fails_real_gateway(self):
        self.gateway._connect()
        self.ports['base'].command('isr 250')
        with self.assertRaises(LinkError):
            self.gateway._cycle()

    def test_roles_and_device_ids_checked_by_real_gateway(self):
        for role, attribute, value in [('base', 'device_id', 'WRONG'),
                                       ('base', 'port', 'COM-OTHER')]:
            with self.subTest(attribute=attribute):
                original = getattr(getattr(self.config, role), attribute)
                setattr(getattr(self.config, role), attribute, value)
                with self.assertRaisesRegex(LinkError, 'identity_or_sequence_mismatch'):
                    self.gateway._connect()
                self.gateway._fail('identity_mismatch')
                setattr(getattr(self.config, role), attribute, original)

    def test_real_worker_polling_and_shutdown(self):
        self.gateway.start()
        until = time.monotonic() + 3
        while not self.gateway.status()['connected'] and time.monotonic() < until:
            time.sleep(0.01)
        self.assertTrue(self.gateway.status()['connected'])
        time.sleep(0.35)
        self.assertTrue(self.gateway.status()['connected'])
        self.assertFalse(self.robot.snapshot()['outputs_enabled'])
        self.gateway.close()
        self.assertFalse(self.gateway.thread.is_alive())
        for port in self.opened:
            self.assertFalse(json.loads(port.sent[-1])['enabled'])


if __name__ == '__main__':
    unittest.main()
