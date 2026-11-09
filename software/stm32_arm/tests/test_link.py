"""Exercise compiled STM32 C against the actual RPi protocol, not a reimplementation.

ARM_LINK_FIXTURE must point to the compiled host executable. Run with the RPi venv.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "software/rpi"))
from app.robot import Acceleration, ZERO
from app.stm32_protocol import SerialPeer, State, encode_frame


class Port:
    def __init__(self, motion=True):
        self.process = subprocess.Popen(
            [os.environ["ARM_LINK_FIXTURE"], *(["motion"] if motion else [])],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )

    def write(self, raw):
        self.process.stdin.write(raw)
        self.process.stdin.flush()
        return len(raw)

    def read(self, count):
        return self.process.stdout.read(count)

    def raw(self, raw):
        self.write(raw)
        line = self.process.stdout.readline()
        if not line:
            raise AssertionError(self.process.stderr.read().decode())
        return None if line == b"NONE\n" else json.loads(line)

    def command(self, command):
        return self.raw(b"@" + command.encode() + b"\n")

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.process.stdout.close()
            self.process.stderr.close()


class LinkTests(unittest.TestCase):
    def setUp(self):
        self.port = Port()
        self.peer = SerialPeer(self.port, "arm", "ARM-TEST", "a" * 32)
        self.identity = self.peer.hello()

    def tearDown(self):
        self.port.close()

    def packet(self, **changes):
        data = self.peer.request("setpoint", enabled=True,
            command=dict(zl_mps=0.001, zr_mps=0.001, x_mps=0.002, gripper=0.0),
            acceleration=dict(z_mps2=0.1, x_mps2=0.2, gripper_per_s=2.0))
        data.update(changes)
        return data

    def send(self, data):
        reply = self.port.raw(encode_frame(data))
        if reply is not None:
            State.model_validate(reply)
        return reply

    def test_gateway_identity_and_disabled_exchange(self):
        self.assertEqual(self.identity.max_z_difference_mm, 220.0)
        self.assertIn("gripper_velocity", self.identity.capabilities)
        reply = self.peer.setpoint(False, ZERO)
        self.assertTrue(reply.accepted)
        self.assertFalse(reply.enabled)
        self.assertIsNone(reply.arm.gripper_pct)

    def test_full_state_and_ramp_not_restarted(self):
        command = dict(ZERO, z_mps=0.01, x_mps=0.02)
        reply = self.peer.setpoint(True, command, Acceleration(z_mps2=0.05, x_mps2=0.1))
        self.assertTrue(reply.ready)
        first = self.port.command("advance 10")
        self.assertEqual(first["target"], [10000, 10000, 20000])
        self.assertEqual(first["velocity"], [500, 500, 1000])
        self.assertEqual(first["accel"], [50000, 50000, 100000])
        self.peer.setpoint(True, command, Acceleration(z_mps2=0.05, x_mps2=0.1))
        self.assertEqual(self.port.command("advance 10")["velocity"], [1000, 1000, 2000])
        self.peer.setpoint(True, ZERO)
        self.assertEqual(self.port.command("state")["target"], [0, 0, 0])

    def test_physical_axes_are_independent_and_stop_immediate(self):
        self.assertTrue(self.send(self.packet(command=dict(zl_mps=0.001, zr_mps=-0.001,
                                                          x_mps=0.002, gripper=0)))['accepted'])
        self.assertEqual(self.port.command("advance 10")["target"], [1000, -1000, 2000])
        self.peer.setpoint(False, ZERO)
        state = self.port.command("state")
        self.assertEqual(state["velocity"], [0, 0, 0])
        self.assertFalse(state["enabled"])

    def test_watchdog_and_fresh_hello_required(self):
        self.send(self.packet())
        self.assertTrue(self.port.command("time 249")["enabled"])
        self.assertFalse(self.port.command("time 250")["enabled"])
        self.assertFalse(self.send(self.packet())["accepted"])
        self.peer.hello()
        self.assertTrue(self.send(self.packet())["accepted"])

    def test_watchdog_interrupt_independent_of_parser(self):
        self.send(self.packet())
        self.assertTrue(self.port.command("isr 250")["expired"])
        self.assertFalse(self.send(self.packet())["accepted"])
        self.assertFalse(self.port.command("state")["enabled"])

    def test_watchdog_wraparound(self):
        self.port.command("time 4294967200")
        self.send(self.packet())
        self.assertTrue(self.port.command("time 153")["enabled"])
        self.assertFalse(self.port.command("time 154")["enabled"])

    def test_replay_stops_and_does_not_keep_alive(self):
        packet = self.packet()
        self.send(packet)
        self.port.command("time 200")
        self.assertFalse(self.send(packet)["accepted"])
        self.assertFalse(self.port.command("state")["session_valid"])

    def test_disabled_setpoints_also_expire(self):
        self.peer.setpoint(False, ZERO)
        self.assertFalse(self.port.command("time 250")["session_valid"])

    def test_hello_stops_but_preserves_hardware_fault(self):
        self.send(self.packet())
        self.port.command("fault")
        self.peer.hello()
        reply = self.send(self.packet(enabled=False, command=dict(zl_mps=0,zr_mps=0,x_mps=0,gripper=0)))
        self.assertFalse(reply["enabled"])
        self.assertEqual(reply["fault"], "latched_motion_fault")

    def test_unhomed_is_unknown_and_cannot_enable(self):
        self.port.command("unhome")
        reply = self.peer.setpoint(False, ZERO)
        self.assertFalse(reply.ready)
        self.assertFalse(reply.arm.homed)
        self.assertIsNone(reply.arm.z_left_mm)
        self.assertFalse(self.send(self.packet())["accepted"])

    def test_noncommissioned_build_still_identifies(self):
        with_port = Port(motion=False)
        try:
            peer = SerialPeer(with_port, "arm", "ARM-TEST", "b" * 32)
            peer.hello()
            self.assertFalse(peer.setpoint(False, ZERO).ready)
            packet = self.packet(session="b" * 32, seq=100)
            reply = with_port.raw(encode_frame(packet))
            self.assertEqual(reply["fault"], "hardware_not_commissioned")
        finally:
            with_port.close()

    def test_invalid_semantics_stop_atomically(self):
        mutations = [
            lambda p: p["command"].update(gripper=1.1),
            lambda p: p["command"].update(zl_mps=0.190001),
            lambda p: p["command"].update(x_mps="0.01"),
            lambda p: p["command"].update(zr_mps=True),
            lambda p: p["command"].update(vx_mps=0.0),
            lambda p: p["command"].pop("zr_mps"),
            lambda p: p["acceleration"].update(z_mps2=0.100001),
            lambda p: p["acceleration"].update(x_mps2=0.500001),
            lambda p: p["acceleration"].update(z_mps2=0),
            lambda p: p["acceleration"].update(gripper_per_s=0),
            lambda p: p.update(enabled=1),
            lambda p: p.update(enabled=False),
            lambda p: p.update(session="b" * 32),
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.peer.hello()
                self.send(self.packet())
                packet = self.packet()
                mutation(packet)
                reply = self.send(packet)
                self.assertTrue(reply is None or not reply["accepted"])
                state = self.port.command("state")
                self.assertFalse(state["enabled"])
                self.assertEqual(state["target"], [0, 0, 0])
                self.assertEqual(state["velocity"], [0, 0, 0])

    def test_bad_json_duplicate_fields_and_numeric_syntax(self):
        raw = encode_frame(self.packet())
        variants = [
            raw.replace(b'"seq":2', b'"seq":2,"seq":3'),
            raw.replace(b'"zl_mps":0.001', b'"zl_mps":0.001,"zl_mps":0.002'),
            raw.replace(b'"zl_mps":0.001', b'"zl_mps":0.001,"zl_\\u006dps":0.002'),
            raw.replace(b'"zl_mps":0.001', b'"zl_mps":NaN'),
            raw.replace(b'"zl_mps":0.001', b'"zl_mps":1e999'),
            raw.replace(b'"zl_mps":0.001', b'"zl_mps":+1'),
            raw.replace(b'"zl_mps":0.001', b'"zl_mps":01'),
            raw.replace(b'"protocol":1', b'"protocol":true'),
            raw.replace(b'"protocol":1', b'"protocol":1.0'),
            raw.replace(b'"seq":2', b'"seq":9007199254740992'),
            raw.replace(b'"seq":2', b'"seq":2.0'),
            raw.replace(b'"seq":2', b'"seq":2e0'),
            raw[:-2] + b',}\n', raw[:-1] + b'{}\n', b'[]\n', b'HB\n',
            raw.replace(b'"setpoint"', b'"set\x00point"'),
            raw.replace(b'"setpoint"', b'"set\xffpoint"'),
        ]
        for value in variants:
            with self.subTest(raw=value):
                self.peer.hello()
                reply = self.port.raw(value)
                self.assertTrue(reply is None or not reply["accepted"])
                self.assertFalse(self.port.command("state")["session_valid"])

    def test_maximum_sequence_echo(self):
        packet = self.packet(seq=9007199254740991)
        self.assertEqual(self.send(packet)["seq"], packet["seq"])

    def test_frame_limit_and_overflow_recovery(self):
        packet = encode_frame(self.packet())
        self.assertTrue(self.port.raw(b' ' * (2048 - len(packet)) + packet)["accepted"])
        packet = encode_frame(self.packet())
        self.assertIsNone(self.port.raw(b' ' * (2049 - len(packet)) + packet))
        self.assertFalse(self.port.command("state")["enabled"])
        self.peer.hello()
        self.assertTrue(self.send(self.packet())["accepted"])

    def test_skew_and_hard_limit_guard(self):
        packet = self.packet()
        packet["command"].update(zl_mps=0.05, zr_mps=0.0)
        self.assertTrue(self.send(packet)["accepted"])
        self.peer.hello()
        packet = self.packet()
        packet["command"].update(zl_mps=0.05, zr_mps=0.05)
        self.assertTrue(self.send(packet)["accepted"])
        self.assertFalse(self.port.command("limit")["enabled"])

    def test_one_byte_transport_and_concatenated_frames(self):
        first, second = self.packet(), self.packet()
        for byte in encode_frame(first):
            self.port.write(bytes([byte]))
        self.assertTrue(json.loads(self.port.process.stdout.readline())["accepted"])
        third = self.packet()
        self.port.write(encode_frame(second) + encode_frame(third))
        for expected in (second, third):
            reply = State.model_validate(json.loads(self.port.process.stdout.readline()))
            self.assertEqual(reply.seq, expected["seq"])
            self.assertTrue(reply.accepted)

    def test_zero_velocity_decelerates_but_disable_resets_ramp(self):
        self.peer.setpoint(True, dict(ZERO, x_mps=0.02))
        self.assertEqual(self.port.command("advance 50")["velocity"][2], 10000)
        self.peer.setpoint(True, ZERO)
        self.assertEqual(self.port.command("advance 10")["velocity"][2], 8000)
        self.peer.setpoint(False, ZERO)
        self.assertEqual(self.port.command("state")["velocity"], [0, 0, 0])
        self.peer.setpoint(True, ZERO)
        self.assertEqual(self.port.command("advance 10")["velocity"], [0, 0, 0])

    def test_firmware_uses_shared_portable_sources(self):
        arm = ROOT / "software/stm32_arm"
        for file, source in (("Src/arm_motion.c", "src/arm_motion.c"),
                             ("Src/arm_link.c", "src/arm_link.c"),
                             ("Inc/arm_motion.h", "include/arm_motion.h"),
                             ("Inc/arm_axis_config.h", "include/arm_axis_config.h")):
            wrapper = arm / "firmware/obotx_arm_f446re/Core" / file
            self.assertIn('#include "../../../../' + source + '"', wrapper.read_text())


if __name__ == "__main__":
    unittest.main()
