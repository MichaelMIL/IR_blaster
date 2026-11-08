#!/usr/bin/env python3
"""IR blaster web remote.

A small web app meant to run on a home server (Raspberry Pi, NAS, Mac …).
It serves a phone-friendly remote control page and forwards every /api/*
request to the blaster, so the browser only ever talks to this server.
Scenes (saved AC states) are kept in a JSON file next to this script.

    /usr/bin/python3 tools/ir_web/server.py --device 192.168.50.209 --port 8080

Standard library only. On macOS use /usr/bin/python3: other Python builds
are blocked from the local network by the "Local Network" privacy setting.
"""
import argparse
import json
import os
import sys
import threading
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
STATIC = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    "/app.js": ("app.js", "application/javascript; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
    "/manifest.json": ("manifest.json", "application/manifest+json"),
    "/icon.svg": ("icon.svg", "image/svg+xml"),
}
DEFAULT_SCENES = [
    {"name": "Cool down", "icon": "❄️", "state": {"power": "on", "mode": "cool", "temp": 22, "fan": "high"}},
    {"name": "Night", "icon": "🌙", "state": {"power": "on", "mode": "cool", "temp": 26, "fan": "low"}},
    {"name": "Dry", "icon": "💧", "state": {"power": "on", "mode": "dry", "temp": 24, "fan": "auto"}},
    {"name": "Off", "icon": "⏻", "state": {"power": "off"}},
]


class Store:
    """Tiny JSON file store: {"device": "...", "scenes": [...]}"""

    def __init__(self, path, device):
        self.path = path
        self.lock = threading.Lock()
        self.data = {"device": device, "scenes": DEFAULT_SCENES}
        try:
            with open(path) as f:
                saved = json.load(f)
            self.data.update({k: v for k, v in saved.items() if k in ("device", "scenes")})
        except FileNotFoundError:
            pass
        except (OSError, ValueError) as e:
            print(f"warning: ignoring unreadable {path}: {e}", file=sys.stderr)
        if device:
            self.data["device"] = device

    def get(self, key):
        with self.lock:
            return json.loads(json.dumps(self.data[key]))

    def set(self, key, value):
        with self.lock:
            self.data[key] = value
            tmp = self.path + ".tmp"
            with open(tmp, "w") as f:
                json.dump(self.data, f, indent=2)
            os.replace(tmp, self.path)


class Handler(BaseHTTPRequestHandler):
    store: Store = None
    timeout_s = 8

    # ------------------------------------------------------------ plumbing
    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def read_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    # ------------------------------------------------------------- routing
    def do_GET(self):
        path = urlsplit(self.path).path
        if path in STATIC:
            return self.serve_static(path)
        if path.startswith("/api/"):
            return self.proxy("GET")
        if path == "/local/scenes":
            return self.send_json(200, self.store.get("scenes"))
        if path == "/local/settings":
            return self.send_json(200, {"device": self.store.get("device")})
        self.send_json(404, {"ok": False, "message": "not found"})

    def do_POST(self):
        path = urlsplit(self.path).path
        if path.startswith("/api/"):
            return self.proxy("POST")
        body = self.read_body()
        try:
            data = json.loads(body or b"null")
        except ValueError:
            return self.send_json(400, {"ok": False, "message": "bad JSON"})
        if path == "/local/scenes":
            if not isinstance(data, list):
                return self.send_json(400, {"ok": False, "message": "expected a list"})
            self.store.set("scenes", data)
            return self.send_json(200, {"ok": True})
        if path == "/local/settings":
            dev = (data or {}).get("device", "").strip()
            if not dev:
                return self.send_json(400, {"ok": False, "message": "device address required"})
            self.store.set("device", dev)
            return self.send_json(200, {"ok": True, "device": dev})
        self.send_json(404, {"ok": False, "message": "not found"})

    # -------------------------------------------------------------- static
    def serve_static(self, path):
        name, ctype = STATIC[path]
        try:
            with open(os.path.join(HERE, name), "rb") as f:
                body = f.read()
        except OSError:
            return self.send_json(404, {"ok": False, "message": "missing " + name})
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    # --------------------------------------------------------------- proxy
    def proxy(self, method):
        device = self.store.get("device")
        url = "http://%s%s" % (device, self.path)
        body = self.read_body() if method == "POST" else None
        req = urllib.request.Request(url, data=body, method=method)
        if body is not None:
            req.add_header("Content-Type", self.headers.get("Content-Type") or "text/plain")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout_s) as r:
                data = r.read()
                code = r.status
                ctype = r.headers.get("Content-Type", "application/json")
        except urllib.error.HTTPError as e:
            data = e.read()
            code = e.code
            ctype = e.headers.get("Content-Type", "application/json")
        except (urllib.error.URLError, OSError) as e:
            return self.send_json(502, {"ok": False, "message": "device %s unreachable: %s" % (device, getattr(e, "reason", e))})
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", default=None, help="blaster address (default: last saved, else ir-blaster.local)")
    ap.add_argument("--bind", default="0.0.0.0", help="interface to listen on (default all)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--data", default=os.path.join(HERE, "ir_web_data.json"), help="where scenes/settings are saved")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every request")
    a = ap.parse_args()

    store = Store(a.data, a.device)
    if not store.get("device"):
        store.set("device", "ir-blaster.local")
    Handler.store = store
    srv = ThreadingHTTPServer((a.bind, a.port), Handler)
    srv.daemon_threads = True
    srv.verbose = a.verbose
    print(f"IR web remote on http://{a.bind}:{a.port}/  →  device http://{store.get('device')}/  (scenes in {a.data})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
