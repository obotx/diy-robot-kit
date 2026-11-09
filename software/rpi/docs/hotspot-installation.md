# Raspberry Pi standalone hotspot installation

This is the second Raspberry Pi installation option. It installs the ObotX
control server on Raspberry Pi OS and prepares a dedicated Wi-Fi hotspot:

- Wi-Fi network: `ObotX`
- Raspberry Pi and operator panel: `http://192.168.60.1:8000`
- camera Ethernet interface: `eth0`, `192.168.50.10/24`

The package does not contain Raspberry Pi OS, STM32 firmware, passwords, camera
credentials, the SmartShunt encryption key or private configuration. Internet
access is required while operating-system packages are installed, but not for
normal local control through the ObotX hotspot.

## 1. Prepare Raspberry Pi OS

1. Keep the existing microSD card as a backup or create an image of it.
2. In Raspberry Pi Imager select Raspberry Pi OS Lite (64-bit).
3. Set the hostname to `rpi-control`, create the user `obotx`, set a strong
   password, enable SSH and configure a temporary Wi-Fi network with Internet
   access.
4. Set the WLAN country to the country where the robot will operate.
5. Write the card and start the Raspberry Pi.
6. Keep motor power off. Connect the ARM and BASE NUCLEO boards over USB.
7. Connect the camera switch to `eth0`. Do not use `eth0` for installation.

## 2. Copy the installation package

Download [`ObotX-RPi-hotspot.zip`](../packages/ObotX-RPi-hotspot.zip). From
Windows PowerShell, run:

```powershell
scp "ObotX-RPi-hotspot.zip" obotx@rpi-control.local:/home/obotx/
ssh obotx@rpi-control.local
```

If `.local` name resolution is unavailable, use the address assigned to the
Raspberry Pi by the temporary Wi-Fi router. Confirm the Raspberry Pi identity
before removing an old SSH host key after reinstalling the operating system.

## 3. Install the control server and hotspot profile

Run on the Raspberry Pi:

```bash
sudo apt-get update
sudo apt-get install -y unzip
mkdir -p ~/ObotX/software/rpi
unzip -o ~/ObotX-RPi-hotspot.zip -d ~/ObotX/software/rpi
cd ~/ObotX/software/rpi
sh systemd/install-fresh.sh
```

Type `INSTALL` when requested. Set a service password and a separate hotspot
password containing 12-63 ASCII letters and digits. The installer stores the
hotspot password in a root-only NetworkManager profile and does not activate
the hotspot yet.

The cameras do not have the ObotX addresses by default. Before adding them to
the operator panel, follow the camera manufacturer's instructions, log in to
each camera separately and assign the static addresses `192.168.50.21` through
`192.168.50.24`. Use subnet mask `255.255.255.0`; use `192.168.50.10` as
gateway/DNS only if the camera requires those fields. Configure one camera at a
time to avoid duplicate factory addresses. See the
[camera configuration guide](cameras.md) for the complete procedure.

Configure SmartShunt while Internet access is still available:

```bash
sh systemd/install-battery.sh
```

Enter the Instant Readout MAC address and encryption key from VictronConnect,
not the Bluetooth pairing PIN. Then verify the installation:

```bash
systemctl is-active obotx-rpi-control
ip -br addr
curl -f http://127.0.0.1:8000/api/robot
```

## 4. Test the hotspot

Stop the robot and release operator control. The following command disconnects
the current Wi-Fi and SSH session, but does not enable hotspot autostart:

```bash
sudo systemd-run --unit=obotx-hotspot-start --collect /usr/bin/nmcli connection up uuid 65df7df3-880d-46af-b541-6d573d279955
```

Connect the operator computer to Wi-Fi `ObotX`, enter the configured password
and keep the connection despite any "no Internet" warning. Leave IPv4 on DHCP.
Open `http://192.168.60.1:8000` and verify the panel, cameras and battery data.

## 5. Enable hotspot at startup

Connect again from PowerShell:

```powershell
ssh obotx@192.168.60.1
```

Then run on the Raspberry Pi:

```bash
sudo nmcli connection modify uuid 65df7df3-880d-46af-b541-6d573d279955 connection.autoconnect yes
sudo reboot
```

Reconnect to `ObotX` after the reboot and repeat the panel, camera and battery
checks before testing motion.

## Recovery and switching back to external Wi-Fi

Before hotspot autostart is enabled, a reboot returns to the temporary Wi-Fi.
After autostart is enabled, use a monitor and keyboard if the hotspot cannot be
reached. To disable hotspot autostart and reconnect to another saved Wi-Fi:

```bash
nmcli -f NAME,UUID,TYPE connection show
sudo nmcli connection modify uuid 65df7df3-880d-46af-b541-6d573d279955 connection.autoconnect no
sudo nmcli connection up "YOUR_SAVED_WIFI_PROFILE"
```

Do not delete the camera Ethernet profile. For diagnostics, use:

```bash
journalctl -u NetworkManager -b -n 80 --no-pager
journalctl -u obotx-rpi-control -b -n 80 --no-pager
```
