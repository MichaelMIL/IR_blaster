# Air-conditioner IR protocol

How the AC remote's signal is built, how it was reverse-engineered, and how
the firmware and `tools/ac_encode.py` generate a frame for any state.

The remote identifies itself with a constant byte `A5`; the brand is not
known, and it is not one of the protocols in IRremoteESP8266's table. None of
that matters: once the layout below is known, any state can be synthesised.

## 1. Physical layer

| Item | Value |
|------|-------|
| Carrier | 38 kHz (the blaster uses 38.46 kHz from hardware PWM) |
| Header | 6000 µs mark, 7300 µs space |
| Bit | 560 µs mark, then a space: **530 µs = 0**, **1590 µs = 1** (pulse-distance coding, like NEC) |
| Payload | 96 bits = 12 bytes, each byte **LSB first** |
| Trailer | 560 µs stop mark, 7300 µs gap, one 560 µs end pulse |
| Frame length | 197 pulses, about 144 ms (depends on how many 1-bits) |
| Repeat | one frame per key press, no repeats while held |

Timings as the receiver sees them vary by roughly ±60 µs; decoders should use
a 1000 µs threshold between a 0-space and a 1-space.

## 2. Payload layout

Byte index is in transmission order. "LSB first" means bit 0 of byte 0 is the
first data bit on the air. Hex values below are the bytes as a decoder
assembles them (bit 0 → value 1, bit 7 → value 128).

| Byte | Value | Meaning |
|------|-------|---------|
| 0 | `00` | constant |
| 1 | `00` | constant |
| 2 | `00` | constant |
| 3 | `01`, or `21` in heat mode | constant `01`; bit 5 (`0x20`) is set when mode = heat |
| 4 | `00` | constant |
| 5 | key code | which key was pressed: `00` power, `01` mode, `02` temp up, `03` temp down, `05` fan. Any value is accepted by the AC; it reacts to the state bytes. |
| 6 | `00` | constant |
| 7 | state | bit 1 (`0x02`) = power on; bits 5-6 = fan speed; bit 3 (`0x08`) always set (meaning unknown) |
| 8 | `00` | constant |
| 9 | mode/temperature | high nibble = mode, low nibble = temperature − 16 |
| 10 | `A5` | constant, probably a brand/model ID |
| 11 | checksum | `(byte0 + … + byte10) mod 256` |

### Byte 7, bit by bit

| Bits | Field | Values |
|------|-------|--------|
| 1 | power | 0 off, 1 on |
| 3 | unknown | always 1 in every capture |
| 5-6 | fan | `00` auto, `01` high, `10` medium, `11` low |

So byte 7 = `0x08 | (power ? 0x02 : 0) | (fan << 5)`:
fan auto on = `0A`, high on = `2A`, medium on = `4A`, low on = `6A`, off (fan auto) = `08`.

### Byte 9: mode and temperature

| Mode | High nibble |
|------|-------------|
| auto | 0 |
| cool | 2 |
| dry | 4 |
| fan | 6 |
| heat | 8 (also sets `0x20` in byte 3) |

Low nibble = temperature − 16, so 16 °C … 31 °C fit in 0 … 15.
Examples: cool 22 °C = `26`, cool 28 °C = `2C`, dry 25 °C = `49`, fan 28 °C = `6C`,
heat 28 °C = `8C`, auto 28 °C = `0C`.

### Not yet mapped

Swing was off in every capture. Record one press of the swing key with
`tools/ir_mapper.py`; `tools/ac_encode.py --verify` will print the byte it
cannot explain, which is where swing lives. The same applies to sleep, turbo,
timer and so on.

## 3. Worked example

Cool, 24 °C, fan high, on, sent with the "fan" key:

```
byte:  0  1  2  3  4  5  6  7  8  9  10 11
       00 00 00 01 00 05 00 2A 00 28 A5 FD
```

- byte 7 = `0x08 | 0x02 | (1 << 5)` = `2A`
- byte 9 = `(2 << 4) | (24 − 16)` = `28`
- checksum = `01 + 05 + 2A + 28 + A5` = `FD`

On the air, LSB first per byte:

```
00000000 00000000 00000000 10000000 00000000 10100000 00000000 01010100 00000000 00010100 10100101 10111111
```

Each `0` is a 560 µs mark + 530 µs space, each `1` a 560 µs mark + 1590 µs space,
preceded by the 6000/7300 µs header and followed by the stop mark, 7300 µs gap
and end pulse.

Off keeps the last mode, temperature and fan in the frame and only clears the
power bit: `00 00 00 01 00 00 00 08 00 2C A5 DA` = off, (cool 28 °C, fan auto).

## 4. Encoding algorithm

```
build(power, mode, temp, fan, key):
    b = [0]*12
    b[3]  = 0x01 | (0x20 if mode == heat else 0)
    b[5]  = key                          # 0 power, 1 mode, 2 temp+, 3 temp-, 5 fan
    b[7]  = 0x08 | (0x02 if power else 0) | (fan << 5)   # fan: auto 0, high 1, med 2, low 3
    b[9]  = (mode << 4) | (temp - 16)    # mode: auto 0, cool 2, dry 4, fan 6, heat 8
    b[10] = 0xA5
    b[11] = sum(b[0:11]) & 0xFF
    timings = [6000, 7300]
    for each byte, for bit in 0..7 (LSB first):
        timings += [560, 1590 if bit set else 530]
    timings += [560, 7300, 560]
```

Decoding is the reverse: take the space after every mark (skipping the
header), threshold at 1000 µs, pack bits LSB first into 12 bytes, check
`byte10 == A5` and the checksum, then read the fields.

Implementations: `src/ac.cpp` (`acBuildTimings`, `acDecodeTimings`) and
`tools/ac_encode.py` (`build_bytes`, `to_timings`, `parse_bytes`). Keep them in
sync when a new field is mapped.

## 5. How it was reverse-engineered

1. The receiver captured raw mark/space timings of every key press
   (`tools/ir_stream.py`, `tools/ir_mapper.py`). Early captures with the
   remote pressed against the receiver were garbage (saturation); 30-50 cm
   away they were clean and all had the same 197-pulse shape.
2. `tools/ir_analyze.py` showed constant marks and two space lengths, so the
   bits are in the spaces. 96 bits = 12 bytes.
3. The mapper tagged each capture with the state shown on the remote. A sweep
   of the temperature key (22 → 29 °C) changed only byte 9's low nibble by +1
   per degree and byte 11 by the same amount, which gave temperature and the
   additive checksum at once. Both were confirmed by `sum mod 256` matching on
   every frame.
4. Fan presses changed bits 5-6 of byte 7; the power key cleared bit 1 of
   byte 7; the mode key changed byte 9's high nibble (and set `0x20` in byte 3
   for heat). Byte 5 differed between presses of *different keys* with the
   same resulting state, which identified it as the key code.
5. `tools/ac_encode.py --verify ac_map.json` regenerates every captured frame
   from its tag and compares bit-exactly: 19 of 19 match. The firmware's
   encoder was then checked by sending frames with the receiver kept on
   (`POST /api/ac?...&loopback=1`) and decoding what came back.

## 6. Using it

| Interface | Command |
|-----------|---------|
| Serial | `ac cool 24 high`, `ac heat 22`, `ac 26`, `ac low`, `ac off` (any order of mode / temperature / fan / on / off) |
| HTTP | `POST /api/ac?power=on&mode=cool&temp=24&fan=high` (any subset; missing fields keep their value), `GET /api/ac` |
| Web page | "Air conditioner" panel |
| MQTT | `<base>/cmd/ac` with `{"power":"on","mode":"cool","temp":24,"fan":"high"}`; or the climate topics `<base>/ac/mode/set`, `/ac/temp/set`, `/ac/fan/set` |
| Home Assistant | a `climate` entity "Air conditioner" appears through MQTT discovery (modes off/auto/cool/dry/fan_only/heat, 16-31 °C, fan auto/low/medium/high) |
| Python | `tools/ac_encode.py --mode heat --temp 22 --fan low --send` |

The last state sent is kept in flash so the climate entity shows it after a
reboot. The AC itself has no feedback channel, so the state reflects what the
blaster sent, not what the remote may have changed since.
