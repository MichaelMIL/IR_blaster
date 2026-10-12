#!/usr/bin/env python3
"""Build (and optionally send) frames for the AC remote mapped in ac_map.json.

Frame: 12 bytes, each sent LSB first, pulse-distance coded:
  header 6000 us mark / 7300 us space, bit = 560 us mark + 530 us (0) / 1590 us (1) space,
  stop mark 560 us, 7300 us gap, one 560 us end pulse. 38 kHz carrier.

  byte  meaning
  0-2   00 00 00
  3     01                       (constant)
  4     00
  5     key that was pressed: 00 power, 02 temp up, 03 temp down, 05 fan   (checksummed; AC seems to use the state bytes)
  6     00
  7     power / fan / mode:  bit1 = power on (0x02); bits 5-6 = fan: 00 auto, 01 high, 10 medium, 11 low;
                             0x08 always set in the captures (mode = cool; other modes not captured yet)
  8     00
  9     temperature + 16  (22 C -> 0x26 ... 29 C -> 0x2D)
  10    A5                       (constant)
  11    checksum = sum(bytes 0..10) & 0xFF

usage:
  ac_encode.py --temp 24                          # cool, fan auto, on -> prints bytes + timings
  ac_encode.py --temp 24 --fan high --send        # also sends it through the blaster
  ac_encode.py --off --send
  ac_encode.py --verify ac_map.json               # regenerate every capture from its tags and compare
"""
import argparse, json, sys, os, urllib.request, urllib.parse

FAN = {"auto": 0, "high": 1, "med": 2, "low": 3}
KEY = {"power": 0x00, "temp_up": 0x02, "temp_down": 0x03, "fan": 0x05}
HDR_MARK, HDR_SPACE, BIT_MARK, ZERO, ONE, END_GAP = 6000, 7300, 560, 530, 1590, 7300

def build_bytes(power=True, temp=24, fan="auto", key="temp_up", mode_bits=0x08):
    b = [0, 0, 0, 0x01, 0, KEY[key], 0, 0, 0, 0, 0xA5, 0]
    b[7] = (0x02 if power else 0) | mode_bits | (FAN[fan] << 5)
    b[9] = (temp + 16) & 0xFF
    b[11] = sum(b[:11]) & 0xFF
    return b

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
        st = c.get("state") or {}
        tag = c.get("tag", "")
        if tag == "off":
            b = build_bytes(power=False, temp=int(st.get("temp", 28)), fan="auto", key="power")
        else:
            b = build_bytes(power=True, temp=int(st["temp"]), fan=st["fan"], key="temp_up")
        got = decode_bits(c["timings"]); exp = to_bits(b)
        # byte 5 (key pressed) is not derivable from the state: compare everything except it, and the checksum
        mask = [i for i in range(96) if not (40 <= i < 48 or 88 <= i < 96)]
        same = got and all(got[i] == exp[i] for i in mask)
        # checksum check with the key byte taken from the capture
        gb = [int(got[i:i+8][::-1], 2) for i in range(0, 96, 8)] if got else []
        csum_ok = bool(gb) and gb[11] == (sum(gb[:11]) & 0xFF)
        print(f"#{c['id']:<3} {tag:<18} state-bytes {'MATCH' if same else 'DIFFER'}  checksum {'ok' if csum_ok else 'BAD'}  key byte in capture: 0x{gb[5]:02X}" if gb else f"#{c['id']} undecodable")
        ok += bool(same and csum_ok); bad += not (same and csum_ok)
    print(f"{ok} match, {bad} differ")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--temp", type=int, default=24); ap.add_argument("--fan", choices=FAN, default="auto")
    ap.add_argument("--off", action="store_true"); ap.add_argument("--key", choices=KEY, default=None)
    ap.add_argument("--send", action="store_true"); ap.add_argument("--host", default="ir-blaster.local")
    ap.add_argument("--verify", metavar="SESSION.json")
    a = ap.parse_args()
    if a.verify: return verify(a.verify)
    key = a.key or ("power" if a.off else "temp_up")
    b = build_bytes(power=not a.off, temp=a.temp, fan=a.fan, key=key)
    print("bytes  :", " ".join(f"{x:02X}" for x in b))
    print("bits   :", " ".join(to_bits(b)[i:i+8] for i in range(0, 96, 8)))
    t = to_timings(b); print(f"timings: {len(t)} pulses, {sum(t)/1000:.1f} ms")
    print(",".join(map(str, t)))
    if a.send: print("device :", send(a.host, b))

if __name__ == "__main__":
    main()
