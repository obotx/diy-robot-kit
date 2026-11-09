# SmartShunt Bluetooth battery monitor

The server reads encrypted Victron Instant Readout advertisements, not a paired
GATT connection. It never changes SmartShunt settings, drives outputs, stops motion,
or shuts down the Pi. The physical BMS remains responsible for battery protection.

## SmartShunt setup in VictronConnect

1. Power the SmartShunt and connect from inside VictronConnect, not from the
   phone's Bluetooth settings.
2. Enter the unique PIN printed on the product label. If the label has no unique
   PIN, use `000000`, then change it when prompted.
3. Update SmartShunt firmware if prompted.
4. Use battery-monitor mode, not DC energy-meter mode. For the 2 x 12.8 V /
   50 Ah series bank use the values below.
5. Open Settings > Product info and enable Instant readout via Bluetooth.
6. Open Instant readout details (Show). Obtain its Bluetooth MAC address and the
   32-character hexadecimal encryption key. This is NOT the six-digit pairing PIN.
7. Check that the device-list Instant Readout shows the expected battery percentage.

| Setting | ObotX value |
| --- | ---: |
| Battery capacity | 50 Ah |
| Charged voltage | 29.0 V, only with a verified 29.2 V final-charge voltage |
| Discharge floor | 20% |
| Tail current | 4.0% (2.0 A for 50 Ah) |
| Charged detection time | 3 min |
| Peukert exponent | 1.05 |
| Charge efficiency factor | 99% |
| Current threshold | 0.10 A |
| Time-to-go averaging | 3 min |
| Battery SOC on reset | Keep SOC |

Two 50 Ah batteries in series remain a 50 Ah bank. Do not enter 100 Ah. Set
`Charged voltage` approximately 0.2-0.3 V below the actual maintained charger
float/final voltage; adjust the 29.0 V starting value if the final charger behaves
differently. Do not mark a partly discharged battery as 100%. Fully charge it,
then allow automatic synchronisation or synchronise manually only after verifying
that the bank is full.

All battery charge and discharge current must pass through the shunt. Connect
only the battery to `BATTERY MINUS`; connect robot and charger negatives to
`SYSTEM MINUS` / `LOAD MINUS`. See the complete
[Victron commissioning procedure](../../../docs/power/victron-battery-commissioning.md).

## Install on Raspberry Pi

Run as user `obotx` through SSH:

```sh
cd ~/ObotX/software/rpi
sh systemd/install.sh
sh systemd/install-battery.sh
```

The second script installs BlueZ/rfkill, enables Bluetooth on boot, prompts locally
for the MAC and hidden encryption key, and restarts the webserver. It
does not change the selected STM32 profile. No `bluetoothctl pair` is required.
The server listens only to the configured MAC and Victron battery-monitor records.
The BLE listener retries after errors; its own thread does not block STM32 control.

The key is written atomically with Linux mode 0600 to
`/home/obotx/.config/obotx/battery.json`. It is not returned in the API, logs, or
repository. Set `OBOTX_BATTERY_CONFIG` to override this path if necessary.
Re-run `.venv/bin/python -m app.configure_battery` to replace it, then restart
the webserver. Do not paste the key in chat or command-line arguments.

## Alerts and data quality

- At or below 20%: recharge warning.
- At or below 5%: stop the robot, shut down the Raspberry Pi, then switch off power.
- Alerts clear with a two-percentage-point hysteresis (7% and 22%).
- No new valid packet for 15 seconds: hide the percentage and report lost data.
- Missing/unsynchronized SOC is unknown, never 0% or 100%.
- Last-known critical state remains warned about if readings disappear.
- No automatic shutdown or power disconnection is implemented.

The thresholds are operational defaults, not a battery-chemistry-specific safe
discharge guarantee. SOC comes from the SmartShunt, not from voltage estimation.
The upstream BLE format does not provide authenticated telemetry; use it for
operator information, not as a replacement for hardware protection.

## Verify after installation and reboot

```sh
systemctl is-active bluetooth obotx-battery-bluetooth obotx-rpi-control
bluetoothctl show
curl -s http://127.0.0.1:8000/api/battery
journalctl -u bluetooth -b -n 40 --no-pager
```

Expect `status: live`, `fresh: true`, and readings matching VictronConnect.
Compare the percentage before relying on the alerts. If `soc_unknown`, inspect
SmartShunt synchronization. If `decode_error`, verify the Instant Readout key.
If `waiting` or `stale`, check power, range, Instant Readout and the MAC.
If Bluetooth is unavailable, inspect the adapter/rfkill and BlueZ status; an
external adapter can be selected using `adapter` (for example `hci1`) in the file.

## Sources

- [Victron SmartShunt configuration](https://www.victronenergy.com/media/pg/SmartShunt/en/configuration.html)
- [Victron SmartShunt settings reference](https://www.victronenergy.com/media/pg/SmartShunt/en/all-features-and-settings.html)
- [Victron SmartShunt installation](https://www.victronenergy.com/media/pg/SmartShunt/en/installation.html)
- [VictronConnect Instant Readout](https://www.victronenergy.com/media/pg/VictronConnect_app/en/stored-trends---instant-readout.html)
- [Victron documentation of the Instant Readout details / key screen](https://www.victronenergy.com/media/pg/Blue_Smart_IP22_Charger_230V_manual/en/monitoring---bsc---psc.html)
- [Victron BLE advertising protocol announcement](https://communityarchive.victronenergy.com/questions/187303/victron-bluetooth-advertising-protocol.html)
- [victron-ble parser source and setup](https://github.com/keshavdv/victron-ble)

Python dependencies: victron-ble 0.9.3 and Bleak 3.0.2.
