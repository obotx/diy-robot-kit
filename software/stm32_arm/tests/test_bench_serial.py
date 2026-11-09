import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import MagicMock, patch

spec = importlib.util.spec_from_file_location("bench_serial", Path(__file__).parents[1] / "tools" / "bench_serial.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class ActuatorTest(unittest.TestCase):
    def run_console(self, replies, extra=()):
        commands = []
        def reply(port, command):
            commands.append(command)
            value = replies.get(command, "STAT en=0 fault=0 mode=0 x=0")
            if isinstance(value, Exception):
                raise value
            return value
        argv = ["bench", "--port", "COM_TEST", "actuator", "--axis", "ZL", "--standalone-actuator", *extra]
        with patch.object(sys, "argv", argv), patch.object(bench.serial, "Serial", MagicMock()), \
                patch.object(bench, "exchange", side_effect=reply), patch.object(bench.time, "sleep"), \
                contextlib.redirect_stdout(io.StringIO()):
            try:
                bench.main()
            except RuntimeError:
                pass
        return commands

    def ready(self):
        return {"INPUTS": "IO power=1 zl=0,0,0 zr=0,0,0 x=0,0,0 bench=1",
                "EN 1": "STAT en=1 fault=0 mode=0 x=0",
                "JOG ZL 500": "STAT en=1 fault=0 mode=1 x=0"}

    def test_bounded_test_without_heartbeat_and_final_stop(self):
        commands = self.run_console(self.ready())
        self.assertEqual(commands, ["INPUTS", "STATUS", "EN 1", "JOG ZL 500", "STOP", "EN 0"])

    def test_enable_failure_does_not_send_jog(self):
        replies = self.ready()
        replies["EN 1"] = "STAT en=0 fault=1 mode=0 x=0"
        commands = self.run_console(replies)
        self.assertNotIn("JOG ZL 500", commands)
        self.assertEqual(commands[-2:], ["STOP", "EN 0"])

    def test_communication_failure_attempts_stop_and_disable(self):
        replies = self.ready()
        replies["JOG ZL 500"] = RuntimeError("lost")
        self.assertEqual(self.run_console(replies)[-2:], ["STOP", "EN 0"])

    def test_active_limit_rejects_before_enable(self):
        replies = self.ready()
        replies["INPUTS"] = "IO power=1 zl=1,0,0 zr=0,0,0 x=0,0,0 bench=1"
        self.assertNotIn("EN 1", self.run_console(replies))

    def test_unsafe_arguments_do_not_open_port(self):
        for extra in (("--seconds", "1"), ("--seconds", "nan"), ("--velocity", "1000")):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                self.run_console(self.ready(), extra)


class TravelTest(unittest.TestCase):
    def test_fast_travel_units(self):
        for speed in (2, 2.5, 5, 7.812, 100, 200, 250):
            with patch.object(bench, "exchange", return_value="STAT en=0 fault=0 mode=0 zl=20000") as exchange, \
                    contextlib.redirect_stdout(io.StringIO()):
                bench.travel(None, 20, "STAT zl=0 h_zl=1", speed)
                exchange.assert_called_once_with(None, f"TRAVEL ZL 20000 {int(speed * 1000)}")

    def test_speed_range_and_nonfinite_rejected(self):
        for speed in ("nan", "inf", "-inf", "0", "-1", "250.001", "1.0001", "abc"):
            with self.assertRaises(bench.argparse.ArgumentTypeError):
                bench.parse_speed(speed)

    def test_fast_travel_preconditions_before_send(self):
        for mm, status in ((20, "STAT zl=0 h_zl=0"), (-20, "STAT zl=20000 h_zl=1"),
                           (20, "STAT zl=70000 h_zl=1"), (20, "STAT zl=-1 h_zl=1")):
            with patch.object(bench, "exchange") as exchange, self.assertRaises(RuntimeError):
                bench.travel(None, mm, status, 5)
            exchange.assert_not_called()

    def test_positive_and_negative_completion(self):
        for mm in (20, -20):
            replies = ["STAT en=1 fault=0 mode=1 zl=114",
                       f"STAT en=0 fault=0 mode=0 zl={114 + mm * 1000}"]
            with patch.object(bench, "exchange", side_effect=replies) as exchange, \
                    patch.object(bench.time, "sleep"), contextlib.redirect_stdout(io.StringIO()):
                bench.travel(None, mm, "STAT en=0 fault=0 mode=0 zl=114")
                self.assertEqual(exchange.call_args_list[0].args[1], f"TRAVEL ZL {mm * 1000} 1000")
                self.assertEqual(exchange.call_args_list[1].args[1], "HB")

    def test_fault_and_early_stop_are_not_success(self):
        for reply in ("STAT en=0 fault=1 mode=0 zl=10", "STAT en=0 fault=0 mode=0 zl=10"):
            with patch.object(bench, "exchange", return_value=reply), self.assertRaises(RuntimeError):
                bench.travel(None, 20, "STAT zl=0")

    def test_old_firmware_rejected_and_cleanup_attempted(self):
        argv = ["bench", "--port", "COM_TEST", "travel", "--standalone-actuator"]
        replies = ["IO bench=1 power=1 zl=0,0,0 zr=0,0,0 x=0,0,0",
                   "STAT en=0 fault=0 mode=0 zl=0", "STAT en=0", "STAT en=0"]
        with patch.object(sys, "argv", argv), patch.object(bench.serial, "Serial", MagicMock()), \
                patch.object(bench, "exchange", side_effect=replies) as exchange:
            with self.assertRaisesRegex(RuntimeError, "Flash"):
                bench.main()
            self.assertEqual([call.args[1] for call in exchange.call_args_list],
                             ["INPUTS", "STATUS", "STOP", "EN 0"])

    def test_unsafe_distance_does_not_open_port(self):
        for mm in ("0", "21", "-21"):
            argv = ["bench", "--port", "COM_TEST", "travel", "--standalone-actuator", "--mm", mm]
            with patch.object(sys, "argv", argv), patch.object(bench.serial, "Serial") as serial_port, \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                bench.main()
            serial_port.assert_not_called()


class HomeTest(unittest.TestCase):
    def test_heartbeat_until_disabled_and_homed(self):
        replies = ["STAT en=1 fault=0 mode=3 zl=-100 h_zl=0",
                   "STAT en=0 fault=0 mode=0 zl=0 h_zl=1"]
        with patch.object(bench, "exchange", side_effect=replies) as exchange, \
                patch.object(bench.time, "sleep"), contextlib.redirect_stdout(io.StringIO()):
            bench.home_zl(None)
            self.assertEqual([call.args[1] for call in exchange.call_args_list], ["HOME ZL", "HB"])

    def test_fault_or_unhomed_stop_is_failure(self):
        for reply in ("STAT en=0 fault=1 mode=0 zl=0 h_zl=0",
                      "STAT en=0 fault=0 mode=0 zl=0 h_zl=0"):
            with patch.object(bench, "exchange", return_value=reply), self.assertRaises(RuntimeError):
                bench.home_zl(None)

    def test_connection_loss_attempts_cleanup(self):
        argv = ["bench", "--port", "COM_TEST", "home-zl", "--standalone-actuator"]
        replies = ["IO bench=1 power=1 zl=0,0,0 zr=0,0,0 x=0,0,0 standalone_zl=1 skew_um=100000",
                   "STAT en=0 fault=0 mode=0 zl=0", TimeoutError("lost"), "STAT en=0", "STAT en=0"]
        with patch.object(sys, "argv", argv), patch.object(bench.serial, "Serial", MagicMock()), \
                patch.object(bench, "exchange", side_effect=replies) as exchange:
            with self.assertRaises(TimeoutError):
                bench.main()
            self.assertEqual([call.args[1] for call in exchange.call_args_list],
                             ["INPUTS", "STATUS", "HOME ZL", "STOP", "EN 0"])


if __name__ == "__main__":
    unittest.main()
