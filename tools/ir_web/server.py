#!/usr/bin/env python3
"""IR blaster web remote.

A small web app meant to run all the time on a home server (Raspberry Pi,
NAS, Mac …). It serves a phone-friendly remote, forwards every /api/* request
to the blaster (so the browser only talks to this server), samples the device
once a minute for the History page, and runs countdown / scheduled timers.

    /usr/bin/python3 tools/ir_web/server.py --device 192.168.50.209 --port 8080

Pages:   /  remote      /timers      /history      /advanced
Data:    ir_web_data.json (device address, scenes, timers)
         ir_web_history.jsonl (samples + events, pruned to --keep-days)

Standard library only. On macOS use /usr/bin/python3: other Python builds
are blocked from the local network by the "Local Network" privacy setting.
"""
import argparse
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
PAGES = {"/": "index.html", "/timers": "timers.html", "/history": "history.html", "/advanced": "advanced.html"}
TYPES = {".html": "text/html; charset=utf-8", ".js": "application/javascript; charset=utf-8",
         ".css": "text/css; charset=utf-8", ".json": "application/manifest+json", ".svg": "image/svg+xml"}
DEFAULT_SCENES = [
    {"name": "Cool down", "icon": "❄️", "state": {"power": "on", "mode": "cool", "temp": 22, "fan": "high"}},
    {"name": "Night", "icon": "🌙", "state": {"power": "on", "mode": "cool", "temp": 26, "fan": "low"}},
    {"name": "Dry", "icon": "💧", "state": {"power": "on", "mode": "dry", "temp": 24, "fan": "auto"}},
    {"name": "Off", "icon": "⏻", "state": {"power": "off"}},
]
SAMPLE_EVERY = 60          # seconds between status samples
RX_EVERY = 5               # seconds between received-frame polls


def now_iso():
    return datetime.now().isoformat(timespec="seconds")


# ======================================================================= store
class Store:
    """JSON file: {"device", "scenes", "timers", "next_id"}"""

    def __init__(self, path, device):
        self.path = path
        self.lock = threading.RLock()
        self.data = {"device": device or "", "scenes": DEFAULT_SCENES, "timers": [], "next_id": 1}
        try:
            with open(path) as f:
                saved = json.load(f)
            self.data.update({k: v for k, v in saved.items() if k in self.data})
        except FileNotFoundError:
            pass
        except (OSError, ValueError) as e:
            print(f"warning: ignoring unreadable {path}: {e}", file=sys.stderr)
        if device:
            self.data["device"] = device
        if not self.data["device"]:
            self.data["device"] = "ir-blaster.local"

    def get(self, key):
        with self.lock:
            return json.loads(json.dumps(self.data[key]))

    def set(self, key, value):
        with self.lock:
            self.data[key] = value
            self._write()

    def new_id(self):
        with self.lock:
            i = self.data["next_id"]
            self.data["next_id"] = i + 1
            self._write()
            return i

    def update_timers(self, fn):
        """fn(list) -> list, under the lock."""
        with self.lock:
            self.data["timers"] = fn(self.data["timers"])
            self._write()
            return json.loads(json.dumps(self.data["timers"]))

    def _write(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.data, f, indent=2, ensure_ascii=False)
        os.replace(tmp, self.path)


# ===================================================================== history
class History:
    """Append-only JSONL of samples ({"k":"s"}) and events ({"k":"e"}), kept in memory too."""

    def __init__(self, path, keep_days):
        self.path = path
        self.keep = keep_days * 86400
        self.lock = threading.Lock()
        self.rows = []
        try:
            with open(path) as f:
                for line in f:
                    try:
                        self.rows.append(json.loads(line))
                    except ValueError:
                        pass
        except FileNotFoundError:
            pass
        self.prune(rewrite=True)

    def add(self, row):
        row.setdefault("t", int(time.time()))
        with self.lock:
            self.rows.append(row)
            with open(self.path, "a") as f:
                f.write(json.dumps(row, ensure_ascii=False) + "\n")

    def event(self, etype, text, **data):
        row = {"k": "e", "type": etype, "text": text}
        if data:
            row["data"] = data
        self.add(row)
        return row

    def prune(self, rewrite=False):
        cutoff = time.time() - self.keep
        with self.lock:
            n = len(self.rows)
            self.rows = [r for r in self.rows if r.get("t", 0) >= cutoff]
            if rewrite or len(self.rows) != n:
                tmp = self.path + ".tmp"
                with open(tmp, "w") as f:
                    for r in self.rows:
                        f.write(json.dumps(r, ensure_ascii=False) + "\n")
                os.replace(tmp, self.path)

    def since(self, t0, kinds=("s", "e")):
        with self.lock:
            return [r for r in self.rows if r.get("t", 0) >= t0 and r.get("k") in kinds]


# ====================================================================== device
class Device:
    """HTTP client for the blaster, shared by the proxy, sampler and scheduler."""

    def __init__(self, store, history, timeout=8):
        self.store, self.history, self.timeout = store, history, timeout

    def addr(self):
        return self.store.get("device")

    def request(self, method, path, body=None, ctype=None):
        """Returns (status, content_type, bytes). Raises OSError/URLError when unreachable."""
        req = urllib.request.Request("http://%s%s" % (self.addr(), path), data=body, method=method)
        if body is not None:
            req.add_header("Content-Type", ctype or "text/plain")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.status, r.headers.get("Content-Type", "application/json"), r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.headers.get("Content-Type", "application/json"), e.read()

    def get_json(self, path):
        code, _, data = self.request("GET", path)
        if code != 200:
            raise OSError("HTTP %d" % code)
        return json.loads(data)

    def post(self, path, params):
        q = urllib.parse.urlencode({k: v for k, v in params.items() if v is not None})
        code, _, data = self.request("POST", path + ("?" + q if q else ""), body=b"")
        try:
            j = json.loads(data)
        except ValueError:
            j = {"ok": code == 200, "message": data.decode(errors="replace")}
        if code != 200 or (isinstance(j, dict) and j.get("ok") is False):
            raise OSError(j.get("message") if isinstance(j, dict) else "HTTP %d" % code)
        return j

    # ---- actions used by timers and scenes
    def run_action(self, action, source):
        """action: {"type":"ac","state":{...}} | {"type":"scene","name"} | {"type":"button","id"} | {"type":"recording","name"}"""
        t = action.get("type")
        if t == "scene":
            scene = next((s for s in self.store.get("scenes") if s["name"] == action.get("name")), None)
            if not scene:
                raise OSError("scene '%s' no longer exists" % action.get("name"))
            action = {"type": "ac", "state": scene["state"]}
            t = "ac"
        if t == "ac":
            st = dict(action.get("state") or {})
            if st.get("power") == "off":
                st = {"power": "off"}
            r = self.post("/api/ac", st)
            text = "AC " + describe_ac(r.get("state") or st)
            self.history.event("cmd", text, source=source, ac=r.get("state") or st)
            return text
        if t == "button":
            self.post("/api/button", {"id": action.get("id")})
            text = "button '%s'" % action.get("label", action.get("id"))
            self.history.event("cmd", text, source=source)
            return text
        if t == "recording":
            self.post("/api/play", {"name": action.get("name")})
            text = "recording '%s'" % action.get("name")
            self.history.event("cmd", text, source=source)
            return text
        raise OSError("unknown action type %r" % t)


def describe_ac(st):
    if not st or st.get("power") == "off":
        return "off"
    fan = st.get("fan")
    fan = "medium" if fan == "med" else fan
    return " ".join(str(x) for x in [st.get("mode"), ("%s°C" % st["temp"]) if st.get("temp") is not None else None, ("fan " + fan) if fan else None] if x)


def describe_action(a, store=None):
    t = a.get("type")
    if t == "ac":
        return "AC " + describe_ac(a.get("state"))
    if t == "scene":
        return "scene '%s'" % a.get("name")
    if t == "button":
        return "button '%s'" % a.get("label", a.get("id"))
    if t == "recording":
        return "recording '%s'" % a.get("name")
    return "?"


# ===================================================================== sampler
class Sampler(threading.Thread):
    """Polls the device: a status sample every minute, received frames every few seconds."""

    def __init__(self, dev, history):
        super().__init__(daemon=True)
        self.dev, self.history = dev, history
        self.last_seq = None
        self.seen = set()
        self.online = None
        self.last_sample = 0
        self.last_prune = time.time()

    def run(self):
        while True:
            t0 = time.time()
            try:
                self.poll_rx()
                if t0 - self.last_sample >= SAMPLE_EVERY:
                    self.sample()
                    self.last_sample = t0
            except Exception as e:  # never die
                print("sampler:", e, file=sys.stderr)
            if t0 - self.last_prune > 3600:
                self.history.prune()
                self.last_prune = t0
            time.sleep(max(0.5, RX_EVERY - (time.time() - t0)))

    def sample(self):
        try:
            s = self.dev.get_json("/api/status")
        except Exception as e:
            if self.online is not False:
                self.history.event("device", "device offline (%s)" % str(e)[:60])
            self.online = False
            self.history.add({"k": "s", "online": False})
            return
        if self.online is False:
            self.history.event("device", "device back online")
        self.online = True
        self.history.add({"k": "s", "online": True, "rssi": s.get("rssi"), "heap": (s.get("heap") or {}).get("free"),
                          "heap_min": (s.get("heap") or {}).get("min_free"), "up": s.get("uptime"),
                          "ac": {k: s["ac"].get(k) for k in ("power", "mode", "temp", "fan")} if s.get("ac") else None,
                          "mqtt": (s.get("mqtt") or {}).get("state")})

    def poll_rx(self):
        try:
            r = self.dev.get_json("/api/received")
        except Exception:
            return
        if r.get("seq") == self.last_seq:
            return
        first = self.last_seq is None
        self.last_seq = r.get("seq")
        now_ms = r.get("now", 0)
        keys = set()
        for f in r.get("frames", []):
            key = (f.get("t"), f.get("type"), f.get("raw"), f.get("pulses"))
            keys.add(key)
            if first or key in self.seen:
                continue
            age = max(0, (now_ms - f.get("t", now_ms)) / 1000.0)
            if age > 3600:          # device clock wrapped or stale history
                continue
            if f["type"] == "nec":
                text = "NEC 0x%02X / 0x%02X" % (f.get("addr", 0), f.get("cmd", 0))
            elif f["type"] == "repeat":
                text = "NEC repeat"
            else:
                text = "raw frame, %d pulses" % f.get("pulses", 0)
            self.history.add({"k": "e", "t": int(time.time() - age), "type": "rx", "text": text,
                              "data": {k: f.get(k) for k in ("type", "addr", "cmd", "raw", "pulses") if k in f}})
        self.seen = keys


# =================================================================== scheduler
class Scheduler(threading.Thread):
    def __init__(self, dev, store, history):
        super().__init__(daemon=True)
        self.dev, self.store, self.history = dev, store, history

    def run(self):
        while True:
            try:
                self.tick()
            except Exception as e:
                print("scheduler:", e, file=sys.stderr)
            time.sleep(1)

    def tick(self):
        now = time.time()
        lt = time.localtime(now)
        hhmm = time.strftime("%H:%M", lt)
        today = time.strftime("%Y-%m-%d", lt)
        weekday = lt.tm_wday                     # Monday = 0
        fire = []

        def step(timers):
            keep = []
            for tm in timers:
                if tm["kind"] == "countdown":
                    if tm["due"] <= now:
                        fire.append(tm)
                        continue                 # one-shot: drop it
                elif tm["kind"] == "schedule":
                    if tm.get("enabled", True) and tm["time"] == hhmm and weekday in tm.get("days", list(range(7))) \
                            and tm.get("last_run") != today:
                        tm["last_run"] = today
                        fire.append(tm)
                keep.append(tm)
            return keep

        self.store.update_timers(step)
        for tm in fire:
            label = tm.get("label") or describe_action(tm["action"])
            try:
                text = self.dev.run_action(tm["action"], source="timer:" + tm["kind"])
                self.history.event("timer", "%s timer '%s' fired → %s" % (tm["kind"], label, text), id=tm["id"])
            except Exception as e:
                self.history.event("timer", "%s timer '%s' FAILED: %s" % (tm["kind"], label, e), id=tm["id"], error=True)


# ===================================================================== handler
class Handler(BaseHTTPRequestHandler):
    store: Store = None
    history: History = None
    dev: Device = None

    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    # ------------------------------------------------------------- helpers
    def send_bytes(self, code, ctype, body, cache="no-store"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", cache)
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, code, obj):
        self.send_bytes(code, "application/json", json.dumps(obj, ensure_ascii=False).encode())

    def read_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def read_json(self):
        try:
            return json.loads(self.read_body() or b"null"), None
        except ValueError:
            return None, "bad JSON"

    # ------------------------------------------------------------- routing
    def do_GET(self):
        u = urlsplit(self.path)
        path, q = u.path, parse_qs(u.query)
        if path in PAGES:
            return self.serve_static(PAGES[path])
        if "/" not in path[1:] and os.path.splitext(path)[1] in TYPES:
            return self.serve_static(path[1:])
        if path.startswith("/api/"):
            return self.proxy("GET")
        if path == "/local/scenes":
            return self.send_json(200, self.store.get("scenes"))
        if path == "/local/settings":
            return self.send_json(200, {"device": self.store.get("device"), "server_time": now_iso(),
                                        "tz": time.strftime("%Z"), "history_days": self.history.keep // 86400})
        if path == "/local/timers":
            return self.send_json(200, {"now": time.time(), "timers": self.store.get("timers")})
        if path == "/local/history":
            hours = float((q.get("hours") or ["24"])[0])
            kinds = tuple((q.get("kinds") or ["s,e"])[0].split(","))
            rows = self.history.since(time.time() - hours * 3600, kinds)
            return self.send_json(200, {"now": time.time(), "rows": rows})
        if path == "/local/history.jsonl":
            body = "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in self.history.since(0)).encode()
            return self.send_bytes(200, "application/x-ndjson", body)
        self.send_json(404, {"ok": False, "message": "not found"})

    def do_POST(self):
        path = urlsplit(self.path).path
        if path.startswith("/api/"):
            return self.proxy("POST")
        data, err = self.read_json()
        if err:
            return self.send_json(400, {"ok": False, "message": err})
        try:
            if path == "/local/scenes":
                if not isinstance(data, list):
                    raise ValueError("expected a list")
                self.store.set("scenes", data)
                return self.send_json(200, {"ok": True})
            if path == "/local/settings":
                dev = (data or {}).get("device", "").strip()
                if not dev:
                    raise ValueError("device address required")
                self.store.set("device", dev)
                return self.send_json(200, {"ok": True, "device": dev})
            if path == "/local/timers":
                return self.send_json(200, {"ok": True, "timer": self.add_timer(data or {})})
            if path == "/local/timers/delete":
                tid = int((data or {}).get("id"))
                self.store.update_timers(lambda ts: [t for t in ts if t["id"] != tid])
                return self.send_json(200, {"ok": True})
            if path == "/local/timers/update":
                tid = int((data or {}).get("id"))
                patch = {k: v for k, v in (data or {}).items() if k in ("enabled", "label", "time", "days", "action", "due")}

                def upd(ts):
                    for t in ts:
                        if t["id"] == tid:
                            t.update(patch)
                    return ts
                self.store.update_timers(upd)
                return self.send_json(200, {"ok": True})
            if path == "/local/run":                       # run an action now (scene / timer preview)
                text = self.dev.run_action(data or {}, source="web")
                return self.send_json(200, {"ok": True, "message": text})
        except (ValueError, TypeError, KeyError) as e:
            return self.send_json(400, {"ok": False, "message": str(e)})
        except OSError as e:
            return self.send_json(502, {"ok": False, "message": str(e)})
        self.send_json(404, {"ok": False, "message": "not found"})

    def add_timer(self, d):
        kind = d.get("kind")
        action = d.get("action") or {}
        if action.get("type") not in ("ac", "scene", "button", "recording"):
            raise ValueError("action type must be ac, scene, button or recording")
        tm = {"id": self.store.new_id(), "kind": kind, "action": action, "label": (d.get("label") or "").strip()[:40],
              "created": int(time.time())}
        if kind == "countdown":
            if "due" in d:
                tm["due"] = float(d["due"])
            else:
                minutes = float(d.get("minutes", 0))
                if minutes <= 0:
                    raise ValueError("minutes must be > 0")
                tm["due"] = time.time() + minutes * 60
            if tm["due"] <= time.time():
                raise ValueError("that time is already in the past")
        elif kind == "schedule":
            t = str(d.get("time", ""))
            hh, mm = t.split(":")
            if not (0 <= int(hh) < 24 and 0 <= int(mm) < 60):
                raise ValueError("bad time")
            tm["time"] = "%02d:%02d" % (int(hh), int(mm))
            days = d.get("days")
            tm["days"] = sorted({int(x) for x in days}) if days else list(range(7))
            if not tm["days"]:
                raise ValueError("pick at least one weekday")
            tm["enabled"] = bool(d.get("enabled", True))
        else:
            raise ValueError("kind must be countdown or schedule")
        self.store.update_timers(lambda ts: ts + [tm])
        self.history.event("timer", "%s timer added: %s" % (kind, tm["label"] or describe_action(action)), id=tm["id"])
        return tm

    # -------------------------------------------------------------- static
    def serve_static(self, name):
        ext = os.path.splitext(name)[1]
        try:
            with open(os.path.join(HERE, name), "rb") as f:
                body = f.read()
        except OSError:
            return self.send_json(404, {"ok": False, "message": "missing " + name})
        self.send_bytes(200, TYPES.get(ext, "application/octet-stream"), body, cache="no-cache")

    # --------------------------------------------------------------- proxy
    def proxy(self, method):
        body = self.read_body() if method == "POST" else None
        try:
            code, ctype, data = self.dev.request(method, self.path, body, self.headers.get("Content-Type"))
        except (urllib.error.URLError, OSError) as e:
            return self.send_json(502, {"ok": False, "message": "device %s unreachable: %s" % (self.dev.addr(), getattr(e, "reason", e))})
        if method == "POST" and code == 200:
            self.log_command(urlsplit(self.path), data)
        self.send_bytes(code, ctype, data)

    def log_command(self, u, data):
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        try:
            j = json.loads(data)
        except ValueError:
            j = {}
        if u.path == "/api/ac":
            self.history.event("cmd", "AC " + describe_ac(j.get("state") or q), source="web", ac=j.get("state") or q)
        elif u.path == "/api/button":
            self.history.event("cmd", "button #%s" % q.get("id"), source="web")
        elif u.path == "/api/play":
            self.history.event("cmd", "recording '%s'" % q.get("name"), source="web")
        elif u.path == "/api/send":
            self.history.event("cmd", "NEC %s" % (q.get("raw") or "%s / %s" % (q.get("addr"), q.get("cmd"))), source="web")


# ======================================================================== main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", default=None, help="blaster address (default: last saved, else ir-blaster.local)")
    ap.add_argument("--bind", default="0.0.0.0", help="interface to listen on (default all)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--data", default=os.path.join(HERE, "ir_web_data.json"), help="scenes, timers and settings file")
    ap.add_argument("--history", default=os.path.join(HERE, "ir_web_history.jsonl"), help="samples and events file")
    ap.add_argument("--keep-days", type=int, default=30, help="how much history to keep (default 30 days)")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every request")
    a = ap.parse_args()

    store = Store(a.data, a.device)
    history = History(a.history, a.keep_days)
    dev = Device(store, history)
    Handler.store, Handler.history, Handler.dev = store, history, dev
    Sampler(dev, history).start()
    Scheduler(dev, store, history).start()
    history.event("server", "web remote started")

    srv = ThreadingHTTPServer((a.bind, a.port), Handler)
    srv.daemon_threads = True
    srv.verbose = a.verbose
    print(f"IR web remote on http://{a.bind}:{a.port}/  →  device http://{store.get('device')}/")
    print(f"data: {a.data}   history: {a.history} ({a.keep_days} days)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
