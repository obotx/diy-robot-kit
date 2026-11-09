# Connecting to Raspberry Pi

The Raspberry Pi hostname is `rpi-control` and the default service user is
`obotx`. The control panel listens on port `8000`.

## Installed robot configuration

| Interface | Purpose | Address |
| --- | --- | --- |
| `wlan0` | Operator access, SSH and control panel | Assigned by Wi-Fi DHCP |
| `eth0` | Dedicated camera network | `192.168.50.10/24` static |

The Wi-Fi address can change when the router or hotspot assigns a new lease.
Check it with `ip -4 address show wlan0`, or reserve an address in the router by
the Raspberry Pi Wi-Fi MAC address. The panel uses port `8000`, for example
`http://<RPi-Wi-Fi-address>:8000/`.

## Connect Raspberry Pi to Wi-Fi

Raspberry Pi OS Bookworm and newer use NetworkManager. From a local terminal or
an existing SSH connection:

```bash
sudo raspi-config
```

Set `Localisation Options -> WLAN Country`, then exit. List the available
networks and connect without placing the password in shell history. Replace
`<WI-FI_NETWORK_NAME>` with the actual name (SSID) of your Wi-Fi network or
phone hotspot. `ObotX-Control` is only an example network name; it is not a
required name.

```bash
nmcli dev wifi list
sudo nmcli --ask dev wifi connect "<WI-FI_NETWORK_NAME>"
```

Enter the Wi-Fi password when prompted. Verify the connection and address:

```bash
nmcli connection show --active
ip -4 address show wlan0
```

Use the address shown after `inet` to connect from the operator computer:

```bash
ssh obotx@<RPi-Wi-Fi-address>
```

Open `http://<RPi-Wi-Fi-address>:8000/` in a browser. `rpi-control.local` can
also be used where mDNS works, but the displayed or reserved IP address is the
reference address.

Configure a new Wi-Fi connection from a local keyboard or while SSH is connected
through Ethernet. Changing `wlan0` while the SSH session itself uses `wlan0`
disconnects the terminal before an interactive password prompt can complete.

## Phone hotspot

A phone hotspot can provide a local Wi-Fi network even without mobile Internet.
Set it to 2.4 GHz and WPA2 compatibility if the Raspberry Pi cannot see or join
it. Connect both the Raspberry Pi and operator computer to the same hotspot.
The hotspot assigns the Raspberry Pi address; read it from the phone's client
list or with `ip -4 address show wlan0`. Do not assume any fixed address.

Some phones isolate hotspot clients. If the computer cannot ping the assigned
RPi address, use the robot router or a service cable instead.

## Ethernet addressing

The Raspberry Pi Ethernet port has the fixed address `192.168.50.10/24`. The
`/24` prefix means that devices communicating with it must use an unused address
from the same `192.168.50.x` subnet and the mask `255.255.255.0`.

Use the following service-computer configuration:

| Device | Address | Subnet mask |
| --- | --- | --- |
| Raspberry Pi `eth0` | `192.168.50.10` | `255.255.255.0` |
| Service computer Ethernet | `192.168.50.100` | `255.255.255.0` |

Leave the gateway and DNS fields empty. The computer's Wi-Fi address is not
used for this connection.

## Connect directly with an Ethernet cable

1. Disconnect the Raspberry Pi Ethernet port from the camera switch.
2. Connect a normal Ethernet cable directly between the Raspberry Pi and the
   service computer.
3. On Windows open
   `Settings -> Network & internet -> Ethernet -> IP assignment -> Edit`.
4. Select `Manual`, enable IPv4 and enter:

   | Setting | Value |
   | --- | --- |
   | IP address | `192.168.50.100` |
   | Subnet mask | `255.255.255.0` |
   | Subnet prefix length, if shown instead of a mask | `24` |
   | Gateway | leave empty |
   | DNS | leave empty |

5. Test the connection in PowerShell:

   ```powershell
   ping 192.168.50.10
   ssh obotx@192.168.50.10
   ```

6. Open `http://192.168.50.10:8000/` in a browser.

After service work, reconnect the Raspberry Pi to the camera switch and change
the Windows Ethernet IP assignment back to `Automatic (DHCP)` if the computer
normally obtains its address automatically.

The same computer settings can be used while connecting through the installed
camera switch instead of a direct cable. Before choosing `192.168.50.100`, make
sure that no camera or other device already uses that address.

Do not expose port `8000`, SSH or camera services to the Internet. Wi-Fi and the
browser STOP control are not a replacement for the hardware emergency stop.

## Reference

- [Raspberry Pi network configuration](https://www.raspberrypi.com/documentation/computers/configuration.html)
