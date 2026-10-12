# IR blaster — custom firmware for a Tuya CB3S IR remote

Open firmware for the cheap Tuya WiFi IR blasters built around the **CB3S**
module (Beken BK7231N): one IR receiver, three IR LEDs, no cloud. Built with
PlatformIO and [LibreTiny](https://docs.libretiny.eu).

What it does:

- **Learn and replay any remote** — raw capture of the whole key press, named
  recordings stored in flash, replay through the LEDs with a hardware-PWM
  38 kHz carrier.
- **NEC remotes natively** — decode and send by address/command.
- **Air conditioner natively** — the AC remote's protocol was reverse-engineered
  (see [docs/AC_PROTOCOL.md](docs/AC_PROTOCOL.md)); set mode, temperature and
  fan directly, no recordings needed.
- **Web UI** — an admin page for everything, and a plain big-button remote
  page (`/remote`) for everyday use.
- **Home Assistant** — MQTT discovery publishes a climate entity for the AC,
  a button entity per remote button and a "last IR code" sensor.
- **OTA updates**, a JSON API, a serial console, and PC tools for streaming,
  tagging and analysing captures.

## Contents

- [Hardware](#hardware)
- [Build and flash](#build-and-flash)
- [First setup](#first-setup)
- [Web pages](#web-pages)
- [Air conditioner](#air-conditioner)
- [Recording any remote](#recording-any-remote)
- [Home Assistant / MQTT](#home-assistant--mqtt)
- [HTTP API](#http-api)
- [Serial console](#serial-console)
- [PC tools](#pc-tools)
- [Troubleshooting and lessons learned](#troubleshooting-and-lessons-learned)
- [Repository layout](#repository-layout)

## Hardware

| Part | Notes |
|------|-------|
| Module | CB3S (BK7231N, 120 MHz, 256 KB RAM, 1 MB flash), castellated pads |
| IR receiver | output on **P7** on this board (found with `rx scan`) |
| IR LEDs | three LEDs driven by one transistor from **P26** |
| Status LED | blue LED on **P8** (optional, configurable) |
| Serial | UART1: **TX1 = P11, RX1 = P10**, 115200 baud; used for the console *and* for flashing |

Pins are configured at runtime and stored in flash, so other boards of the
same family only need `rx scan` / `led scan` to find theirs. PWM-capable
GPIOs (needed for the LED carrier): P6 P7 P8 P9 P24 P26.

## Build and flash

```sh
pio run                       # build  -> .pio/build/cb3s/firmware.uf2
```

### First flash: serial

Wire a **3.3 V** USB-UART adapter (never 5 V): adapter RX → TX1, adapter
TX → RX1, GND → GND. Power the blaster from its own USB cable, not from the
adapter's 3.3 V pin (the boot inrush resets the adapter and the port
disappears). Then:

```sh
ls /dev/cu.usbserial*                                   # the name changes with the USB socket
~/.platformio/penv/bin/ltchiptool flash write -d /dev/cu.usbserial-XXXX -b 115200 -t 150 .pio/build/cb3s/firmware.uf2
```

With the stock Tuya firmware the chip must be reset *while the tool polls*:
short **CEN** to GND for half a second or drop the module's 3V3. 921600 baud
dropped the link mid-write on this wiring; 115200 is reliable. Once this
firmware runs, the console lives on UART1 and the flasher reboots the chip by
itself.

### Every later update: OTA

```sh
pio run -e cb3s_ota -t upload --upload-port ir-blaster.local   # or the IP
```

Builds and POSTs the UF2 to `/update`; the device verifies it, reboots in
~8 s, and keeps its settings and recordings. The web page has the same under
**Firmware update**. MQTT is suspended during an upload (see RAM notes).

## First setup

Open a serial monitor (`pio device monitor -b 115200`) and type `help`.

```
wifi ssid My Network          # spaces allowed
wifi pass hunter2
wifi connect                  # auto-connects at every boot from now on
rx scan 10                    # press any remote at the device -> reports the receiver GPIO
rx pin P7
led scan 2                    # each candidate GPIO blinks; watch the LEDs through a phone camera
led 1 pin P26
status
```

The console prints the URL once WiFi is up; from then on everything can be
done from the browser, including the above under **Pins** and **WiFi**.

## Web pages

- **`/`** — admin: send NEC, air conditioner, received-frame log, live capture
  stream, recordings, remote buttons, pins and test helpers, WiFi, MQTT, OTA,
  export, status (heap, reboot reason, …).
- **`/remote`** — a grid of large buttons for phones. Buttons are defined on
  the admin page: a label plus a NEC code or a recording, optionally repeated.

Hostname `ir-blaster.local` (mDNS) or the IP from the console / router.

## Air conditioner

The AC remote's 12-byte protocol is fully decoded; the firmware builds frames
itself. Details, byte map and the reverse-engineering story are in
[docs/AC_PROTOCOL.md](docs/AC_PROTOCOL.md).

```
serial:  ac cool 24 high     ac heat 22     ac 26     ac low     ac off
http:    POST /api/ac?power=on&mode=cool&temp=24&fan=high      GET /api/ac
mqtt:    <base>/cmd/ac  {"power":"on","mode":"cool","temp":24,"fan":"high"}
         or the climate topics <base>/ac/mode/set, /ac/temp/set, /ac/fan/set
HA:      "Air conditioner" climate entity via MQTT discovery
python:  tools/ac_encode.py --mode heat --temp 22 --send
```

Modes auto/cool/dry/fan/heat, 16-31 °C, fan auto/low/med/high. Swing and
other extras are not mapped yet; record one press of each with the mapper
and `tools/ac_encode.py --verify` points at the byte.

## Recording any remote

Every received frame is captured as raw mark/space timings (up to 800
pulses; multi-part frames with gaps under 80 ms stay together). Recordings
are stored by name in flash (32 slots) and survive reboots.

```
rec ac_old_remote_on          # waits up to 15 s for the next key press and stores it
rec a b c                     # one key press per name
save <name>                   # store the last received frame instead
play <name>                   # retransmit through all LEDs
list / dump <name> / del <name>
stream on                     # print every received frame with raw timings (live)
```

Web: **Live capture → Start stream** lists frames as they arrive with a
"save…" button each; **Recordings** has play/delete; **Record a sequence**
takes a comma-separated list of names. Receiver dropouts (pulses under
250 µs) are merged on save and on play.

NEC remotes decode directly: frames print as `RX NEC addr=0xBA cmd=0x1E`
and can be resent with `send 0xBA 0x1E` or `raw 0xE11E45BA`.

## Home Assistant / MQTT

Set the broker under **Home Assistant (MQTT)** on the admin page or with
`mqtt host <host> [port]`, `mqtt user …`, `mqtt pass …`. With MQTT discovery
enabled in HA (the default) the device appears as "IR Blaster" with:

- a **climate** entity for the AC (modes off/auto/cool/dry/fan_only/heat,
  16-31 °C, fan auto/low/medium/high); its state is what the blaster last
  sent and is kept across reboots
- one **button** entity per remote button, kept in sync with the button list
- a **Last IR code** sensor with the full frame as attributes
- availability from the last-will `status` topic

Topics (`<base>` = `ir_blaster/<last 3 MAC bytes>`, shown on the Status panel):

```
<base>/status        online | offline (retained)
<base>/ac/state      {"power":"on","mode":"cool","temp":24,"fan":"high","hvac_mode":"cool","fan_mode":"high"} (retained)
<base>/received      {"code":"NEC BA/1E","type":"nec","addr":186,"cmd":30,"raw":"0xE11E45BA","pulses":67}
<base>/result        {"ok":true,"message":"…"} after every command
<base>/ac/mode/set   off | auto | cool | dry | fan_only | heat      (HA climate)
<base>/ac/temp/set   16..31
<base>/ac/fan/set    auto | low | medium | high
<base>/cmd/ac        {"power":"on","mode":"cool","temp":24,"fan":"high"}   (any subset)
<base>/cmd/button    button index or label
<base>/cmd/send      "0xBA 0x1E" or {"addr":"0xBA","cmd":30}
<base>/cmd/raw       32-bit NEC word
<base>/cmd/play      recording name
```

## HTTP API

All `POST`s take query-string arguments.

```
GET  /api/status                       firmware, wifi, pins, heap, reboot reason, mqtt, ac state
GET  /api/ac   POST /api/ac?power=&mode=&temp=&fan=[&loopback=1]
POST /api/send?addr=&cmd=  |  ?raw=0x…  [&loopback=1]     loopback keeps the receiver on (self-test)
GET  /api/received         POST /api/received/clear
GET  /api/stream?after=<id>            last captures with full timings
POST /api/stream/save?id=&name=        POST /api/stream/clear
GET  /api/recordings   POST /api/play?name=   POST /api/record?name=&sec=   POST /api/record/last?name=
POST /api/recordings/put?name=   (body: comma-separated timings)   POST /api/recordings/delete?name=
GET  /api/export       GET /api/last          JSON of all recordings / last capture
GET  /api/buttons      POST /api/button?id=   POST /api/buttons/set?label=&type=nec|rec&addr=&cmd=&rec=&repeat=
POST /api/buttons/move?id=&dir=-1|1    POST /api/buttons/delete?id=
POST /api/pins?led1=&led2=&led3=&rx=&status=&status_invert=
POST /api/test?led=1|2|3|all&sec=      POST /api/blink?pin=&sec=      POST /api/scan?sec=
POST /api/wifi?ssid=&pass=   POST /api/mqtt?host=&port=&user=&pass=   POST /api/reboot
POST /update                           multipart "file" = firmware.uf2 (OTA)
```

## Serial console

115200 baud on UART1. `help` lists everything; the main groups:

```
status | reboot | reset                         wifi ssid|pass|connect|status|clear
ac …                                            mqtt host|user|pass|clear
send <addr> <cmd> | raw <hex32>                 rec | save | play | list | del | dump | export | stream
led <1-3> pin|test, led all test, led scan      rx pin|test|scan
sled pin|invert|test (status LED)               carrier <hz>
txtest <name> | txtest nec <addr> <cmd>         txcal | txmode pwm|bitbang | txcomp <mark> <space>
```

`txtest` is a loopback: it plays a frame with the receiver kept on and
compares what came back (the receiver sees the LEDs directly on this board).

## PC tools

All in `tools/`, standard library only. On macOS run them with
**`/usr/bin/python3`**: other Python builds are blocked from the local network
by the "Local Network" privacy setting and fail with "No route to host".

| Tool | Purpose |
|------|---------|
| `ir_mapper.py` | **Key mapper GUI** (local web app). Shows captures live in a state table with dropdowns (power, mode, temp, fan, swing, extra); a new capture copies the previous row and steps the temperature up; tags are generated from the state; compare two captures bit by bit; upload captures to the device; export. `--session file.json`, `--host`. |
| `ac_encode.py` | Build / send AC frames from a state; `--verify session.json` regenerates every tagged capture and compares bit-exactly; `parse_bytes` decodes a frame into a state and reports unexplained bytes. |
| `ir_analyze.py` | Generic analysis of raw captures: glitch merging, section split, header detection, mark- vs space-coded bits, bytes LSB/MSB-first, protocol-family guess. Reads `/api/export` JSON and `ir_stream.py` JSONL. |
| `ir_stream.py` | Follow the device's capture stream and append every frame to a JSONL file; `--prompt` names and saves each on the device. |

## Troubleshooting and lessons learned

- **Replay reads fine but the AC ignores it** → the software bit-bang
  carrier measured only 31-33 kHz on this chip; the firmware now uses
  hardware PWM (38.46 kHz). `txcal` shows the figures.
- **Frames come out corrupted at random** → the WiFi task preempted the
  transmission; transmission now runs at top task priority. Suspending the
  scheduler instead crashed the device.
- **Captures of the remote are garbage** (hundreds of 1.6 ms marks) →
  receiver saturation; hold the remote 30-50 cm away.
- **OTA upload drops ("Broken pipe")** → heap exhaustion. The BK7231N leaves
  ~72 KB of heap; below ~20 KB free, uploads and large responses fail. Static
  buffers are sized to keep ~30 KB free, MQTT is suspended (and its buffer
  released) during an upload, and a broken upload is aborted so it cannot
  leak. `GET /api/status` shows `heap.free` / `heap.min_free`.
- **Admin page blank** → pages over ~16 KB cannot be handed to the stack in
  one write; they are sent in 1 KB chunks.
- **Device "unreachable" from a Python script but fine with curl** → macOS
  Local Network privacy; use `/usr/bin/python3` or allow the interpreter.
- **The build fails inside VS Code** with `Library.__post_init__() missing
  'env'` → LibreTiny 1.13.0 vs PlatformIO Core 6.2.0; the platform's
  `builder/utils/libs-queue.py` is patched locally (`Library.env` as a plain
  field). Re-apply after a platform reinstall. PlatformIO's package mirror is
  blocked by some routers, which looks like a hung build.
- **Receiver chatter** → single-edge glitches are dropped (frames under
  6 pulses); the stream and received list only show real frames.
- `GET /api/status` → `reboot_reason` says whether the last restart was a
  software reboot, watchdog, crash or power-up.

## Repository layout

```
platformio.ini          cb3s (serial) and cb3s_ota (OTA) environments, pin defaults
src/main.cpp            setup/loop, serial console, capture ring, test helpers
src/ir_nec.cpp          IR receiver (interrupt driven), sender (PWM carrier), NEC codec
src/ac.cpp              native AC protocol encoder/decoder
src/web_ui.cpp          HTTP API, admin page, remote page, OTA
src/mqtt.cpp            MQTT client, Home Assistant discovery
src/recordings.cpp      named raw recordings in flash
src/buttons.cpp         remote-page buttons in flash
src/config.cpp          settings in flash (with migration between versions)
lib/PubSubClient        vendored MQTT library
tools/                  ir_mapper.py, ac_encode.py, ir_analyze.py, ir_stream.py
docs/AC_PROTOCOL.md     the AC protocol
ac_map.json             the tagged AC captures the protocol was derived from
```

Flash layout: app partitions for OTA, plus the `userdata` partition holding
the config block, 32 recording slots (4 KB each) and the button table.
