# Camera configuration

ObotX uses four 3.6 mm non-PoE IP cameras. Power them from the camera supply
shown in the electrical diagram and connect their Ethernet ports to the robot
switch. Do not connect them to a PoE output.

## Network addresses

The camera network is `192.168.50.0/24`. The Raspberry Pi uses
`192.168.50.10`. The cameras do not use the ObotX addresses by default. The
person building the robot must assign a static address to each camera before it
can be used on the robot camera network. Configure these addresses only after
confirming that they are unused:

| Position | Address |
| --- | --- |
| Arm | `192.168.50.21` |
| Rear | `192.168.50.22` |
| Front left | `192.168.50.23` |
| Front right | `192.168.50.24` |

Use subnet mask `255.255.255.0`. A gateway is not required on the isolated
camera network. Change every factory password before operation.

## Configure a camera

1. Connect and power one camera at a time.
2. Follow the camera manufacturer's setup instructions to find its factory or
   DHCP address. Use the manufacturer's configuration tool or the DHCP client
   list of the temporary setup network when required.
3. Log in to the camera management page using the manufacturer's procedure and
   assign its static address from the table above. Do not assume that a new
   camera already has an address in the `192.168.50.0/24` network.
4. Enable RTSP and confirm that port `554` is used.
5. Disconnect the configured camera, then repeat for the remaining cameras one
   at a time to avoid duplicate factory addresses.

The Revotech stream paths are `/stream2` for the substream and `/stream1` for
the main stream. Use the substream for the operator-panel preview.

## Add cameras to the operator panel

Open `http://rpi-control.local:8000/`, enter camera settings and save each slot:

| Field | Value |
| --- | --- |
| Source | Revotech RTSP |
| Camera IP | Address from the table, without `http://` |
| RTSP port | `554` |
| Stream | Substream (`stream2`) |
| Username | Camera administrator username |
| Password | Camera password |

Settings are stored in `/home/obotx/.config/obotx/cameras.json` with Linux mode
`0600`. Do not commit or share this file. The panel is for a trusted robot LAN;
do not forward camera or panel ports to the Internet.

## Check operation

```bash
ffmpeg -version
systemctl is-active obotx-rpi-control
curl -s http://127.0.0.1:8000/api/cameras
```

If a preview is unavailable, first ping the camera address from the Raspberry
Pi, then verify power, address, credentials, RTSP port and stream path. Camera
video is for observation and is not a safety feedback channel.
