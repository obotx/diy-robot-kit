"""Explicit, non-motion identification for commissioning; never scan by opening ports."""

import argparse
import json
from uuid import uuid4

from serial.tools import list_ports

from .stm32 import open_port
from .stm32_config import BoardConfig, STM32Config
from .stm32_protocol import SerialPeer


def main():
    parser = argparse.ArgumentParser(description="List USB ports or identify one ObotX v1 controller")
    parser.add_argument("--identify", metavar="PORT", help="Send HELLO to this port (disables the controller)")
    parser.add_argument("--role", choices=("arm", "base"))
    args = parser.parse_args()
    if not args.identify:
        print(json.dumps([{"port": p.device, "usb_serial": p.serial_number,
                           "description": p.description, "vid": p.vid, "pid": p.pid,
                           "location": p.location} for p in list_ports.comports()], indent=2))
        return
    if not args.role or "://" in args.identify:
        parser.error("Identification requires --role and a local serial port")
    board = BoardConfig(port=args.identify, device_id="not-yet-known")
    settings = STM32Config(arm=board, base=BoardConfig(port="unused", device_id="unused"))
    peer = SerialPeer(open_port(board, settings, "stm32"), args.role, None, uuid4().hex)
    try:
        print(json.dumps(peer.hello().model_dump(), indent=2))
    finally:
        peer.stop_best_effort()
        peer.close()


if __name__ == "__main__":
    main()
