"""Commission the full JSON ARM profile without sending motion commands."""
import argparse
import json
import math
from pathlib import Path
import shutil
import time
from uuid import uuid4

from .configure_zl import detect, stable_port
from .stm32_config import BoardConfig, STM32Config
from .stm32_protocol import LinkError


def select_arm(replies, max_z_difference_mm):
    if not math.isfinite(max_z_difference_mm) or not 0 < max_z_difference_mm <= 1000:
        raise LinkError("Confirm a finite Z difference limit in (0, 1000] mm")
    roles = [identity.role for _, identity in replies]
    ids = [identity.device_id for _, identity in replies]
    if len(set(roles)) != len(roles) or len(set(ids)) != len(ids):
        raise LinkError("Ambiguous controller roles or IDs")
    arms = [(path, identity) for path, identity in replies if identity.role == "arm"]
    if len(arms) != 1:
        raise LinkError("Exactly one ARM controller is required")
    path, identity = arms[0]
    required = {"full_state_v1", "local_watchdog", "acceleration_limits",
                "arm_actions_v1", "z_difference_limit"}
    if (identity.enabled or identity.firmware != "arm-link-2"
            or not required.issubset(identity.capabilities)
            or "zl_only" in identity.capabilities):
        raise LinkError("Requires disabled full arm-link-2 firmware, not the zl_only build")
    if identity.max_z_difference_mm != max_z_difference_mm:
        raise LinkError("Confirmed Z difference limit must match the ARM firmware limit")
    return STM32Config(arm_only=True, commissioned=True,
        arm=BoardConfig(port=path, device_id=identity.device_id),
        max_z_difference_mm=max_z_difference_mm)


def select_robot(replies):
    arms = [identity for _, identity in replies if identity.role == "arm"]
    if len(arms) != 1 or arms[0].max_z_difference_mm is None:
        raise LinkError("Exactly one ARM with a reported Z difference limit is required")
    config = select_arm(replies, arms[0].max_z_difference_mm)
    bases = [(path, identity) for path, identity in replies if identity.role == "base"]
    if len(bases) != 1 or bases[0][1].enabled:
        raise LinkError("Exactly one disabled BASE controller is required")
    path, identity = bases[0]
    required = {"full_state_v1", "local_watchdog", "acceleration_limits", "base_clear_fault_v1"}
    if not required.issubset(identity.capabilities):
        raise LinkError("Update BASE firmware to base-mdd10a-5 or newer")
    config.arm_only = False
    config.base = BoardConfig(port=stable_port(path), device_id=identity.device_id)
    return STM32Config.model_validate(config.model_dump())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--max-z-difference-mm", type=float)
    parser.add_argument("--service-only", action="store_true")
    parser.add_argument("--with-base", action="store_true")
    parser.add_argument("--robot", action="store_true",
                        help="Unified normal/service profile using the ARM firmware Z limit")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.robot and (args.service_only or args.max_z_difference_mm is not None):
        parser.error("--robot cannot be combined with service-only or a Z limit override")
    if not args.robot and not args.service_only and args.max_z_difference_mm is None:
        parser.error("--max-z-difference-mm is required for normal commissioning")
    def select(replies):
        if args.robot:
            return select_robot(replies)
        config = select_service(replies) if args.service_only else select_arm(replies, args.max_z_difference_mm)
        if args.with_base:
            bases = [(path, identity) for path, identity in replies if identity.role == "base"]
            if len(bases) != 1 or bases[0][1].enabled:
                raise LinkError("Exactly one disabled BASE controller is required")
            path, identity = bases[0]
            if not {"full_state_v1", "local_watchdog", "acceleration_limits"}.issubset(identity.capabilities):
                raise LinkError("BASE firmware does not support the control protocol")
            config.arm_only = False
            config.base = BoardConfig(port=stable_port(path), device_id=identity.device_id)
        return STM32Config.model_validate(config.model_dump())
    config = detect(selector=select)
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
    print(f"Configured full ARM: {config.arm.device_id}. Profile: {'service only' if args.service_only else 'normal commissioned' }.")
    print("No HOME or motion commands sent. BASE configured: " + str(config.base is not None))
    print(f"Normal-mode Z difference limit from firmware: {config.max_z_difference_mm} mm")


def select_service(replies):
    arms = [identity for _, identity in replies if identity.role == "arm"]
    if len(arms) != 1 or "service_jog_v1" not in arms[0].capabilities or arms[0].max_z_difference_mm is None:
        raise LinkError("Requires exactly one full ARM with service_jog_v1 firmware")
    config = select_arm(replies, arms[0].max_z_difference_mm)
    # Pin the firmware identity, but do not commission normal operation or its skew limit.
    config.commissioned = False
    config.service_only = True
    return config


if __name__ == "__main__":
    main()
