"""Service bench tests. Never reconnect or clear faults automatically."""
import argparse
from decimal import Decimal, InvalidOperation
import time

import serial


def actuator_pulse(port, axis, velocity, seconds):
    """One short JOG with no heartbeat: the MCU lease bounds a stalled PC too."""
    status = exchange(port, "EN 1")
    if "en=1 " not in status or "fault=0 " not in status:
        raise RuntimeError("Enable rejected; no JOG sent")
    deadline = time.monotonic() + seconds
    status = exchange(port, f"JOG {axis} {velocity}")
    if "en=1 " not in status or "fault=0 " not in status:
        raise RuntimeError("JOG rejected or firmware fault")
    time.sleep(max(0, deadline - time.monotonic()))


def exchange(port, command):
    port.write((command + "\n").encode("ascii"))
    port.flush()
    deadline = time.monotonic() + 0.6
    while time.monotonic() < deadline:
        line = port.readline().decode("ascii", errors="replace").strip()
        if not line:
            continue
        print(line, flush=True)
        if line.startswith("ERR"):
            raise RuntimeError(line)
        if line.startswith(("STAT ", "IO ")):
            return line
    raise TimeoutError("No service-console reply; check firmware profile, COM and 115200 baud")


def parse_speed(value):
    try:
        speed = Decimal(str(value))
    except InvalidOperation as exc:
        raise argparse.ArgumentTypeError("Speed must be a number in mm/s") from exc
    if not speed.is_finite() or not Decimal("0.001") <= speed <= Decimal("250") or speed * 1000 != (speed * 1000).to_integral_value():
        raise argparse.ArgumentTypeError("Speed: 0.001..250 mm/s, at most three decimal places")
    return speed


def travel(port, millimeters, initial_status, speed_mm_s=1):
    """Distance is bounded by the MCU pulse budget; heartbeat only keeps the lease alive."""
    def fields(status):
        return dict(part.split("=", 1) for part in status.split() if "=" in part)
    start = int(fields(initial_status)["zl"])
    speed_mm_s = parse_speed(speed_mm_s)
    if not 0 < abs(millimeters) <= 20:
        raise ValueError("Travel: at most 20 mm")
    if speed_mm_s > 1 and (millimeters <= 0 or fields(initial_status).get("h_zl") != "1" or
                           start < 0 or start + millimeters * 1000 > 80000):
        raise RuntimeError("Fast travel requires homed ZL, positive travel and target <= 80 mm")
    deadline = time.monotonic() + max(45, abs(millimeters) / float(speed_mm_s) + 10)
    status = exchange(port, f"TRAVEL ZL {millimeters * 1000} {int(speed_mm_s * 1000)}")
    while True:
        values = fields(status)
        if values.get("fault") != "0":
            raise RuntimeError("Firmware fault during travel")
        if values.get("en") == "0" and values.get("mode") == "0":
            if int(values["zl"]) - start != millimeters * 1000:
                raise RuntimeError("Travel stopped before the requested pulse count")
            print("Completed pulse-counted travel; measure actual carriage displacement.")
            return
        if time.monotonic() >= deadline:
            raise TimeoutError("Travel did not finish within 45 seconds")
        time.sleep(0.15)
        status = exchange(port, "HB")


def home_zl(port):
    """Standalone guide only: the MCU owns homing limits and the pulse budget."""
    deadline = time.monotonic() + 305
    status = exchange(port, "HOME ZL")
    while True:
        values = dict(part.split("=", 1) for part in status.split() if "=" in part)
        if values.get("fault") != "0":
            raise RuntimeError("Firmware fault during homing; inspect before explicit CLR")
        if values.get("en") == "0" and values.get("mode") == "0":
            if values.get("h_zl") != "1" or values.get("zl") != "0":
                raise RuntimeError("Homing stopped without establishing ZL reference")
            print("ZL homed and disabled. Zero is the final released position, not the switch edge.")
            return
        if time.monotonic() >= deadline:
            raise TimeoutError("Homing exceeded PC deadline")
        time.sleep(0.15)
        status = exchange(port, "HB")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    modes = parser.add_subparsers(dest="mode", required=True)
    modes.add_parser("inputs")
    modes.add_parser("status")
    modes.add_parser("clear", help="Explicit CLR request; does not enable outputs")
    scope = modes.add_parser("scope")
    scope.add_argument("--axis", choices=("Z", "ZL", "ZR", "X"), required=True)
    scope.add_argument("--velocity", type=int, required=True, help="um/s, not Hz")
    scope.add_argument("--seconds", type=float, default=1.0)
    scope.add_argument("--motors-disconnected", action="store_true", required=True)
    actuator = modes.add_parser("actuator", help="One secured horizontal Z screw actuator only, verified ENA/watchdog")
    actuator.add_argument("--axis", choices=("ZL", "ZR"), required=True)
    actuator.add_argument("--velocity", type=int, default=500, help="um/s; maximum absolute value 500")
    actuator.add_argument("--seconds", type=float, default=0.25)
    actuator.add_argument("--standalone-actuator", action="store_true", required=True,
                          help="Confirm only one driver connected, actuator secured outside robot, travel clear, power cut accessible")
    bounded = modes.add_parser("travel", help="Pulse-counted ZL travel in standalone firmware only")
    bounded.add_argument("--mm", type=int, default=20, help="Signed distance, at most 20 mm")
    bounded.add_argument("--speed-mm-s", type=parse_speed, default=1,
                         help="0.001..250 mm/s; above 1: homed ZL, positive only, target <= 80 mm")
    bounded.add_argument("--standalone-actuator", action="store_true", required=True,
                         help="One secured horizontal guide outside robot, clear travel, accessible power cut")
    homing = modes.add_parser("home-zl", help="Standalone ZL only, start within 1000 mm of verified MIN")
    homing.add_argument("--standalone-actuator", action="store_true", required=True,
                        help="Secured horizontal ZL, both limits verified, MIN in negative direction, power cut accessible")
    args = parser.parse_args()
    if args.mode == "travel" and not 0 < abs(args.mm) <= 20:
        parser.error("Travel requires 0 < |distance| <= 20 mm")
    if args.mode == "scope":
        limit = 10000 if args.axis == "X" else 1000
        if not (0 < args.seconds <= 1 and 0 < abs(args.velocity) <= limit):
            parser.error("Scope test: 0 < duration <= 1 s, |Z velocity| <= 1000, |X velocity| <= 10000 um/s")
    if args.mode == "actuator" and not (0 < args.seconds <= 0.5 and 0 < abs(args.velocity) <= 500):
        parser.error("Actuator test: 0 < duration <= 0.5 s and 0 < |velocity| <= 500 um/s")
    with serial.Serial(args.port, 115200, timeout=0.1, write_timeout=0.2) as port:
        if args.mode not in ("scope", "actuator", "travel", "home-zl"):
            exchange(port, {"inputs": "INPUTS", "status": "STATUS", "clear": "CLR"}[args.mode])
            return
        try:
            info = exchange(port, "INPUTS")
            if "bench=1" not in info or "power=1" not in info:
                raise RuntimeError("Requires bench firmware and asserted bench POWER_OK input")
            status = exchange(port, "STATUS")
            if "fault=0 " not in status or "en=0 " not in status:
                raise RuntimeError("Board must be disabled and fault-free; inspect inputs before explicit CLR")
            if args.mode in ("actuator", "travel", "home-zl"):
                if "mode=0 " not in status or any(f"{axis}=0,0,0" not in info for axis in ("zl", "zr", "x")):
                    raise RuntimeError("Actuator test requires idle state and all limit/alarm inputs inactive")
                if args.mode in ("travel", "home-zl"):
                    if "standalone_zl=1" not in info or "skew_um=100000" not in info:
                        raise RuntimeError("Flash the standalone ZL firmware before using travel")
                    if args.mode == "home-zl":
                        home_zl(port)
                    else:
                        travel(port, args.mm, status, args.speed_mm_s)
                else:
                    actuator_pulse(port, args.axis, args.velocity, args.seconds)
                return
            exchange(port, "EN 1")
            exchange(port, f"JOG {args.axis} {args.velocity}")
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                time.sleep(min(0.15, max(0, deadline - time.monotonic())))
                status = exchange(port, "HB")
                if "fault=0 " not in status:
                    raise RuntimeError("Firmware fault during scope test")
        finally:
            # Both requests are attempted even after an I/O failure. MCU watchdog is independent.
            for command in ("STOP", "EN 0"):
                try:
                    exchange(port, command)
                except (OSError, RuntimeError, TimeoutError, serial.SerialException) as error:
                    print(f"{command}: {error}", flush=True)


if __name__ == "__main__":
    main()
