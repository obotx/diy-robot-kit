"""Native PC WebSocket example. Default: telemetry only; --demo: mock motion."""

import argparse
import json
import time

from websockets.sync.client import connect


ZERO = dict(vx_mps=0.0, vy_mps=0.0, wz_radps=0.0, z_mps=0.0,
            zl_mps=0.0, zr_mps=0.0, x_mps=0.0, gripper=0.0)


def receive_kind(socket, kind, expected_seq=None):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        message = json.loads(socket.recv(timeout=max(0.01, deadline - time.monotonic())))
        if message["type"] == "error":
            raise RuntimeError(message["code"])
        if message["type"] == kind and (expected_seq is None or message.get("seq") == expected_seq):
            return message
    raise TimeoutError(f"No {kind} response")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="ws://rpi-control.local:8000/ws/control")
    parser.add_argument("--demo", action="store_true", help="Request control and jog grouped Z in mock mode for one second")
    args = parser.parse_args()
    with connect(args.url, open_timeout=5, max_size=65536) as socket:
        receive_kind(socket, "welcome")
        state = receive_kind(socket, "telemetry")
        print(json.dumps(state, indent=2))
        if not args.demo:
            return
        if state.get("backend") != "mock" or state.get("hardware_connected") is not False:
            raise RuntimeError("Demo is permitted only in confirmed mock mode")
        seq = 1
        socket.send(json.dumps({"type": "enable", "seq": seq, "enabled": True, "source": "pc-python"}))
        receive_kind(socket, "ack", seq)
        try:
            for _ in range(10):
                seq += 1
                socket.send(json.dumps({"type": "motion", "seq": seq, **ZERO, "z_mps": 0.02}))
                receive_kind(socket, "ack", seq)
                time.sleep(0.1)
        finally:
            socket.send(json.dumps({"type": "stop", "seq": seq + 1}))
            receive_kind(socket, "ack", seq + 1)
        print("Demo stopped.")
        print(json.dumps(receive_kind(socket, "telemetry"), indent=2))


if __name__ == "__main__":
    main()
