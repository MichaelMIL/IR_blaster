# ir_blaster — custom firmware for a Tuya IR blaster (CB3S / BK7231N)

PlatformIO project using the [LibreTiny](https://docs.libretiny.eu) platform
(Arduino-style API on the Beken BK7231N inside the CB3S module).

The starter firmware decodes NEC frames from the IR receiver and prints them
on the serial console, and sends NEC frames from serial commands.

## Build

```sh
pio run
```

## Wiring for serial flashing

The CB3S has castellated pads. You need four wires to a 3.3 V USB-UART adapter
(CP2102, CH340, FT232 ...). **Do not use 5 V.**

| CB3S pad | Adapter |
|----------|---------|
| 3V3      | 3.3 V   |
| GND      | GND     |
| TX1      | RX      |
| RX1      | TX      |

Notes:
- The module's own 3.3 V regulator on the board may be fed from the USB port
  of the blaster instead. Powering from the adapter's 3.3 V pin usually works,
  but the BK7231N draws up to ~300 mA on WiFi; a weak adapter will brown out.
  If flashing is flaky, power the blaster from its own USB cable and connect
  only GND, TX1 and RX1.
- TX1/RX1 are the download UART. TX2/RX2 is the log UART and will not flash.
- Some boards tie the Tuya MCU reset to the IR receiver power rail; nothing to
  do, just be aware the receiver stops while flashing.

## Flash

1. Plug in the adapter and find the port:
   ```sh
   ls /dev/cu.*
   ```
2. Put the port into `platformio.ini` (`upload_port = /dev/cu.usbserial-XXXX`)
   or pass it on the command line.
3. Start the upload, then power-cycle the module (unplug/replug 3V3 or short
   CEN/RST to GND briefly) while the flasher is waiting for the bootloader:
   ```sh
   pio run -t upload --upload-port /dev/cu.usbserial-XXXX
   ```
   LibreTiny retries for several seconds, so the reset just has to happen
   while it is polling.
4. Open the monitor:
   ```sh
   pio device monitor -b 115200
   ```

Before the first flash it is worth backing up the stock firmware:

```sh
pio pkg exec -- ltchiptool flash read -d /dev/cu.usbserial-XXXX -b 2000000 stock_backup.bin
```

## Pin mapping

Defaults in `platformio.ini`:

| Define        | GPIO | Role                              |
|---------------|------|-----------------------------------|
| `IR_TX_PIN`   | P26  | drives the transistor for the LEDs |
| `IR_RX_PIN`   | P8   | output of the IR receiver          |

These are the most common Tuya IR-blaster assignments, but brands differ.
If nothing is received, trace the receiver's OUT pin to the CB3S pad and
change the define. Common alternatives: RX on P7, P9 or P24; TX on P7 or P24.

CB3S pad to BK7231N GPIO reference:

```
P6  P7  P8  P9  P14 P16 P17 P20 P21 P22 P23 P24 P26 P28  ADC3  CEN
TX1 RX1 = P11 / P10    TX2 RX2 = P0 / P1
```

## Serial commands

```
send <addr> <cmd>      NEC with standard inverse bytes, e.g. send 0x04 0x08
raw  <hex32>           full 32-bit NEC word as printed by the receiver
help
```

Point any NEC remote (most TVs, set-top boxes, LED strips) at the blaster and
it prints lines like:

```
RX NEC addr=0x04 cmd=0x08 raw=0xF708FB04
```

## Next steps

- WiFi + MQTT (LibreTiny ships `WiFi.h`; PubSubClient works)
- Add more protocols (Samsung, Sony SIRC, RC5, raw timing capture/replay)
- OTA updates via `ArduinoOTA` or the LibreTiny OTA helper

## Toolchain notes (this machine)

- LibreTiny 1.13.0 fails on PlatformIO Core 6.2.0 with
  `TypeError: Library.__post_init__() missing 1 required positional argument: 'env'`.
  The VS Code extension forces core 6.2.0 (it re-upgrades on every restart), so
  `~/.platformio/platforms/libretiny/builder/utils/libs-queue.py` is patched locally:
  `Library.env` is a plain dataclass field instead of an `InitVar`. The original is
  kept next to it as `libs-queue.py.orig`. Re-apply if the platform is reinstalled.
- The PlatformIO package mirror (`sin1.contabostorage.com`) is DNS-blocked by the
  ASUS router on the home network, so core package downloads hang and retry
  endlessly. Allow that host in the router, or expect "stuck" installs.
- Flashing helper (`ltchiptool`) lives in `~/.platformio/penv/bin/ltchiptool`.
