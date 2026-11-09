"""Prepare, but never activate, the dedicated NetworkManager hotspot."""
import getpass
import os
from pathlib import Path
import subprocess
import tempfile

UUID = "65df7df3-880d-46af-b541-6d573d279955"
TARGET = Path("/etc/NetworkManager/system-connections/obotx-hotspot.nmconnection")


def profile(password):
    # Restrict the password alphabet to avoid keyfile escaping ambiguities.
    if not 12 <= len(password) <= 63 or not all(c.isascii() and c.isalnum() for c in password):
        raise ValueError("Use 12-63 ASCII letters and digits, without spaces.")
    return f"""[connection]
id=obotx-hotspot
uuid={UUID}
type=wifi
interface-name=wlan0
autoconnect=false
autoconnect-priority=999

[wifi]
mode=ap
ssid=ObotX
band=bg
powersave=2

[wifi-security]
key-mgmt=wpa-psk
proto=rsn;
psk={password}

[ipv4]
method=shared
address1=192.168.60.1/24
never-default=true

[ipv6]
method=disabled
"""


def main():
    if os.geteuid() != 0:
        raise SystemExit("Run with sudo python3 systemd/prepare-hotspot.py")
    capability = subprocess.check_output(
        ["nmcli", "-g", "WIFI-PROPERTIES.AP", "device", "show", "wlan0"],
        env={**os.environ, "LC_ALL": "C"}, text=True).strip()
    if capability != "yes":
        raise SystemExit("wlan0 does not report AP support. No changes made.")
    if TARGET.exists():
        raise SystemExit("Hotspot profile already exists; leaving it unchanged.")
    password = getpass.getpass("Hotspot password (12-63 ASCII letters/digits): ")
    contents = profile(password)
    if password != getpass.getpass("Repeat password: "):
        raise SystemExit("Passwords do not match. No changes made.")
    fd, name = tempfile.mkstemp(prefix=".obotx-", dir=TARGET.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(contents)
        os.replace(name, TARGET)
    finally:
        Path(name).unlink(missing_ok=True)
    subprocess.run(["nmcli", "connection", "load", str(TARGET)], check=True)
    print("Hotspot prepared, NOT activated. Current Wi-Fi unchanged.")
    print("Follow docs/hotspot-installation.md to activate and verify it.")


if __name__ == "__main__":
    main()
