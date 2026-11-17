# AutotermProxy

A transparent UART proxy for monitoring and injecting frames between an
Autoterm controller and heater, using an Arduino Mega 2560.

The firmware relays bytes in both directions, prints observed frames to the USB
serial port, and allows frames to be sent to the heater from a serial terminal
or a home automation tool such as Node-RED.

> **Community project. Not affiliated with or endorsed by Autoterm.**
> Compatibility and operation must be verified with your specific controller
> and heater model.

## Features

- Bidirectional UART relay between the controller and the heater.
- Byte-by-byte forwarding: relayed frames are not filtered.
- USB output of received frames, including CRC validation status.
- Injection of hexadecimal frames to the heater.
- Length and CRC validation for injected frames.
- Rejects injection when the bus appears to be busy.

## Hardware

- Arduino Mega 2560 or compatible board with `Serial1` and `Serial2`.
- UART connection between the controller and the Arduino.
- UART connection between the heater and the Arduino.
- Electrical wiring and signal-level adaptation appropriate for your setup.

### Pin assignments

| Connection | Arduino UART | Mega pins |
|---|---|---|
| Controller | `Serial1` | RX1 = 19, TX1 = 18 |
| Heater | `Serial2` | RX2 = 17, TX2 = 16 |
| PC / home automation | `Serial` | USB |

On the Mega 2560, `Serial1` uses pins 19 (RX) and 18 (TX), while `Serial2`
uses pins 17 (RX) and 16 (TX). Check the pinout if you are using a clone or
compatible board. [Arduino Mega 2560 reference](https://docs.arduino.cc/hardware/mega-2560)

### Wiring

Cross the UART transmit and receive lines:

- Controller TX → Arduino RX1
- Controller RX ← Arduino TX1
- Arduino TX2 → Heater RX
- Arduino RX2 ← Heater TX
- Common ground, **only if required and electrically compatible with your setup**

**Important:** before connecting the UART lines, verify their voltage levels to be 5V.

## Installation

1. Open `firmware/AutotermProxy/AutotermProxy.ino` in the Arduino IDE.
2. Select your Mega 2560 or compatible board.
3. Upload the firmware.
4. Open the serial monitor at **115200 baud**.
5. Set the controller and heater UART links to **9600 baud**.

On startup, the firmware prints:

```text
PROXY READY
