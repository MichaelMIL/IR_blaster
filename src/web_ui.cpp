// Built-in web page + JSON API, served by the device on port 80.
#include <WiFi.h>
#include <WebServer.h>
#include <mDNS.h>
#include "app.h"
#include "config.h"

static WebServer server(80);
static mDNS      mdns;
static bool      started = false;

static const char INDEX_HTML[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>IR Blaster</title>
<style>
:root{--bg:#0f1115;--card:#171a21;--line:#262a33;--fg:#e6e8ec;--mute:#8b92a1;--acc:#4f8cff;--ok:#3ddc84;--warn:#ffb02e;--bad:#ff5c5c}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{padding:16px 20px;border-bottom:1px solid var(--line);display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px}
h1{font-size:18px;margin:0}#net{color:var(--mute);font-size:13px}
main{max-width:980px;margin:0 auto;padding:16px;display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(300px,1fr))}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px}
h2{font-size:14px;margin:0 0 12px;color:var(--mute);text-transform:uppercase;letter-spacing:.06em}
label{display:block;font-size:13px;color:var(--mute);margin:8px 0 4px}
input,select{width:100%;padding:8px 10px;background:#0c0e12;border:1px solid var(--line);border-radius:6px;color:var(--fg);font:inherit}
button{padding:8px 14px;border:0;border-radius:6px;background:var(--acc);color:#fff;font:inherit;cursor:pointer}
button.sec{background:#2a2f3a}button:disabled{opacity:.5;cursor:default}
.row{display:flex;gap:8px;align-items:end;flex-wrap:wrap}.row>*{flex:1}.row>button{flex:0 0 auto}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
table{width:100%;border-collapse:collapse;font-size:13px}td,th{padding:6px 4px;border-bottom:1px solid var(--line);text-align:left}th{color:var(--mute);font-weight:500}
code{font-family:ui-monospace,Menlo,monospace;background:#0c0e12;padding:1px 5px;border-radius:4px}
#log{font-family:ui-monospace,Menlo,monospace;font-size:12px;color:var(--mute);white-space:pre-wrap;max-height:160px;overflow:auto;margin-top:10px}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--bad);margin-right:6px}.dot.on{background:var(--ok)}
.wide{grid-column:1/-1}
</style></head><body>
<header><h1>IR Blaster <span style="color:var(--mute);font-weight:400">CB3S</span></h1><div id="net">connecting…</div></header>
<main>
<section><h2>Send</h2>
 <div class="grid"><div><label>Address</label><input id="addr" placeholder="0x04"></div><div><label>Command</label><input id="cmd" placeholder="0x08"></div></div>
 <div class="row"><button onclick="sendNec()">Send NEC</button></div>
 <label>Raw 32-bit NEC word</label><div class="row"><input id="raw" placeholder="0xF708FB04"><button onclick="sendRaw()">Send raw</button></div>
 <div id="txmsg" style="margin-top:8px;color:var(--mute);font-size:13px"></div>
</section>
<section><h2>Received <span id="rxcount" style="text-transform:none;letter-spacing:0"></span></h2>
 <table><thead><tr><th>age</th><th>type</th><th>addr</th><th>cmd</th><th>raw</th><th></th></tr></thead><tbody id="rx"><tr><td colspan="6" style="color:var(--mute)">point a remote at the device…</td></tr></tbody></table>
 <div class="row" style="margin-top:10px"><button class="sec" onclick="api('/api/received/clear',{m:'POST'}).then(poll)">Clear</button></div>
</section>
<section class="wide"><h2>Pins</h2>
 <div class="grid" style="grid-template-columns:repeat(auto-fit,minmax(170px,1fr))">
  <div><label>LED 1</label><div class="row"><select id="led1"></select><button class="sec" onclick="test(1)">Test</button></div></div>
  <div><label>LED 2</label><div class="row"><select id="led2"></select><button class="sec" onclick="test(2)">Test</button></div></div>
  <div><label>LED 3</label><div class="row"><select id="led3"></select><button class="sec" onclick="test(3)">Test</button></div></div>
  <div><label>Receiver</label><div class="row"><select id="rx_pin"></select></div></div>
 </div>
 <div class="row" style="margin-top:12px"><button onclick="savePins()">Save pins</button><button class="sec" onclick="test('all')">Test all LEDs</button><button class="sec" id="ledscan" onclick="ledScan()">Find LEDs (blink each GPIO)</button><button class="sec" id="rxscan" onclick="rxScan()">Find receiver (10 s)</button></div>
 <div id="log"></div>
</section>
<section><h2>WiFi</h2>
 <label>Network</label><input id="ssid"><label>Password</label><input id="pass" type="password">
 <div class="row" style="margin-top:10px"><button onclick="saveWifi()">Save &amp; connect</button><button class="sec" onclick="if(confirm('Reboot the device?'))api('/api/reboot',{m:'POST'})">Reboot</button></div>
</section>
<section><h2>Status</h2><table id="st"></table></section>
</main>
<script>
const $=id=>document.getElementById(id);
const log=m=>{const l=$('log');l.textContent+=m+"\n";l.scrollTop=l.scrollHeight};
async function api(p,{m='GET',q={}}={}){const u=new URL(p,location.href);for(const k in q)u.searchParams.set(k,q[k]);const r=await fetch(u,{method:m});const t=await r.text();if(!r.ok)throw new Error(t||r.status);try{return JSON.parse(t)}catch{return t}}
const hex=(v,w)=>'0x'+v.toString(16).toUpperCase().padStart(w,'0');
let cands=[],lastSeq=-1;
function fillSel(id,val){const s=$(id);s.innerHTML='<option value="none">none</option>'+cands.map(p=>`<option value="${p}">P${p}</option>`).join('');s.value=val==null?'none':val}
async function status(){const s=await api('/api/status');cands=s.candidates;
 $('net').innerHTML=`<span class="dot ${s.wifi?'on':''}"></span>${s.wifi?s.ssid+' · '+s.ip+' · '+s.rssi+' dBm':'wifi not connected'}`;
 if(document.activeElement.tagName!=='SELECT'){fillSel('led1',s.pins.led1);fillSel('led2',s.pins.led2);fillSel('led3',s.pins.led3);fillSel('rx_pin',s.pins.rx)}
 if(!$('ssid').value)$('ssid').value=s.ssid||'';
 $('st').innerHTML=Object.entries({firmware:s.firmware,uptime:s.uptime+' s',ip:s.ip,hostname:s.hostname+'.local',rssi:s.rssi+' dBm',carrier:s.carrier+' Hz',rx_idle:s.rx_idle}).map(([k,v])=>`<tr><th>${k}</th><td>${v}</td></tr>`).join('')}
async function poll(){try{const r=await api('/api/received');if(r.seq===lastSeq)return;lastSeq=r.seq;$('rxcount').textContent=r.frames.length?`(${r.frames.length})`:'';
 const now=r.now;$('rx').innerHTML=r.frames.length?r.frames.slice().reverse().map(f=>`<tr><td>${Math.round((now-f.t)/1000)}s</td><td>${f.type}</td><td>${f.type==='nec'?hex(f.addr,2):''}</td><td>${f.type==='nec'?hex(f.cmd,2):''}</td><td>${f.type==='nec'?'<code>'+hex(f.raw,8)+'</code>':f.pulses+' pulses'}</td><td>${f.type==='nec'?`<button class="sec" onclick="api('/api/send',{m:'POST',q:{raw:'${hex(f.raw,8)}'}})">send</button>`:''}</td></tr>`).join(''):'<tr><td colspan="6" style="color:var(--mute)">nothing received yet</td></tr>'}catch(e){}}
async function sendNec(){try{const r=await api('/api/send',{m:'POST',q:{addr:$('addr').value,cmd:$('cmd').value}});$('txmsg').textContent=r.message}catch(e){$('txmsg').textContent='error: '+e.message}}
async function sendRaw(){try{const r=await api('/api/send',{m:'POST',q:{raw:$('raw').value}});$('txmsg').textContent=r.message}catch(e){$('txmsg').textContent='error: '+e.message}}
async function savePins(){const r=await api('/api/pins',{m:'POST',q:{led1:$('led1').value,led2:$('led2').value,led3:$('led3').value,rx:$('rx_pin').value}});log(r.message);status()}
async function test(n){log(`testing LED ${n} for 3 s…`);const r=await api('/api/test',{m:'POST',q:{led:n,sec:3}});log(r.message)}
async function ledScan(){const b=$('ledscan');b.disabled=true;log('LED scan: watch the LEDs through a phone camera. Each GPIO blinks for 2 s.');
 for(const p of cands){log(`  blinking P${p} …`);try{await api('/api/blink',{m:'POST',q:{pin:p,sec:2}})}catch(e){log('  error '+e.message)}}
 log('LED scan done. Pick the GPIOs that blinked above and press Save pins.');b.disabled=false}
async function rxScan(){const b=$('rxscan');b.disabled=true;log('Receiver scan: press buttons on a remote pointed at the device for 10 s…');
 try{const r=await api('/api/scan',{m:'POST',q:{sec:10}});r.pins.filter(p=>p.transitions>0).forEach(p=>log(`  P${p.pin}: ${p.transitions} transitions, idle ${p.idle?'HIGH':'LOW'}`));
 if(r.best!=null){log(`=> receiver is most likely P${r.best}`);$('rx_pin').value=r.best}else log('=> no IR activity seen on any GPIO')}catch(e){log('error '+e.message)}b.disabled=false}
async function saveWifi(){const r=await api('/api/wifi',{m:'POST',q:{ssid:$('ssid').value,pass:$('pass').value}});log(r.message)}
status();poll();setInterval(poll,1500);setInterval(status,5000);
</script></body></html>
)HTML";

// ------------------------------------------------------------------ helpers

static String jsonStr(const String &s) {
    String o = "\"";
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if ((uint8_t)c < 0x20) continue;
        else o += c;
    }
    return o + "\"";
}

static String pinJson(uint8_t p) { return p == NO_PIN ? String("null") : String(p); }

static void ok(const String &message) {
    server.send(200, "application/json", "{\"ok\":true,\"message\":" + jsonStr(message) + "}");
}
static void fail(int code, const String &message) {
    server.send(code, "application/json", "{\"ok\":false,\"message\":" + jsonStr(message) + "}");
}

static bool parsePinArg(const String &s, uint8_t &out) {
    if (s == "none" || s == "" || s == "null") { out = NO_PIN; return true; }
    const char *p = s.c_str();
    if (*p == 'P' || *p == 'p') p++;
    char *end; long v = strtol(p, &end, 10);
    if (*end || v < 0 || v > 26 || v == 10 || v == 11) return false;
    out = (uint8_t)v;
    return true;
}

// ----------------------------------------------------------------- handlers

static void handleRoot() {
    server.send_P(200, "text/html", INDEX_HTML);
}

static void handleStatus() {
    String j = "{";
    j += "\"firmware\":\"ir_blaster / LibreTiny " + String(lt_get_version()) + "\",";
    j += "\"hostname\":\"ir-blaster\",";
    j += "\"uptime\":" + String(millis() / 1000) + ",";
    j += "\"wifi\":" + String(appWifiConnected() ? "true" : "false") + ",";
    j += "\"ssid\":" + jsonStr(cfg.ssid) + ",";
    j += "\"ip\":\"" + appIp() + "\",";
    j += "\"rssi\":" + String(appWifiConnected() ? WiFi.RSSI() : 0) + ",";
    j += "\"carrier\":" + String(IR_CARRIER_HZ) + ",";
    j += "\"rx_idle\":\"" + String(cfg.rxPin == NO_PIN ? "n/a" : (digitalRead(cfg.rxPin) ? "HIGH" : "LOW (no receiver?)")) + "\",";
    j += "\"pins\":{\"led1\":" + pinJson(cfg.ledPin[0]) + ",\"led2\":" + pinJson(cfg.ledPin[1]) +
         ",\"led3\":" + pinJson(cfg.ledPin[2]) + ",\"rx\":" + pinJson(cfg.rxPin) + "},";
    j += "\"candidates\":[";
    for (int i = 0; i < CANDIDATE_COUNT; i++) j += String(CANDIDATE_PINS[i]) + (i + 1 < CANDIDATE_COUNT ? "," : "");
    j += "]}";
    server.send(200, "application/json", j);
}

static void handleReceived() {
    RxRecord rec[RX_HISTORY];
    int n = appGetReceived(rec, RX_HISTORY);
    String j = "{\"seq\":" + String(appReceivedSeq()) + ",\"now\":" + String(millis()) + ",\"frames\":[";
    for (int i = 0; i < n; i++) {
        const RxRecord &r = rec[i];
        j += "{\"t\":" + String(r.t) + ",\"type\":\"" + (r.type == 1 ? "nec" : r.type == 2 ? "repeat" : "raw") + "\"";
        if (r.type == 1) j += ",\"addr\":" + String(r.address) + ",\"cmd\":" + String(r.command) + ",\"raw\":" + String(r.raw);
        j += ",\"pulses\":" + String(r.pulses) + "}" + (i + 1 < n ? "," : "");
    }
    j += "]}";
    server.send(200, "application/json", j);
}

static void handleSend() {
    if (appTxPinCount() == 0) return fail(400, "no LED pins assigned");
    if (server.hasArg("raw") && server.arg("raw").length()) {
        uint32_t word = strtoul(server.arg("raw").c_str(), nullptr, 0);
        appSendRaw(word);
        char b[16]; snprintf(b, sizeof(b), "0x%08lX", (unsigned long)word);
        return ok("sent raw " + String(b));
    }
    unsigned a, c;
    if (sscanf(server.arg("addr").c_str(), "%i", &a) != 1 || sscanf(server.arg("cmd").c_str(), "%i", &c) != 1)
        return fail(400, "need addr and cmd (e.g. 0x04 0x08) or raw");
    appSendNec(a, c);
    char b[32]; snprintf(b, sizeof(b), "sent NEC addr=0x%02X cmd=0x%02X", a & 0xFF, c & 0xFF);
    ok(b);
}

static void handlePins() {
    uint8_t led[LED_COUNT], rxp;
    const char *names[LED_COUNT] = {"led1", "led2", "led3"};
    for (int i = 0; i < LED_COUNT; i++)
        if (!parsePinArg(server.arg(names[i]), led[i])) return fail(400, String("bad pin for ") + names[i]);
    if (!parsePinArg(server.arg("rx"), rxp)) return fail(400, "bad pin for rx");
    for (int i = 0; i < LED_COUNT; i++) cfg.ledPin[i] = led[i];
    cfg.rxPin = rxp;
    cfg.save();
    appApplyPins();
    ok("pins saved");
}

static void handleTest() {
    uint32_t sec = server.arg("sec").toInt(); if (sec == 0 || sec > 30) sec = 3;
    String led = server.arg("led");
    int idx = led == "all" ? -1 : led.toInt() - 1;
    if (idx < -1 || idx >= LED_COUNT) return fail(400, "led must be 1-3 or all");
    if (idx >= 0 && cfg.ledPin[idx] == NO_PIN) return fail(400, "that LED has no pin assigned");
    if (idx < 0 && appTxPinCount() == 0) return fail(400, "no LED pins assigned");
    appLedTest(idx, sec);
    ok("blinked " + (idx < 0 ? String("all LEDs") : "LED " + String(idx + 1)) + " for " + String(sec) + " s");
}

static void handleBlink() {
    uint8_t pin;
    if (!parsePinArg(server.arg("pin"), pin) || pin == NO_PIN || !isCandidatePin(pin)) return fail(400, "bad pin");
    uint32_t sec = server.arg("sec").toInt(); if (sec == 0 || sec > 10) sec = 2;
    appBlinkPin(pin, sec);
    ok("blinked P" + String(pin));
}

static void handleScan() {
    uint32_t sec = server.arg("sec").toInt(); if (sec == 0 || sec > 30) sec = 10;
    ScanResult r;
    appRxScan(sec, r);
    String j = "{\"best\":" + (r.best < 0 ? String("null") : String(CANDIDATE_PINS[r.best])) + ",\"pins\":[";
    for (int i = 0; i < CANDIDATE_COUNT; i++)
        j += "{\"pin\":" + String(CANDIDATE_PINS[i]) + ",\"transitions\":" + String(r.transitions[i]) +
             ",\"idle\":" + String(r.idle[i]) + "}" + (i + 1 < CANDIDATE_COUNT ? "," : "");
    j += "]}";
    server.send(200, "application/json", j);
}

static void handleWifi() {
    if (!server.arg("ssid").length()) return fail(400, "ssid required");
    cfg.ssid = server.arg("ssid");
    if (server.hasArg("pass") && server.arg("pass").length()) cfg.pass = server.arg("pass");
    cfg.save();
    ok("saved; connecting to " + cfg.ssid + " (this page will drop if the network changes)");
    delay(200);
    appWifiBegin();
}

static void handleReboot() {
    ok("rebooting");
    delay(200);
    lt_reboot();
}

// ------------------------------------------------------------------ public

void webBegin() {
    if (started) return;
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/received", HTTP_GET, handleReceived);
    server.on("/api/received/clear", HTTP_POST, []() { appClearReceived(); ok("cleared"); });
    server.on("/api/send", HTTP_POST, handleSend);
    server.on("/api/pins", HTTP_POST, handlePins);
    server.on("/api/test", HTTP_POST, handleTest);
    server.on("/api/blink", HTTP_POST, handleBlink);
    server.on("/api/scan", HTTP_POST, handleScan);
    server.on("/api/wifi", HTTP_POST, handleWifi);
    server.on("/api/reboot", HTTP_POST, handleReboot);
    server.onNotFound([]() { fail(404, "not found"); });
    server.begin();
    if (mdns.begin("ir-blaster")) mdns.addService((char *)"http", (char *)"tcp", 80);
    started = true;
    Serial.printf("web: http://%s/  (http://ir-blaster.local/)\n", appIp().c_str());
}

void webLoop() {
    if (started) server.handleClient();
}
