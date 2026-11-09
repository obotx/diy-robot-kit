import time
import unittest

from pydantic import ValidationError
from app.arm_bench import BenchConfig, BenchGateway, ArmBenchRobot, parse_reply
from app.robot import RobotSimulator, Home, HomeKeepalive, Stop, Motion, ZERO, ControlError


class FakePort:
    def __init__(self):
        self.commands = []
        self.buffer = bytearray()
        self.active = False
        self.complete = False
        self.bad_profile = False
        self.fault = False
        self.closed = False

    def write(self, data):
        command = data.decode().strip()
        self.commands.append(command)
        if command == "HOME ZL":
            self.active = True
        if command in ("STOP", "EN 0") or self.complete:
            self.active = False
        if command == "INPUTS":
            response = f"IO power=1 zl=0,0,0 zr=0,0,0 x=0,0,0 bench=1 standalone_zl={int(not self.bad_profile)} skew_um=100000\r\n"
        else:
            response = f"OK\r\nSTAT en={int(self.active)} mode={3 if self.active else 0} fault={int(self.fault)} zl=0 h_zl={int(self.complete)}\r\n"
        self.buffer.extend(response.encode())

    def read(self, count):
        result = self.buffer[:count]
        del self.buffer[:count]
        return bytes(result)

    def close(self):
        self.closed = True


def wait_for(predicate):
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("Timed out")


class BenchTests(unittest.TestCase):
    def setUp(self):
        self.port = FakePort()
        self.gateway = BenchGateway(BenchConfig(port="COM_TEST", standalone_confirmed=True), lambda: self.port)
        self.robot = ArmBenchRobot(self.gateway)
        self.gateway.start()
        self.addCleanup(self.gateway.close)
        wait_for(lambda: self.gateway.snapshot()["ready"])

    def home(self):
        self.robot.apply("browser", Home(type="home", seq=1, axis="ZL"))
        wait_for(lambda: "HOME ZL" in self.port.commands)

    def test_success_requires_reported_home_and_releases_owner(self):
        self.home()
        self.port.complete = True
        wait_for(lambda: self.gateway.snapshot()["phase"] == "complete")
        state = self.robot.snapshot()
        self.assertIsNone(state["owner"])
        self.assertTrue(state["arm"]["homed_axes"]["ZL"])
        self.assertFalse(state["arm"]["homed"])
        self.assertIsNone(state["arm"]["z_right_mm"])

    def test_worker_watchdog_without_web_ticks(self):
        self.home()
        wait_for(lambda: self.port.commands.count("STOP") >= 2)
        self.assertFalse(self.port.active)
        count = self.port.commands.count("HOME ZL")
        with self.assertRaises(ControlError):
            self.robot.apply("browser", HomeKeepalive(type="home_keepalive", seq=2))
        time.sleep(0.1)
        self.assertEqual(count, self.port.commands.count("HOME ZL"))

    def test_keepalive_then_disconnect_stops(self):
        self.home()
        for seq in range(2, 9):
            self.robot.apply("browser", HomeKeepalive(type="home_keepalive", seq=seq))
            time.sleep(0.08)
        self.assertTrue(self.port.active)
        self.robot.disconnect("browser")
        wait_for(lambda: not self.port.active)

    def test_stop_prevents_renewal(self):
        self.home()
        self.robot.apply("observer", Stop(type="stop", seq=2))
        with self.assertRaises(ControlError):
            self.robot.apply("browser", HomeKeepalive(type="home_keepalive", seq=3))
        wait_for(lambda: not self.port.active)

    def test_fault_stops_and_never_reconnects(self):
        self.home()
        self.port.fault = True
        wait_for(lambda: self.port.closed)
        self.assertFalse(self.gateway.snapshot()["connected"])
        self.assertFalse(self.port.active)

    def test_confirmation_required(self):
        self.gateway.config.standalone_confirmed = False
        with self.assertRaises(ControlError):
            self.home()
        self.assertNotIn("HOME ZL", self.port.commands)

    def test_changed_profile_rejects_home(self):
        self.port.bad_profile = True
        self.robot.apply("browser", Home(type="home", seq=1, axis="ZL"))
        wait_for(lambda: self.port.closed)
        self.assertNotIn("HOME ZL", self.port.commands)


class HomingTests(unittest.TestCase):
    def test_simulator_home_only_zl_and_watchdog(self):
        now = [0.0]
        robot = RobotSimulator(lambda: now[0])
        robot.z_left_mm = 1
        robot.apply("test", Home(type="home", seq=1, axis="ZL"))
        now[0] += 0.1
        state = robot.snapshot()
        self.assertTrue(state["arm"]["homed_axes"]["ZL"])
        self.assertEqual(state["arm"]["z_right_mm"], 350)
        robot.z_left_mm = 100
        robot.apply("test", Home(type="home", seq=2, axis="ZL"))
        now[0] += 0.7
        self.assertIsNone(robot.snapshot()["homing"]["active_axis"])
        self.assertFalse(robot.homed_axes["ZL"])

    def test_linear_limit_all_axes(self):
        for axis in ("vx_mps", "vy_mps", "z_mps", "zl_mps", "zr_mps", "x_mps"):
            limit = .19 if axis in ("z_mps", "zl_mps", "zr_mps") else .2
            Motion(type="motion", seq=1, **(ZERO | {axis: limit}))
            with self.assertRaises(ValidationError):
                Motion(type="motion", seq=1, **(ZERO | {axis: limit + .0001}))

    def test_parser_rejects_invalid_and_duplicate(self):
        for line in ("ERR bad", "STAT en=0 en=1", "STAT en=NaN"):
            with self.assertRaises(ValueError):
                parse_reply(line, "STAT")
