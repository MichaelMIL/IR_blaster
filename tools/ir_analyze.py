#!/usr/bin/env python3
"""Analyse raw IR captures exported from the ir_blaster firmware.

usage:
  ir_analyze.py ir_export.json            # everything in the export
  ir_analyze.py ir_export.json ac_on      # one recording by name
  ir_analyze.py --timings 9000,4500,562,...   # a bare timing list

Timings are microseconds, alternating mark,space,mark,... starting with a mark.
The script finds the header, splits multi-part frames on long gaps, clusters
mark/space lengths, guesses the protocol family, and decodes bits both ways
(space-length and mark-length encoded) so the bytes can be compared across
recordings (e.g. ac_cool_22 vs ac_cool_23 shows which byte is the temperature).
"""
import json, sys, statistics

KNOWN = [  # (name, header mark, header space, bit mark, zero space, one space)
    ("NEC",             9000, 4500,  560,  560, 1690),
    ("Samsung",         4500, 4500,  560,  560, 1690),
    ("LG (AC/TV)",      8500, 4250,  560,  560, 1600),
    ("Gree AC",         9000, 4500,  620,  540, 1600),
    ("Daikin AC",       3500, 1750,  430,  430, 1280),
    ("Mitsubishi AC",   3400, 1750,  450,  420, 1300),
    ("Panasonic AC",    3500, 1750,  430,  430, 1300),
    ("Fujitsu AC",      3300, 1600,  400,  400, 1200),
    ("Hitachi AC",      3300, 1700,  400,  500, 1250),
    ("Toshiba AC",      4400, 4300,  580,  490, 1600),
    ("Midea / Coolix",  4400, 4400,  560,  560, 1600),
    ("Haier AC",        3000, 3000,  520,  650, 1650),
    ("Sharp AC",        3800, 1900,  470,  500, 1400),
    ("Carrier AC",      8500, 4200,  500,  600, 1600),
    ("Electra AC",      9000, 4500,  560,  560, 1690),
    ("Kelvinator AC",   9000, 4500,  680,  510, 1530),
    ("Whirlpool AC",    8950, 4480,  600,  520, 1650),
    ("Samsung AC",      3000, 9000,  500,  500, 1500),
    ("Sony SIRC",       2400,  600, 1200,  600,  600),
    ("JVC",             8400, 4200,  525,  525, 1575),
]

def near(v, t, pct=30):
    return abs(v - t) <= t * pct / 100

def clean_glitches(t, min_us=250):
    """Merge receiver dropouts: a mark or space shorter than min_us is joined
    with its neighbours (mark+glitch space+mark -> one mark, and vice versa).
    Returns (cleaned, notes)."""
    t = list(t); notes = []
    i = 1
    while i < len(t) - 1:
        if t[i] < min_us:
            kind = "space" if i % 2 else "mark"
            merged = t[i-1] + t[i] + t[i+1]
            notes.append(f"merged {t[i-1]}+{t[i]}+{t[i+1]} -> {kind == 'space' and 'mark' or 'space'} {merged} us (glitch {kind} {t[i]} us at index {i})")
            t[i-1:i+2] = [merged]
            i = max(1, i - 1)
        else:
            i += 1
    return t, notes

def split_sections(t, gap_us=5000, long_mark_us=4000):
    """Split on spaces longer than gap_us, or on marks longer than long_mark_us
    (a long mark starts a new section's header). Returns [(timings, gap_before)]."""
    parts, cur, gap = [], [], 0
    for i, v in enumerate(t):
        if i % 2 == 1 and v > gap_us:               # long space ends a section
            parts.append((cur, gap)); cur, gap = [], v
        elif i % 2 == 0 and v > long_mark_us and cur:  # long mark begins a new section
            parts.append((cur, gap)); cur, gap = [v], 0
        else:
            cur.append(v)
    if cur: parts.append((cur, gap))
    return [p for p in parts if p[0]]

def cluster(values):
    """2-means on log scale, ignoring outliers (> 3x median). Returns (short, long) or None."""
    if len(values) < 4: return None
    med = statistics.median(values)
    vals = [v for v in values if v <= 3 * med]
    if len(vals) < 4: return None
    import math
    lo, hi = math.log(min(vals)), math.log(max(vals))
    if hi - lo < math.log(1.5): return None          # one cluster only
    c0, c1 = lo, hi
    for _ in range(20):
        a = [v for v in vals if abs(math.log(v) - c0) <= abs(math.log(v) - c1)]
        b = [v for v in vals if abs(math.log(v) - c0) >  abs(math.log(v) - c1)]
        if not a or not b: return None
        n0, n1 = statistics.mean(map(math.log, a)), statistics.mean(map(math.log, b))
        if abs(n0 - c0) < 1e-6 and abs(n1 - c1) < 1e-6: break
        c0, c1 = n0, n1
    if min(b) / max(a) < 1.4: return None              # not separable
    if min(len(a), len(b)) < max(3, len(vals) // 12): return None   # a few outliers are not a level
    return sorted(a), sorted(b)

def has_header(sec):
    """First mark/space pair counts as a header if it stands out from the rest."""
    if len(sec) < 6: return False
    hm, hs = sec[0], sec[1]
    mm = statistics.median(sec[2::2]); ms = statistics.median(sec[3::2])
    return hm > 2 * mm or hs > 2 * ms

def decode_section(sec):
    """sec: timings of one section (mark first). Returns dict with header + bits."""
    out = {}
    if len(sec) < 3: return out
    if has_header(sec):
        out["header"] = (sec[0], sec[1]); body = sec[2:]
    else:
        out["header"] = None; body = sec
    marks, spaces = body[0::2], body[1::2]
    n = min(len(marks), len(spaces))
    out["marks"] = marks; out["spaces"] = spaces
    cm, cs = cluster(marks), cluster(spaces)
    bits_space = bits_mark = None
    if cs:
        thr = (max(cs[0]) + min(cs[1])) / 2
        bits_space = ["1" if spaces[i] > thr else "0" for i in range(n)]
        out["space_thr"] = thr; out["space_clusters"] = cs
    if cm:
        thr = (max(cm[0]) + min(cm[1])) / 2
        bits_mark = ["1" if marks[i] > thr else "0" for i in range(n)]
        out["mark_thr"] = thr; out["mark_clusters"] = cm
    out["bits_space"], out["bits_mark"] = bits_space, bits_mark
    out["trailer"] = body[n*2:]
    return out

def bits_to_bytes(bits, lsb_first=True):
    bs = []
    for i in range(0, len(bits) - len(bits) % 8, 8):
        chunk = bits[i:i+8]
        if lsb_first: chunk = chunk[::-1]
        bs.append(int("".join(chunk), 2))
    rem = bits[len(bits) - len(bits) % 8:]
    return bs, rem

def guess_protocol(hm, hs, bm, s0, s1):
    hits = []
    for name, khm, khs, kbm, ks0, ks1 in KNOWN:
        score = sum([near(hm, khm, 20), near(hs, khs, 20), near(bm, kbm, 35), near(s0, ks0, 35), near(s1, ks1, 25)])
        if score >= 4: hits.append((score, name))
    return [n for _, n in sorted(hits, reverse=True)]

def analyse(name, t, carrier=None):
    print("=" * 78)
    print(f"{name}: {len(t)} pulses, {sum(t)/1000:.1f} ms total" + (f", carrier {carrier} Hz" if carrier else ""))
    if len(t) < 3:
        print("  too short"); return
    t, notes = clean_glitches(t)
    for n in notes: print("  glitch: " + n)
    if notes: print(f"  after cleaning: {len(t)} pulses")
    sections = split_sections(t)
    print(f"  {len(sections)} section(s)" + (" (split on spaces > 5 ms)" if len(sections) > 1 else ""))
    for si, (sec, gap) in enumerate(sections):
        d = decode_section(sec)
        if not d: continue
        marks, spaces = d["marks"], d["spaces"]
        print(f"  -- section {si+1}: {len(sec)} pulses" + (f", preceded by {gap} us gap" if gap else ""))
        if d["header"]:
            hm, hs = d["header"]; print(f"     header: mark {hm} us, space {hs} us")
        else:
            hm = hs = 0; print("     header: none detected (data starts immediately)")
        enc = []
        if d.get("mark_clusters"):  enc.append(f"marks {int(statistics.median(d['mark_clusters'][0]))}/{int(statistics.median(d['mark_clusters'][1]))} us")
        if d.get("space_clusters"): enc.append(f"spaces {int(statistics.median(d['space_clusters'][0]))}/{int(statistics.median(d['space_clusters'][1]))} us")
        if enc: print(f"     two-level: {'; '.join(enc)}  -> data is {'mark' if d.get('mark_clusters') and not d.get('space_clusters') else 'space' if d.get('space_clusters') and not d.get('mark_clusters') else 'mark or space'}-length encoded")
        if marks:
            print(f"     marks : n={len(marks)} min={min(marks)} median={int(statistics.median(marks))} max={max(marks)}")
        if spaces:
            print(f"     spaces: n={len(spaces)} min={min(spaces)} median={int(statistics.median(spaces))} max={max(spaces)}")
        if d.get("trailer"): print(f"     trailer: {d['trailer']}")
        for label, bits, key in (("space-coded", d["bits_space"], "space_thr"), ("mark-coded", d["bits_mark"], "mark_thr")):
            if not bits: continue
            ones = bits.count("1")
            if ones == 0 or ones == len(bits): continue
            lsb, rem = bits_to_bytes(bits, True)
            msb, _ = bits_to_bytes(bits, False)
            print(f"     {label} (threshold {int(d[key])} us): {len(bits)} bits" + (f" + {len(rem)} leftover" if rem else ""))
            print(f"        LSB-first bytes: {' '.join(f'{b:02X}' for b in lsb)}")
            print(f"        MSB-first bytes: {' '.join(f'{b:02X}' for b in msb)}")
            print(f"        bits: {''.join(bits)}")
        if d["bits_space"] and spaces and marks and d["header"]:
            cs = d.get("space_clusters")
            if cs:
                g = guess_protocol(hm, hs, statistics.median(marks), statistics.median(cs[0]), statistics.median(cs[1]))
                print(f"     protocol guess: {', '.join(g) if g else 'no match in table (fine - replay works regardless)'}")
    if len(sections) >= 2 and all(len(s[0]) == len(sections[0][0]) for s in sections):
        same = all(s[0] == sections[0][0] for s in sections)
        print(f"  sections are {'identical repeats' if same else 'different (likely one message in parts)'}")

def main(argv):
    if len(argv) >= 2 and argv[0] == "--timings":
        t = [int(x) for x in argv[1].replace(" ", "").split(",") if x]
        analyse("timings", t); return
    if not argv:
        print(__doc__); return
    data = json.load(open(argv[0]))
    carrier = data.get("carrier_hz")
    want = argv[1] if len(argv) > 1 else None
    items = []
    if isinstance(data, dict) and "recordings" in data:
        if data.get("last_capture", {}).get("pulses") and want in (None, "last"):
            items.append(("last_capture", data["last_capture"]["timings"]))
        items += [(r["name"], r["timings"]) for r in data["recordings"] if want in (None, r["name"])]
    elif isinstance(data, dict) and "timings" in data:
        items.append((data.get("name", "capture"), data["timings"]))
    elif isinstance(data, list):
        items.append(("timings", data))
    if not items:
        print("nothing matched"); return
    for name, t in items:
        analyse(name, t, carrier)

if __name__ == "__main__":
    main(sys.argv[1:])
