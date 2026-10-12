#!/usr/bin/env python3
"""Build (and optionally send) frames for the AC remote mapped in ac_map.json.

Frame: 12 bytes, each sent LSB first, pulse-distance coded:
  header 6000 us mark / 7300 us space, bit = 560 us mark + 530 us (0) / 1590 us (1) space,
  stop mark 560 us, 7300 us gap, one 560 us end pulse. 38 kHz carrier.

  byte  meaning
  0-2   00 00 00
  3     01, plus 0x20 in heat mode
  4     00
  5     key that was pressed: 00 power, 01 mode, 02 temp up, 03 temp down, 05 fan
        (goes into the checksum; the AC acts on the state bytes, any value works)
  6     00
  7     bit1 = power on (0x02); bits 5-6 = fan: 00 auto, 01 high, 10 medium, 11 low; 0x08 always set
  8     00
  9     high nibble = mode: 0 auto, 2 cool, 4 dry, 6 fan, 8 heat;  low nibble = temperature - 16
  10    A5                       (constant)
  11    checksum = sum(bytes 0..10) & 0xFF
  swing: not captured yet (all captures were swing off)

usage:
  ac_encode.py --mode cool --temp 24 --fan high           # prints bytes + timings
  ac_encode.py --mode heat --temp 22 --send               # also sends it through the blaster
  ac_encode.py --off --send
  ac_encode.py --verify ac_map.json                       # decode every capture and compare with its tag
"""
import argparse, json, sys, os, urllib.request, urllib.parse

FAN = {"auto": 0, "high": 1, "med": 2, "low": 3}
MODE = {"auto": 0, "cool": 2, "dry": 4, "fan": 6, "heat": 8}
KEY = {"power": 0x00, "mode": 0x01, "temp_up": 0x02, "temp_down": 0x03, "fan": 0x05}
HDR_MARK, HDR_SPACE, BIT_MARK, ZERO, ONE, END_GAP = 6000, 7300, 560, 530, 1590, 7300

def build_bytes(power=True, mode="cool", temp=24, fan="auto", key="temp_up"):
    if not 16 <= temp <= 31: raise ValueError("temp 16..31")
    b = [0, 0, 0, 0x01, 0, KEY[key], 0, 0, 0, 0, 0xA5, 0]
    if mode == "heat": b[3] |= 0x20
    b[7] = (0x02 if power else 0) | 0x08 | (FAN[fan] << 5)
    b[9] = (MODE[mode] << 4) | ((temp - 16) & 0x0F)
    b[11] = sum(b[:11]) & 0xFF
    return b

def parse_bytes(b):
    """inverse of build_bytes: state dict from 12 bytes (None if it is not this protocol)"""
    if len(b) != 12 or b[10] != 0xA5 or b[11] != (sum(b[:11]) & 0xFF): return None
    fan = {v: k for k, v in FAN.items()}.get((b[7] >> 5) & 3, "?")
    mode = {v: k for k, v in MODE.items()}.get(b[9] >> 4, f"?{b[9] >> 4}")
    key = {v: k for k, v in KEY.items()}.get(b[5], f"?{b[5]:02X}")
    extra = []
    if b[3] & ~0x21: extra.append(f"b3=0x{b[3]:02X}")
    if b[7] & ~0x6A: extra.append(f"b7=0x{b[7]:02X}")
    for i in (0, 1, 2, 4, 6, 8):
        if b[i]: extra.append(f"b{i}=0x{b[i]:02X}")
    return {"power": "on" if b[7] & 0x02 else "off", "mode": mode, "temp": 16 + (b[9] & 0x0F),
            "fan": fan, "key": key, "unexplained": extra}

def to_bits(b):
    return "".join(format(x, "08b")[::-1] for x in b)       # LSB first per byte

def to_timings(b):
    t = [HDR_MARK, HDR_SPACE]
    for bit in to_bits(b):
        t += [BIT_MARK, ONE if bit == "1" else ZERO]
    t += [BIT_MARK, END_GAP, BIT_MARK]
    return t

def decode_bits(timings):
    """bits of a captured frame (same layout), or None if the shape is wrong"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import ir_analyze as A
    t, _ = A.clean_glitches(timings)
    if len(t) < 2 + 192: return None
    return "".join("1" if s > 1000 else "0" for s in t[3:3 + 192:2])

def send(host, b, name="_ac_tmp"):
    body = ",".join(map(str, to_timings(b))).encode()
    def post(path, data=None):
        req = urllib.request.Request(f"http://{host}{path}", method="POST", data=data, headers={"Content-Type": "text/plain"} if data else {})
        return json.load(urllib.request.urlopen(req, timeout=15))
    r = post("/api/recordings/put?" + urllib.parse.urlencode({"name": name}), body)
    if r.get("ok"):
        r = post("/api/play?name=" + name)
        post("/api/recordings/delete?name=" + name)
    return r

def verify(path):
    d = json.load(open(path)); ok = bad = 0
    for c in d["captures"]:
        tag = c.get("tag", ""); st = c.get("state") or {}
        got = decode_bits(c["timings"])
        if not got: print(f"#{c['id']:<3} {tag:<20} undecodable ({c['pulses']} pulses)"); bad += 1; continue
        gb = [int(got[i:i+8][::-1], 2) for i in range(0, 96, 8)]
        p = parse_bytes(gb)
        if not p: print(f"#{c['id']:<3} {tag:<20} checksum/ID mismatch: {' '.join(f'{x:02X}' for x in gb)}"); bad += 1; continue
        # re-encode from the parsed state (with the captured key) and compare bit-exactly
        rb = build_bytes(p["power"] == "on", p["mode"] if p["mode"] in MODE else "cool", p["temp"], p["fan"] if p["fan"] in FAN else "auto", p["key"] if p["key"] in KEY else "power")
        exact = rb == gb
        derived = f"{p['power']} {p['mode']} {p['temp']}C fan {p['fan']} (key {p['key']})"
        tagged = f"{st.get('power')} {st.get('mode')} {st.get('temp')}C fan {st.get('fan')}" if st else "-"
        note = "" if not st or (str(p["temp"]) == str(st.get("temp")) and p["mode"] == st.get("mode") and p["fan"] == st.get("fan") and p["power"] == st.get("power")) else "  <- tag differs from frame"
        print(f"#{c['id']:<3} {tag:<20} model {'EXACT' if exact else 'DIFFERS'}  decoded: {derived}{'  unexplained: ' + ','.join(p['unexplained']) if p['unexplained'] else ''}{note}")
        ok += exact; bad += not exact
    print(f"{ok} reproduced bit-exactly, {bad} not")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=MODE, default="cool"); ap.add_argument("--temp", type=int, default=24); ap.add_argument("--fan", choices=FAN, default="auto")
    ap.add_argument("--off", action="store_true"); ap.add_argument("--key", choices=KEY, default=None)
    ap.add_argument("--send", action="store_true"); ap.add_argument("--host", default="ir-blaster.local")
    ap.add_argument("--verify", metavar="SESSION.json")
    a = ap.parse_args()
    if a.verify: return verify(a.verify)
    key = a.key or ("power" if a.off else "temp_up")
    b = build_bytes(power=not a.off, mode=a.mode, temp=a.temp, fan=a.fan, key=key)
    print("bytes  :", " ".join(f"{x:02X}" for x in b))
    print("bits   :", " ".join(to_bits(b)[i:i+8] for i in range(0, 96, 8)))
    t = to_timings(b); print(f"timings: {len(t)} pulses, {sum(t)/1000:.1f} ms")
    print(",".join(map(str, t)))
    if a.send: print("device :", send(a.host, b))

if __name__ == "__main__":
    main()
