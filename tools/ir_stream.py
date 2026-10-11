#!/usr/bin/env python3
"""Listen to the IR blaster's live capture stream and record it to a file.

usage:
  ir_stream.py                          # device ir-blaster.local, writes ir_stream_<date>.jsonl
  ir_stream.py --host 192.168.50.209 --out captures.jsonl
  ir_stream.py --prompt                 # after each capture, ask for a name and save it on the device
  ir_stream.py --json                   # write one JSON array (same shape as /api/export) instead of JSON lines

Each capture is one line of JSON:
  {"ts": "...", "id": 12, "type": "nec", "addr": 186, "cmd": 30, "raw": "0xE11E45BA",
   "pulses": 67, "timings": [9000, 4500, ...]}
Timings are microseconds, mark,space,mark,... starting with a mark. The file can
be fed straight to ir_analyze.py (both the .jsonl and the --json form).
Stop with Ctrl-C.
"""
import argparse, json, sys, time, urllib.request, urllib.parse, urllib.error
from datetime import datetime

def get(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.load(r)

def post(url, timeout=10):
    req = urllib.request.Request(url, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="ir-blaster.local", help="device hostname or IP")
    ap.add_argument("--out", help="output file (default ir_stream_<date>.jsonl or .json)")
    ap.add_argument("--interval", type=float, default=0.5, help="poll interval in seconds")
    ap.add_argument("--prompt", action="store_true", help="ask for a name after each capture and save it on the device")
    ap.add_argument("--json", action="store_true", help="write a single JSON document instead of JSON lines")
    ap.add_argument("--from-start", action="store_true", help="also record captures already held by the device")
    a = ap.parse_args()

    base = f"http://{a.host}"
    out = a.out or datetime.now().strftime("ir_stream_%Y%m%d_%H%M%S") + (".json" if a.json else ".jsonl")
    after = 0 if a.from_start else None
    captures = []
    fh = None if a.json else open(out, "a")
    connected = False
    errors = 0
    print(f"listening to {base}/api/stream -> {out}  (Ctrl-C to stop)")

    try:
        while True:
            try:
                if after is None:                      # start from "now"
                    after = get(f"{base}/api/stream?after=4294967295")["last"]
                r = get(f"{base}/api/stream?after={after}")
                if not connected:
                    print(f"connected to {a.host}, waiting for IR...")
                    connected = True
            except (urllib.error.URLError, OSError, ValueError, KeyError) as e:
                if connected or not errors:
                    print(f"device unreachable ({e}); retrying...")
                errors += 1
                connected = False
                time.sleep(2)
                continue

            for c in r["captures"]:
                after = max(after, c["id"])
                rec = {"ts": datetime.now().isoformat(timespec="seconds"), "id": c["id"], "type": c["type"]}
                if c["type"] == "nec":
                    rec.update(addr=c["addr"], cmd=c["cmd"], raw=f"0x{c['raw']:08X}")
                rec.update(pulses=c["pulses"], timings=c["timings"])
                captures.append(rec)
                if fh:
                    fh.write(json.dumps(rec) + "\n"); fh.flush()
                desc = f"NEC addr=0x{c['addr']:02X} cmd=0x{c['cmd']:02X}" if c["type"] == "nec" else c["type"]
                total_ms = sum(c["timings"]) / 1000
                print(f"[{rec['ts'][11:]}] #{c['id']:<4} {desc:<28} {c['pulses']:>4} pulses {total_ms:7.1f} ms  "
                      f"first: {' '.join(str(t) for t in c['timings'][:8])}")
                if a.prompt:
                    try:
                        name = input("   name to save on device (enter to skip): ").strip()
                    except EOFError:
                        name = ""
                    if name:
                        try:
                            res = post(f"{base}/api/stream/save?" + urllib.parse.urlencode({"id": c["id"], "name": name}))
                            print("   " + res.get("message", res))
                            rec["saved_as"] = name
                        except urllib.error.HTTPError as e:
                            print("   save failed: " + e.read().decode(errors="replace"))
            time.sleep(a.interval)
    except KeyboardInterrupt:
        pass
    finally:
        if fh:
            fh.close()
        elif captures or a.json:
            with open(out, "w") as f:
                json.dump({"device": a.host, "unit": "us", "format": "mark,space,mark,...",
                           "recordings": [dict(name=c.get("saved_as") or f"capture_{c['id']}", pulses=c["pulses"], timings=c["timings"],
                                               ts=c["ts"], type=c["type"], addr=c.get("addr"), cmd=c.get("cmd")) for c in captures]}, f)
        print(f"\n{len(captures)} capture(s) written to {out}")

if __name__ == "__main__":
    main()
