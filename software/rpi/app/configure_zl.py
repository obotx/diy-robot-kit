"""Explicit commissioning helper for the operator-confirmed isolated ZL fixture."""
import argparse
import json
from pathlib import Path
import shutil
import time
from uuid import uuid4

from .stm32 import open_port
from .stm32_config import BoardConfig, STM32Config
from .stm32_discovery import candidates, MAX_CANDIDATES
from .stm32_protocol import LinkError, SerialPeer


def select_zl(replies):
    roles = [identity.role for _, identity in replies]
    ids = [identity.device_id for _, identity in replies]
    if len(set(roles)) != len(roles) or len(set(ids)) != len(ids):
        raise LinkError("Ambiguous controller roles or IDs")
    arms = [(path, identity) for path, identity in replies if identity.role == "arm"]
    if len(arms) != 1:
        raise LinkError("Exactly one ARM controller is required")
    path, identity = arms[0]
    if (identity.enabled or identity.firmware != "arm-link-2"
            or not {"arm_actions_v1", "zl_only"}.issubset(identity.capabilities)
            or identity.max_z_difference_mm != 100.0):
        raise LinkError("Expected disabled arm-link-2 isolated zl_only profile with 100 mm skew setting")
    return STM32Config(arm_only=True, commissioned=True,
                       arm=BoardConfig(port=path, device_id=identity.device_id),
                       max_z_difference_mm=identity.max_z_difference_mm)


def detect(opener=open_port, enumerate_ports=candidates, selector=select_zl):
    paths = enumerate_ports()
    if not paths or len(paths) > MAX_CANDIDATES:
        raise LinkError("Expected 1-8 supported ST-Link ports")
    replies = []
    config = STM32Config(discovery=True)
    for path in paths:
        if "://" in path:
            raise LinkError("Only local serial ports are permitted")
        peer = SerialPeer(opener(BoardConfig(port=path, device_id="detecting"), config, "stm32"),
                          None, None, uuid4().hex)
        try:
            replies.append((path, peer.hello()))
        except Exception as exc:
            raise LinkError(f"{path}: {exc}") from exc
        finally:
            peer.stop_best_effort()
            peer.close()
    return selector(replies)


def stable_port(port):
    # Retain role/device validation; prefer a stable Linux USB symlink when unique.
    root = Path("/dev/serial/by-id")
    matches = [p for p in root.glob("*") if p.resolve() == Path(port).resolve()]
    return str(matches[0]) if len(matches) == 1 else port


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--confirmed-isolated-zl", action="store_true", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    config = detect()
    config.arm.port = stable_port(config.arm.port)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists():
        shutil.copy2(args.output, args.output.with_name(args.output.name + ".bak-" + str(time.time_ns())))
    temporary = args.output.with_name(args.output.name + ".tmp-" + uuid4().hex)
    try:
        temporary.write_text(json.dumps(config.model_dump(), indent=2) + "\n", encoding="utf-8")
        temporary.replace(args.output)
    finally:
        temporary.unlink(missing_ok=True)
    print(f"Configured isolated ZL: {config.arm.device_id} at {config.arm.port}")
    print("No HOME or motion commands sent. Service configuration can now be activated.")


if __name__ == "__main__":
    main()
