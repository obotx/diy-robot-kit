# ObotX instructions

Follow these documents in order. Do not energise motor circuits before the
relevant wiring and commissioning checks are complete.

## Parts and mechanics

- [Bill of materials](../hardware/bom/bom.csv)
- [Mechanical BOM](../hardware/bom/mechanical-bom.csv)
- [Mechanical assembly](assembly/README.md)
- [CAD model](../hardware/cad/README.md)
- [Mechanical drawings](../hardware/drawings/README.md)
- [Four-finger gripper](../gripper/README.md)
- [3D-printing parameters](../hardware/cad/printed-parts/README.md)

## Electrical system

- [Complete electrical diagram](../hardware/wiring/ObotX_electrical_diagram.pdf)
- [Electrical-diagram page index](../hardware/wiring/README.md)
- [Battery, charging, BatteryProtect and SmartShunt](power/victron-battery-commissioning.md)
- [ZDM-2HA865 driver configuration](../software/stm32_arm/docs/zdm-2ha865-parameters.md)
- [System architecture](architecture/system-overview.md)
- [Software architecture](architecture/software-architecture.md)

## Firmware and control software

- [ARM controller firmware](../software/stm32_arm/README.md)
- [BASE controller firmware](../software/stm32_base/README.md)
- [Raspberry Pi installation](../software/rpi/INSTALL.md)
- [Raspberry Pi standalone hotspot installation](../software/rpi/docs/hotspot-installation.md)
- [Raspberry Pi operation](../software/rpi/README.md)
- [Wi-Fi, phone hotspot, Ethernet and SSH](../software/rpi/docs/network-connection.md)
- [Camera configuration](../software/rpi/docs/cameras.md)
- [SmartShunt telemetry](../software/rpi/docs/smartshunt.md)

## Commissioning

- [Safety rules](safety/safety-concept.md)
- [Commissioning checklist](testing/commissioning-checklist.md)
