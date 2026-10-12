#!/usr/bin/env python3
"""IR remote key mapper: a local web GUI for tagging captures from the IR blaster.

usage:
  ir_mapper.py                              # device ir-blaster.local, session ac_map.json, opens the browser
  ir_mapper.py --host 192.168.50.209 --session living_room_ac.json --port 8765

Workflow: start it, press keys on the remote in front of the device, and tag
each capture as it appears (presets for AC keys, or free text). Tags are saved
to the session file on every change. Buttons let you replay a capture through
the device, upload tagged captures to the device as recordings (and so make
them available on /remote and in Home Assistant), compare the decoded bits
of two captures, and export everything in the format ir_analyze.py reads.

The script talks to the device itself (polls /api/stream), so the browser only
talks to localhost. On macOS run it with /usr/bin/python3 (other Pythons are
blocked from the local network until allowed in Privacy & Security).
"""
import argparse, json, os, sys, threading, time, webbrowser, urllib.request, urllib.parse, urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import ir_analyze
except Exception:
    ir_analyze = None

# ----------------------------------------------------------------- state

# Columns of the state table. Every capture row gets one dropdown per column.
COLUMNS = [
    {"key": "power", "label": "Power", "options": ["on", "off"]},
    {"key": "mode",  "label": "Mode",  "options": ["cool", "heat", "dry", "fan", "auto"]},
    {"key": "temp",  "label": "Temp",  "options": [str(t) for t in range(16, 31)]},
    {"key": "fan",   "label": "Fan",   "options": ["auto", "low", "med", "high", "turbo"]},
    {"key": "swing", "label": "Swing", "options": ["off", "on"]},
    {"key": "extra", "label": "Extra", "options": ["-", "sleep", "eco", "light", "ifeel", "timer"]},
]
DEFAULT_STATE = {"power": "on", "mode": "cool", "temp": "22", "fan": "auto", "swing": "off", "extra": "-"}

def tag_for(state):
    if not state: return ""
    if state.get("power") == "off":
        return "off" + ("" if state.get("extra", "-") in ("-", "") else "_" + state["extra"])
    parts = [state.get("mode", ""), state.get("temp", ""), state.get("fan", ""), state.get("swing", "")]
    if state.get("extra", "-") not in ("-", ""): parts.append(state["extra"])
    return "_".join(p for p in parts if p)

def next_state(prev):
    """New row = copy of the previous row; identical settings -> temperature + 1."""
    st = dict(prev or DEFAULT_STATE)
    if prev is not None and st == prev:
        temps = COLUMNS[2]["options"]
        try:
            i = temps.index(str(st.get("temp")))
            st["temp"] = temps[min(i + 1, len(temps) - 1)]
        except ValueError:
            st["temp"] = temps[0]
    return st

class Session:
    def __init__(self, path, host):
        self.path = path
        self.host = host
        self.lock = threading.Lock()
        self.captures = []          # dicts: id, ts, type, addr, cmd, raw, pulses, timings, tag, note, uploaded
        self.next_id = 1
        self.device_last = None     # last stream id seen on the device
        self.status = {"device": "connecting", "last_error": ""}
        self.load()

    def load(self):
        if os.path.exists(self.path):
            try:
                d = json.load(open(self.path))
                self.captures = d.get("captures", [])
                self.next_id = max([c["id"] for c in self.captures] + [0]) + 1
            except Exception as e:
                print(f"could not read {self.path}: {e}")

    def save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"device": self.host, "unit": "us", "format": "mark,space,mark,...",
                       "captures": self.captures}, f, indent=1)
        os.replace(tmp, self.path)

    def add(self, c):
        with self.lock:
            prev = self.captures[-1].get("state") if self.captures else None
            st = next_state(prev)
            rec = {"id": self.next_id, "device_id": c["id"], "ts": time.strftime("%Y-%m-%d %H:%M:%S"),
                   "type": c["type"], "pulses": c["pulses"], "timings": c["timings"],
                   "state": st, "tag": tag_for(st), "note": "", "uploaded": False}
            if c["type"] == "nec":
                rec.update(addr=c["addr"], cmd=c["cmd"], raw="0x%08X" % c["raw"])
            rec["decode"] = decode_summary(c["timings"])
            self.captures.append(rec)
            self.next_id += 1
            self.save()
            return rec

    def update(self, cid, fields):
        with self.lock:
            for c in self.captures:
                if c["id"] == cid:
                    for k in ("note", "uploaded"):
                        if k in fields: c[k] = fields[k]
                    if "state" in fields:
                        st = dict(c.get("state") or DEFAULT_STATE)
                        for col in COLUMNS:
                            v = fields["state"].get(col["key"])
                            if v in col["options"]: st[col["key"]] = v
                        c["state"] = st; c["tag"] = tag_for(st); c["uploaded"] = False
                    elif "tag" in fields:
                        c["tag"] = fields["tag"]; c["uploaded"] = False
                    self.save(); return c
        return None

    def delete(self, cid):
        with self.lock:
            self.captures = [c for c in self.captures if c["id"] != cid]
            self.save()

    def get(self, cid):
        return next((c for c in self.captures if c["id"] == cid), None)


def decode_summary(timings):
    """Compact decode for the table: header, encoding, bytes (LSB-first) per section."""
    if not ir_analyze or len(timings) < 6:
        return {}
    try:
        t, _ = ir_analyze.clean_glitches(timings)
        out = {"sections": []}
        for sec, gap in ir_analyze.split_sections(t):
            d = ir_analyze.decode_section(sec)
            if not d: continue
            bits = d.get("bits_mark") if d.get("mark_clusters") and not d.get("space_clusters") else d.get("bits_space") or d.get("bits_mark")
            enc = "mark" if (d.get("mark_clusters") and not d.get("space_clusters")) else "space" if d.get("space_clusters") else "?"
            item = {"header": d["header"], "encoding": enc, "bits": "".join(bits) if bits else "", "pulses": len(sec)}
            if bits:
                lsb, rem = ir_analyze.bits_to_bytes(bits, True)
                item["bytes_lsb"] = " ".join(f"{b:02X}" for b in lsb) + (f" +{len(rem)}b" if rem else "")
                msb, _ = ir_analyze.bits_to_bytes(bits, False)
                item["bytes_msb"] = " ".join(f"{b:02X}" for b in msb)
            out["sections"].append(item)
        return out
    except Exception as e:
        return {"error": str(e)}

# ----------------------------------------------------------------- device

def dev_get(host, path, timeout=5):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
        return json.load(r)

def dev_post(host, path, body=None, timeout=10):
    req = urllib.request.Request(f"http://{host}{path}", method="POST", data=body,
                                 headers={"Content-Type": "text/plain"} if body else {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        try: return json.load(e)
        except Exception: return {"ok": False, "message": str(e)}

def poller(sess, stop):
    after = None
    while not stop.is_set():
        try:
            if after is None:
                after = dev_get(sess.host, "/api/stream?after=4294967295")["last"]
            r = dev_get(sess.host, f"/api/stream?after={after}")
            sess.status["device"] = "connected"
            for c in r["captures"]:
                after = max(after, c["id"])
                rec = sess.add(c)
                print(f"capture #{rec['id']}: {rec.get('raw', rec['type'])} {rec['pulses']} pulses")
        except Exception as e:
            sess.status["device"] = "unreachable"
            sess.status["last_error"] = str(e)
            after = None
            time.sleep(2)
            continue
        time.sleep(0.5)

# ----------------------------------------------------------------- http

PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>IR key mapper</title>
<style>
:root{--bg:#0f1115;--card:#171a21;--line:#262a33;--fg:#e6e8ec;--mute:#8b92a1;--acc:#4f8cff;--ok:#3ddc84;--warn:#ffb02e;--bad:#ff5c5c}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{padding:12px 18px;border-bottom:1px solid var(--line);display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap}
h1{font-size:16px;margin:0}#dev{color:var(--mute);font-size:13px}.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--bad);margin-right:6px}.dot.on{background:var(--ok)}
main{padding:14px 18px;display:grid;gap:14px;grid-template-columns:300px 1fr}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px}
h2{font-size:12px;margin:0 0 10px;color:var(--mute);text-transform:uppercase;letter-spacing:.06em}
button{padding:6px 10px;border:0;border-radius:6px;background:var(--acc);color:#fff;font:inherit;cursor:pointer}button.sec{background:#2a2f3a}button.sm{padding:3px 8px;font-size:12px}button:disabled{opacity:.5}
td select{width:auto;padding:3px 4px}input,select,textarea{padding:6px 8px;background:#0c0e12;border:1px solid var(--line);border-radius:6px;color:var(--fg);font:inherit;width:100%}
table{width:100%;border-collapse:collapse;font-size:13px}td,th{padding:6px 5px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}th{color:var(--mute);font-weight:500;white-space:nowrap}
tr.new td{background:#1d2a1f}tr.sel td{background:#1d2436}
code,.mono{font-family:ui-monospace,Menlo,monospace;font-size:12px}
.chips{display:flex;flex-wrap:wrap;gap:6px}.chip{padding:4px 9px;border-radius:14px;background:#262b36;cursor:pointer;font-size:12px;user-select:none}.chip:hover{background:#333a48}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.grow{flex:1}
#cmp{white-space:pre;overflow:auto;font-family:ui-monospace,Menlo,monospace;font-size:12px;line-height:1.5}
.diff{color:var(--warn);font-weight:700}.tag{font-weight:600;color:#9ec1ff}
#msg{color:var(--mute);font-size:12px;margin-top:8px;min-height:16px}
@media(max-width:900px){main{grid-template-columns:1fr}}
</style></head><body>
<header><h1>IR key mapper <span style="color:var(--mute);font-weight:400" id="sess"></span></h1><div id="dev"><span class="dot"></span>connecting…</div></header>
<main>
<div style="display:grid;gap:14px;align-content:start">
<section><h2>Next capture</h2>
 <p style="margin:0 0 8px;color:var(--mute);font-size:12px">A new capture copies the previous row's settings; if nothing was changed the temperature steps up by one. Fix any dropdown on a row and the following captures continue from there.</p>
 <div id="next" class="mono" style="font-size:13px"></div>
 <div class="row" style="margin-top:10px"><label style="font-size:12px;color:var(--mute)"><input type="checkbox" id="autoup" style="width:auto"> auto-upload tagged captures to the device (off: captures stay on this PC; use "to device" / "Upload all tagged" when you want them on the blaster)</label></div>
</section>
<section><h2>Session</h2>
 <div class="row"><button class="sec sm" onclick="exportAll()">Export for ir_analyze.py</button><button class="sec sm" onclick="uploadAll()">Upload all tagged to device</button></div>
 <div class="row" style="margin-top:8px"><button class="sec sm" onclick="if(confirm('Delete untagged captures?'))api('/api/prune',{m:'POST'}).then(load)">Delete untagged</button><button class="sec sm" onclick="api('/api/device/stream/clear',{m:'POST'})">Clear device history</button></div>
 <div id="msg"></div>
</section>
<section><h2>Compare two captures</h2>
 <div class="row"><select id="cmpA"></select><select id="cmpB"></select><button class="sm" onclick="compare()">Compare</button></div>
 <div id="cmp" style="margin-top:8px"></div>
</section>
</div>
<section><h2>Captures <span id="count" style="text-transform:none;letter-spacing:0"></span></h2>
 <table><thead id="head"></thead><tbody id="rows"><tr><td colspan="12" style="color:var(--mute)">press a key on the remote…</td></tr></tbody></table>
</section>
</main>
<script>
const $=id=>document.getElementById(id);
let caps=[],cols=[],selected=null,lastSeen=0;
function esc(s){return String(s==null?'':s).replace(/[<>&"]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;'}[c]))}
async function api(p,{m='GET',q={},body}={}){const u=new URL(p,location.href);for(const k in q)u.searchParams.set(k,q[k]);const r=await fetch(u,{method:m,body:body?JSON.stringify(body):undefined,headers:body?{'Content-Type':'application/json'}:{}});const t=await r.text();try{return JSON.parse(t)}catch{return t}}
function msg(m){$('msg').textContent=m}
async function setState(id,key,val){const c=caps.find(c=>c.id===id);if(!c)return;const st=Object.assign({},c.state||{});st[key]=val;await api('/api/capture',{m:'POST',q:{id},body:{state:st}});await load();if($('autoup').checked)upload(id,true)}
function sel(c,col){return `<select onclick="event.stopPropagation()" onchange="setState(${c.id},'${col.key}',this.value)">${col.options.map(o=>`<option ${((c.state||{})[col.key]===o)?'selected':''}>${o}</option>`).join('')}</select>`}
async function load(){const r=await api('/api/state');caps=r.captures;cols=r.columns;$('sess').textContent='· '+r.session;$('dev').innerHTML=`<span class="dot ${r.status.device==='connected'?'on':''}"></span>${r.host}: ${r.status.device}${r.status.device!=='connected'&&r.status.last_error?' ('+esc(r.status.last_error)+')':''}`;
 $('next').innerHTML=cols.map(c=>`<span style="color:var(--mute)">${c.label.toLowerCase()}</span> <b>${esc(r.next.state[c.key])}</b>`).join(' &nbsp; ')+`<br>tag: <span class="tag">${esc(r.next.tag)}</span>`;
 for(const c of caps){if(c.id>lastSeen){lastSeen=c.id;if($('autoup').checked&&c.tag&&!c.uploaded)upload(c.id,true)}}
 render()}
function render(){$('count').textContent=caps.length?`(${caps.length})`:'';
 $('head').innerHTML='<tr><th>#</th><th>time</th>'+cols.map(c=>`<th>${c.label}</th>`).join('')+'<th>tag</th><th>decode</th><th>bytes (LSB-first)</th><th></th></tr>';
 const tb=$('rows');
 tb.innerHTML=caps.length?caps.slice().reverse().map(c=>{const d=c.decode&&c.decode.sections&&c.decode.sections[0]||{};const dec=c.type==='nec'?`NEC ${c.addr.toString(16).toUpperCase().padStart(2,'0')}/${c.cmd.toString(16).toUpperCase().padStart(2,'0')}`:(d.encoding?`${d.encoding} ${d.bits?d.bits.length+'b':''}${c.decode.sections.length>1?' ×'+c.decode.sections.length:''}`:c.type);
  return `<tr class="${c.id===selected?'sel':''}${c.id===lastSeen?' new':''}" onclick="selected=${c.id};render()"><td>${c.id}</td><td style="white-space:nowrap;color:var(--mute)">${c.ts.slice(11)}</td>${cols.map(col=>`<td>${sel(c,col)}</td>`).join('')}<td class="mono tag" style="white-space:nowrap">${esc(c.tag)}${c.uploaded?' <span style="color:var(--ok)" title="on device">●</span>':''}</td><td style="white-space:nowrap">${dec}</td><td class="mono">${esc(d.bytes_lsb||'')}</td><td style="white-space:nowrap"><button class="sec sm" onclick="event.stopPropagation();play(${c.id})">play</button> <button class="sec sm" onclick="event.stopPropagation();upload(${c.id})">${c.uploaded?'re-up':'to device'}</button> <button class="sec sm" onclick="event.stopPropagation();del(${c.id})">✕</button></td></tr>`}).join(''):'<tr><td colspan="12" style="color:var(--mute)">press a key on the remote…</td></tr>';
 const opts=caps.map(c=>`<option value="${c.id}">#${c.id} ${esc(c.tag||'(untagged)')}</option>`).join('');if($('cmpA').innerHTML!==opts){$('cmpA').innerHTML=opts;$('cmpB').innerHTML=opts}}
async function play(id){const r=await api('/api/play',{m:'POST',q:{id}});msg(r.message||JSON.stringify(r))}
async function upload(id,quiet){const c=caps.find(c=>c.id===id);if(!c||!c.tag)return quiet?0:msg('tag it first');const r=await api('/api/upload',{m:'POST',q:{id}});msg(r.message||JSON.stringify(r));if(!quiet)load()}
async function uploadAll(){let n=0;for(const c of caps){if(c.tag&&!c.uploaded){const r=await api('/api/upload',{m:'POST',q:{id:c.id}});if(r.ok)n++;msg(r.message)}}msg(`uploaded ${n} recording(s) to the device`);load()}
async function del(id){await api('/api/capture',{m:'DELETE',q:{id}});load()}
async function exportAll(){const r=await api('/api/export',{m:'POST'});msg(r.message)}
function bitsOf(c){const s=c.decode&&c.decode.sections||[];return s.map(x=>x.bits||'').join('|')}
function compare(){const a=caps.find(c=>c.id==$('cmpA').value),b=caps.find(c=>c.id==$('cmpB').value);if(!a||!b)return;const ba=bitsOf(a),bb=bitsOf(b);let out=`#${a.id} ${a.tag||''}\n#${b.id} ${b.tag||''}\n\n`;
 if(!ba||!bb){out+='no decoded bits for one of them';$('cmp').textContent=out;return}
 const n=Math.max(ba.length,bb.length);let la='',lb='',lm='',diffs=[];for(let i=0;i<n;i++){const x=ba[i]||' ',y=bb[i]||' ';const d=x!==y;la+=d?`<span class="diff">${x}</span>`:x;lb+=d?`<span class="diff">${y}</span>`:y;lm+=d?'^':' ';if(d)diffs.push(i)}
 out=esc(out)+`A: ${la}\nB: ${lb}\n   ${lm}\n\n`;out+=diffs.length?`${diffs.length} bit(s) differ at positions ${diffs.join(', ')} (bit 0 = first transmitted). Byte index = position / 8.`:'identical bit streams';
 const sa=(a.decode.sections||[])[0]||{},sb=(b.decode.sections||[])[0]||{};if(sa.bytes_lsb&&sb.bytes_lsb)out+=`\n\nbytes A: ${sa.bytes_lsb}\nbytes B: ${sb.bytes_lsb}`;$('cmp').innerHTML=out}
load();setInterval(load,1000);
</script></body></html>
"""

class Handler(BaseHTTPRequestHandler):
    sess = None
    def log_message(self, *a): pass

    def _json(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}") if n else {}

    def do_GET(self):
        u = urllib.parse.urlparse(self.path); q = dict(urllib.parse.parse_qsl(u.query))
        s = self.sess
        if u.path == "/":
            b = PAGE.encode(); self.send_response(200); self.send_header("Content-Type", "text/html; charset=utf-8"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
        elif u.path == "/api/state":
            with s.lock:
                caps = [{k: v for k, v in c.items() if k != "timings"} for c in s.captures]
            with s.lock:
                prev = s.captures[-1].get("state") if s.captures else None
            nxt = next_state(prev)
            self._json({"captures": caps, "status": s.status, "host": s.host, "session": os.path.basename(s.path),
                        "columns": COLUMNS, "next": {"state": nxt, "tag": tag_for(nxt)}})
        else:
            self._json({"ok": False, "message": "not found"}, 404)

    def do_POST(self):
        u = urllib.parse.urlparse(self.path); q = dict(urllib.parse.parse_qsl(u.query))
        s = self.sess
        if u.path == "/api/capture":
            c = s.update(int(q["id"]), self._body()); self._json({"ok": bool(c)})
        elif u.path == "/api/play":
            c = s.get(int(q["id"]))
            if not c: return self._json({"ok": False, "message": "no such capture"}, 404)
            # play via a temporary recording on the device
            body = ",".join(str(t) for t in c["timings"]).encode()
            r = dev_post(s.host, "/api/recordings/put?name=_mapper_tmp", body)
            if r.get("ok"):
                r = dev_post(s.host, "/api/play?name=_mapper_tmp")
                dev_post(s.host, "/api/recordings/delete?name=_mapper_tmp")
            self._json(r)
        elif u.path == "/api/upload":
            c = s.get(int(q["id"]))
            if not c or not c.get("tag"): return self._json({"ok": False, "message": "tag it first"}, 400)
            name = c["tag"].replace(" ", "_")[:23]
            r = dev_post(s.host, "/api/recordings/put?" + urllib.parse.urlencode({"name": name}), ",".join(str(t) for t in c["timings"]).encode())
            if r.get("ok"): s.update(c["id"], {"uploaded": True})
            self._json(r)
        elif u.path == "/api/export":
            out = os.path.splitext(s.path)[0] + "_export.json"
            with s.lock:
                recs = [{"name": c["tag"] or f"capture_{c['id']}", "pulses": c["pulses"], "timings": c["timings"], "note": c.get("note", "")} for c in s.captures]
            json.dump({"device": s.host, "unit": "us", "format": "mark,space,mark,...", "recordings": recs}, open(out, "w"))
            self._json({"ok": True, "message": f"wrote {out} ({len(recs)} captures) - run: python3 tools/ir_analyze.py {out}"})
        elif u.path == "/api/prune":
            with s.lock:
                s.captures = [c for c in s.captures if c.get("tag")]; s.save()
            self._json({"ok": True})
        elif u.path == "/api/device/stream/clear":
            self._json(dev_post(s.host, "/api/stream/clear"))
        else:
            self._json({"ok": False, "message": "not found"}, 404)

    def do_DELETE(self):
        u = urllib.parse.urlparse(self.path); q = dict(urllib.parse.parse_qsl(u.query))
        if u.path == "/api/capture":
            self.sess.delete(int(q["id"])); self._json({"ok": True})
        else:
            self._json({"ok": False}, 404)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="ir-blaster.local")
    ap.add_argument("--session", default="ac_map.json", help="session file (created if missing, appended to otherwise)")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--no-browser", action="store_true")
    a = ap.parse_args()
    sess = Session(a.session, a.host)
    Handler.sess = sess
    stop = threading.Event()
    threading.Thread(target=poller, args=(sess, stop), daemon=True).start()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    url = f"http://127.0.0.1:{a.port}/"
    print(f"IR key mapper: {url}  device: {a.host}  session: {a.session}  ({len(sess.captures)} captures loaded)")
    if not a.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    stop.set()
    print(f"\nsession saved to {a.session}")

if __name__ == "__main__":
    main()
