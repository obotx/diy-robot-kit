# System Architecture

## Functional blocks

| Block | Responsibility |
| --- | --- |
| External control system | Sends robot commands and receives telemetry through the published API. |
| Raspberry Pi 4B | Hosts the operator panel and API, handles cameras and SmartShunt data, and bridges commands to both STM32 controllers. |
| STM32 BASE | Runs mecanum kinematics, encoder speed control, output ramps, watchdog and base fault handling. |
| STM32 ARM | Controls ZL, ZR, X and the four-finger gripper; performs homing, limits, watchdog and arm fault handling. |
| Motor drivers | MDD10A drivers power the base motors; closed-loop stepper drivers power Z and X; Pololu 2973 powers the gripper. |
| Power system | Two series-connected LiFePO4 batteries, fused distribution, BatteryProtect, SmartShunt and DC/DC rails. |
| Safety chain | Hardware emergency stop/motor-power isolation plus independent STM32 and Raspberry Pi communication timeouts. |

## Connections

```text
External control system / operator browser
                |
        Wi-Fi or Ethernet
                |
         Raspberry Pi 4B
          |            |
       USB serial   USB serial
          |            |
      STM32 ARM     STM32 BASE
          |            |
      Z / X /       2 x MDD10A
      gripper           |
                   4 base motors

Non-PoE cameras -- Ethernet/RTSP --> Raspberry Pi --> browser previews
SmartShunt -- Bluetooth Instant Readout -------> Raspberry Pi
```

The Raspberry Pi handles networking, the user interface, configuration and
telemetry aggregation. It does not generate STEP, PWM or other timing-critical
motor signals. These signals and the local motion watchdogs run on STM32.

Camera video is processed separately from the control WebSocket. Video loss or
a stale image is never used as a motion-safety signal.
