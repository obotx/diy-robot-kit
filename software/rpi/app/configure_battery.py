"""Save the SmartShunt MAC and Instant Readout key locally (never in shell history)."""
import getpass
import json
import os
from uuid import uuid4

from .battery import BatteryConfig, config_path


def main():
    address = input("SmartShunt Bluetooth MAC address: ").strip()
    key = getpass.getpass("Instant Readout encryption key (32 hex characters, NOT the PIN): ").strip()
    try:
        config = BatteryConfig(address=address, advertisement_key=key)
    except ValueError:
        raise SystemExit("Invalid MAC address or key. Configuration was not changed.")
    path = config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp-" + uuid4().hex)
    data = config.model_dump(exclude={"advertisement_key"})
    data["advertisement_key"] = config.advertisement_key.get_secret_value()
    try:
        with os.fdopen(os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as file:
            json.dump(data, file, indent=2)
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)
    print("SmartShunt saved. Recharge warning: 20%; power-off warning: 5%.")
    print("Restart obotx-rpi-control to activate. No Bluetooth pairing is required.")


if __name__ == "__main__":
    main()
