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

1. Plug in the adapter and find the port (the name changes with the USB socket):
   ```sh
   ls /dev/cu.usbserial*
   ```
2. Flash at 115200 baud. The default 921600 died mid-write on this wiring.
   ```sh
   ~/.platformio/penv/bin/ltchiptool flash write -d /dev/cu.usbserial-XXXX -b 115200 -t 150 .pio/build/cb3s/firmware.uf2
   ```
   or, via PlatformIO (uses `upload_speed` / `upload_port` from `platformio.ini`):
   ```sh
   pio run -t upload --upload-port /dev/cu.usbserial-XXXX
   ```
3. The first time (stock Tuya firmware) the chip must be reset while the
   flasher polls: short CEN to GND for half a second, or drop the module's 3V3.
   Once this firmware is on the chip, `Serial` lives on UART1 and LibreTiny's
   auto-download-reboot kicks the chip into the bootloader by itself.
4. Open the monitor:
   ```sh
   pio device monitor -b 115200
   ```

Lessons from the first flash:
- Power the blaster from its own USB cable, not from the adapter's 3.3 V pin.
  The module's boot inrush reset the USB-serial chip and the port vanished.
- Keep the USB adapter plugged in; reset only the module.
- `Serial` on BK72xx defaults to UART2 (TX2/RX2). `-D LT_UART_DEFAULT_PORT=1`
  in `platformio.ini` moves it to UART1 so the flash pads double as console.

## Pin mapping

Pins are assigned at runtime from the serial console and stored in flash
(first 4 KiB of the `userdata` partition). Defaults come from `platformio.ini`
(`IR_TX_PIN` = LED 1, `IR_RX_PIN` = receiver). `reset` restores them.

Candidate GPIOs on the CB3S: P6 P7 P8 P9 P14 P20 P21 P22 P23 P24 P26.
P10/P11 are the serial console and are refused.

### Finding the pins (test mode)

```
rx scan 10          # press remote buttons at the device; reports which GPIO toggles
rx pin P7           # assign it
rx test 15          # live readings: edges/s, decoded NEC frames, idle level
led scan 2          # each candidate GPIO blinks for 2 s - watch LEDs through a phone camera
led 1 pin P26       # assign the LED that blinked (repeat for led 2 / led 3)
led 1 test 3        # blink only LED 1 for 3 s
led all test        # blink all configured LEDs
```

A working receiver idles HIGH. `rx test` printing `idle level: LOW` and
`0 edges/s` means that GPIO is not the receiver output. If the three LEDs are
driven by one transistor, only one GPIO will blink them all; leave the other two
as `none`.

## WiFi

```
wifi ssid My Network      # spaces allowed
wifi pass hunter2
wifi connect              # also auto-connects at every boot once saved
wifi status               # ip, rssi
wifi clear
```

Hostname is `ir-blaster`. Nothing listens on the network yet; the next step is
an HTTP or MQTT endpoint for `send`.

## Serial commands

```
status                     firmware / wifi / pin summary
send <addr> <cmd>          NEC with inverse bytes on all configured LEDs, e.g. send 0x04 0x08
raw <hex32>                full 32-bit NEC word as printed by the receiver
led <1-3> pin <P#|none>    led <1-3> test [sec]   led all test [sec]   led scan [sec]
rx pin <P#|none>           rx test [sec]          rx scan [sec]
reset                      reboot                 help
```

Every received frame is printed as it arrives:

```
RX NEC addr=0x04 cmd=0x08 raw=0xF708FB04
RX raw 36 pulses (not NEC)         <- another protocol; still proves the pin
```

## Next steps

- HTTP / MQTT endpoint so Home Assistant can call `send`
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
