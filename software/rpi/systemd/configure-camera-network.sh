#!/bin/sh
set -eu
# Dedicated camera switch; Wi-Fi remains the default route.
command -v nmcli >/dev/null
test -d /sys/class/net/eth0
profile=obotx-cameras
uuid=3210f2af-557a-4e2a-8c4c-5af179e338e9
if ! nmcli connection show uuid "$uuid" >/dev/null 2>&1; then
    sudo nmcli connection add type ethernet ifname eth0 con-name "$profile" \
        connection.uuid "$uuid" ipv4.method manual ipv4.addresses 192.168.50.10/24 \
        ipv4.never-default yes ipv6.method disabled
fi
sudo nmcli connection modify uuid "$uuid" connection.interface-name eth0 \
    connection.autoconnect yes connection.autoconnect-priority 100 \
    ipv4.method manual ipv4.addresses 192.168.50.10/24 \
    ipv4.gateway "" ipv4.dns "" ipv4.never-default yes ipv6.method disabled
sudo nmcli connection up uuid "$uuid"
ip -br address show eth0
