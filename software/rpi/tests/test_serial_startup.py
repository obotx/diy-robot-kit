import unittest
from unittest.mock import patch

from app.stm32 import drain_serial_input, open_port
from app.stm32_config import BoardConfig, STM32Config
from app.stm32_protocol import SerialPeer, LinkError
from app.robot import ZERO
from test_stm32 import Clock, FakePort
from app.configure_zl import detect


class StartupPort(FakePort):
    def __init__(self, clock):
        super().__init__("base", clock)
        self.input.extend(b'ype":"state","session":"old"}\n')
        self.delayed = True

    def read(self, count):
        if not self.input and self.delayed and self.clock.now >= 0.02:
            self.input.extend(b'late old USB bytes\r\n')
            self.delayed = False
        return super().read(count)


class StartupTests(unittest.TestCase):
    def test_configurator_with_old_base_fragment_and_real_port_open_path(self):
        clock = Clock()
        base = StartupPort(clock)
        arm = FakePort("arm", clock)
        arm.filter = lambda reply: reply | {"firmware": "arm-link-2",
            "capabilities": reply["capabilities"] + ["arm_actions_v1", "zl_only"]} if reply["type"] == "identity" else reply
        with patch("serial.Serial", side_effect=[base, arm]), \
             patch("app.stm32.drain_serial_input", side_effect=lambda port: drain_serial_input(port, clock)):
            config = detect(enumerate_ports=lambda: ["COM-base", "COM-arm"])
        self.assertTrue(config.arm_only)
        self.assertTrue(config.commissioned)
        self.assertEqual(config.arm.port, "COM-arm")
        self.assertTrue(base.closed and arm.closed)
        self.assertFalse(any(frame.get("enabled", False) for port in (base, arm) for frame in port.sent))

    def test_fragment_and_delayed_usb_bytes_drained_before_hello(self):
        clock = Clock()
        port = StartupPort(clock)
        count = drain_serial_input(port, clock)
        self.assertGreater(count, 30)
        self.assertEqual(port.sent, [])
        self.assertFalse(port.delayed)
        peer = SerialPeer(port, None, None, "a" * 32, clock)
        self.assertEqual(peer.hello().role, "base")
        self.assertFalse(peer.setpoint(False, ZERO).enabled)

    def test_malformed_bytes_after_hello_still_rejected(self):
        clock = Clock()
        port = StartupPort(clock)
        drain_serial_input(port, clock)
        port.input.extend(b'garbage\n')
        peer = SerialPeer(port, None, None, "a" * 32, clock)
        with self.assertRaisesRegex(LinkError, "invalid_json"):
            peer.hello()

    def test_continuous_input_is_bounded_by_time(self):
        clock = Clock()
        class Noisy:
            def read(self, count):
                clock.now += 0.01
                return b"x"
        with self.assertRaisesRegex(LinkError, "not_quiet"):
            drain_serial_input(Noisy(), clock)
        self.assertLess(clock.now, 0.52)

    def test_continuous_input_is_bounded_by_size(self):
        class Noisy:
            def read(self, count):
                return b"x"
        with self.assertRaisesRegex(LinkError, "overflow"):
            drain_serial_input(Noisy(), Clock())

    def test_hardware_open_drains_and_closes_on_sync_failure(self):
        clock = Clock()
        port = StartupPort(clock)
        board = BoardConfig(port="COM-test", device_id="BASE-test")
        with patch("serial.Serial", return_value=port), \
             patch("app.stm32.drain_serial_input", side_effect=LinkError("noisy")):
            with self.assertRaisesRegex(LinkError, "noisy"):
                open_port(board, STM32Config(discovery=True), "stm32")
        self.assertTrue(port.closed)
