// Built-in web page + JSON API, served by the device on port 80.
#include <WiFi.h>
#include <WebServer.h>
#include <mDNS.h>
#include <Update.h>
#include "app.h"
#include "config.h"
#include "recordings.h"
#include "buttons.h"
#include "mqtt.h"
#include "ac.h"

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
<header><h1>IR Blaster <span style="color:var(--mute);font-weight:400">CB3S</span> <a href="/remote" style="color:var(--acc);font-size:13px;font-weight:400;margin-left:10px">open remote page →</a></h1><div id="net">connecting…</div></header>
<main>
<section><h2>Send</h2>
 <div class="grid"><div><label>Address</label><input id="addr" placeholder="0x04"></div><div><label>Command</label><input id="cmd" placeholder="0x08"></div></div>
 <div class="row"><button onclick="sendNec()">Send NEC</button></div>
 <label>Raw 32-bit NEC word</label><div class="row"><input id="raw" placeholder="0xF708FB04"><button onclick="sendRaw()">Send raw</button></div>
 <div id="txmsg" style="margin-top:8px;color:var(--mute);font-size:13px"></div>
</section>
<section><h2>Air conditioner <span style="text-transform:none;letter-spacing:0;color:var(--mute)">(native protocol)</span></h2>
 <div class="grid" style="grid-template-columns:repeat(3,1fr)">
  <div><label>Mode</label><select id="acmode"><option>cool</option><option>heat</option><option>dry</option><option>fan</option><option>auto</option></select></div>
  <div><label>Temp °C</label><select id="actemp"></select></div>
  <div><label>Fan</label><select id="acfan"><option>auto</option><option>low</option><option>med</option><option>high</option></select></div>
 </div>
 <div class="row" style="margin-top:10px"><button onclick="acSend(true)">Send (on)</button><button class="sec" onclick="acSend(false)">Off</button><span id="acmsg" style="color:var(--mute);font-size:13px"></span></div>
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
  <div><label>Status LED (blue)</label><div class="row"><select id="sled"></select><button class="sec" onclick="api('/api/sled/test',{m:'POST'})">Test</button></div><label style="display:flex;gap:6px;align-items:center;margin-top:6px"><input type="checkbox" id="sinv" style="width:auto">on when pin is LOW (inverted)</label></div>
 </div>
 <div class="row" style="margin-top:12px"><button onclick="savePins()">Save pins</button><button class="sec" onclick="test('all')">Test all LEDs</button><button class="sec" id="ledscan" onclick="ledScan()">Find LEDs (blink each GPIO)</button><button class="sec" id="rxscan" onclick="rxScan()">Find receiver (10 s)</button></div>
 <div id="log"></div>
</section>
<section class="wide"><h2>Remote buttons <span style="text-transform:none;letter-spacing:0;color:var(--mute)">(shown on <a href="/remote" style="color:var(--acc)">/remote</a>)</span></h2>
 <div class="grid" style="grid-template-columns:repeat(auto-fit,minmax(150px,1fr))">
  <div><label>Label</label><input id="blabel" placeholder="AC on" maxlength="16"></div>
  <div><label>Action</label><select id="btype" onchange="btype()"><option value="nec">NEC code</option><option value="rec">Recording</option></select></div>
  <div id="bnec1"><label>Address</label><input id="baddr" placeholder="0xBA"></div>
  <div id="bnec2"><label>Command</label><input id="bcmd" placeholder="0x1E"></div>
  <div id="brec" style="display:none"><label>Recording</label><select id="brecsel"></select></div>
  <div><label>Repeat</label><select id="brep"><option>1</option><option>2</option><option>3</option></select></div>
 </div>
 <div class="row" style="margin-top:10px"><button onclick="addBtn()">Add button</button><button class="sec" onclick="fromLast()">Fill from last received NEC</button></div>
 <table style="margin-top:12px"><thead><tr><th>#</th><th>label</th><th>action</th><th></th></tr></thead><tbody id="btns"><tr><td colspan="4" style="color:var(--mute)">no buttons yet</td></tr></tbody></table>
 <div id="btnmsg" style="margin-top:8px;color:var(--mute);font-size:13px"></div>
</section>
<section class="wide"><h2>Live capture <span style="text-transform:none;letter-spacing:0;color:var(--mute)">(stream of everything the receiver sees)</span></h2>
 <div class="row"><button id="strbtn" onclick="streamToggle()">Start stream</button><button class="sec" onclick="streamShowRecent()">Show recent</button><button class="sec" onclick="streamClear()">Clear</button>
  <div><label>Record a sequence: names separated by commas, one key press each</label><div class="row"><input id="seqnames" placeholder="ac_off, ac_cool_22, ac_cool_23"><button class="sec" id="seqbtn" onclick="recSeq()">Record sequence</button></div></div></div>
 <div id="seqmsg" style="margin-top:8px;color:var(--mute);font-size:13px"></div>
 <table style="margin-top:12px"><thead><tr><th>id</th><th>age</th><th>decode</th><th>pulses</th><th>timings (µs)</th><th></th></tr></thead><tbody id="str"></tbody></table>
</section>
<section class="wide"><h2>Recordings <span style="text-transform:none;letter-spacing:0;color:var(--mute)">(any remote, e.g. air conditioner)</span></h2>
 <div class="row"><div><label>Name</label><input id="recname" placeholder="ac_cool_24"></div><button onclick="record()" id="recbtn">Record next key press</button><button class="sec" onclick="saveLast()">Save last received</button></div>
 <table style="margin-top:12px"><thead><tr><th>name</th><th>pulses</th><th>length</th><th></th></tr></thead><tbody id="recs"><tr><td colspan="4" style="color:var(--mute)">no recordings yet</td></tr></tbody></table>
 <div id="recmsg" style="margin-top:8px;color:var(--mute);font-size:13px"></div>
</section>
<section><h2>Export &amp; analysis</h2>
 <p style="margin:0 0 10px;color:var(--mute);font-size:13px">Download every recording plus the last capture as JSON, then run <code>tools/ir_analyze.py ir_export.json</code> from the repo.</p>
 <div class="row"><a href="/api/export" download="ir_export.json"><button>Download ir_export.json</button></a><button class="sec" onclick="copyLast()">Copy last capture</button></div>
 <div id="expmsg" style="margin-top:8px;color:var(--mute);font-size:13px"></div>
</section>
<section><h2>Firmware update (OTA)</h2>
 <p style="margin:0 0 10px;color:var(--mute);font-size:13px">Upload <code>.pio/build/cb3s/firmware.uf2</code>. The device reboots when done.</p>
 <input type="file" id="fw" accept=".uf2"><div class="row" style="margin-top:10px"><button onclick="ota()" id="otabtn">Upload &amp; flash</button></div>
 <div id="otamsg" style="margin-top:8px;color:var(--mute);font-size:13px"></div>
</section>
<section><h2>WiFi</h2>
 <label>Network</label><input id="ssid"><label>Password</label><input id="pass" type="password">
 <div class="row" style="margin-top:10px"><button onclick="saveWifi()">Save &amp; connect</button><button class="sec" onclick="if(confirm('Reboot the device?'))api('/api/reboot',{m:'POST'})">Reboot</button></div>
</section>
<section><h2>Home Assistant (MQTT)</h2>
 <div class="grid"><div><label>Broker host</label><input id="mhost" placeholder="homeassistant.local"></div><div><label>Port</label><input id="mport" placeholder="1883"></div></div>
 <div class="grid"><div><label>Username</label><input id="muser"></div><div><label>Password</label><input id="mpass" type="password" placeholder="(unchanged)"></div></div>
 <div class="row" style="margin-top:10px"><button onclick="saveMqtt()">Save &amp; connect</button><button class="sec" onclick="if(confirm('Disable MQTT?')){$('mhost').value='';saveMqtt()}">Disable</button></div>
 <div id="mqttmsg" style="margin-top:8px;color:var(--mute);font-size:13px"></div>
 <p style="margin:10px 0 0;color:var(--mute);font-size:12px">With MQTT discovery enabled in Home Assistant, the device and one button entity per remote button appear automatically, plus a "Last IR code" sensor.</p>
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
 if(document.activeElement.tagName!=='SELECT'){fillSel('led1',s.pins.led1);fillSel('led2',s.pins.led2);fillSel('led3',s.pins.led3);fillSel('rx_pin',s.pins.rx);fillSel('sled',s.pins.status);$('sinv').checked=!!s.pins.status_invert}
 if(!$('ssid').value)$('ssid').value=s.ssid||'';
 if(!mqttTouched){$('mhost').value=s.mqtt.host||'';$('mport').value=s.mqtt.port||1883;$('muser').value=s.mqtt.user||''}
 $('mqttmsg').textContent='status: '+s.mqtt.state;
 if(s.ac&&!acTouched){$('acmode').value=s.ac.mode;$('actemp').value=s.ac.temp;$('acfan').value=s.ac.fan;if(!$('acmsg').textContent)$('acmsg').textContent='last: '+(s.ac.power==='on'?s.ac.mode+' '+s.ac.temp+'°C fan '+s.ac.fan:'off')}
 $('st').innerHTML=Object.entries({firmware:s.firmware,uptime:s.uptime+' s',ip:s.ip,hostname:s.hostname+'.local',rssi:s.rssi+' dBm',carrier:s.carrier+' Hz',rx_idle:s.rx_idle,mqtt:s.mqtt.state+(s.mqtt.host?' ('+s.mqtt.host+':'+s.mqtt.port+')':''),mqtt_topic:s.mqtt.base}).map(([k,v])=>`<tr><th>${k}</th><td>${v}</td></tr>`).join('')}
async function poll(){try{const r=await api('/api/received');if(r.seq===lastSeq)return;lastSeq=r.seq;$('rxcount').textContent=r.frames.length?`(${r.frames.length})`:'';
 const necs=r.frames.filter(f=>f.type==='nec');if(necs.length)lastNec=necs[necs.length-1];const now=r.now;$('rx').innerHTML=r.frames.length?r.frames.slice().reverse().map(f=>`<tr><td>${Math.round((now-f.t)/1000)}s</td><td>${f.type}</td><td>${f.type==='nec'?hex(f.addr,2):''}</td><td>${f.type==='nec'?hex(f.cmd,2):''}</td><td>${f.type==='nec'?'<code>'+hex(f.raw,8)+'</code>':f.pulses+' pulses (raw)'}</td><td>${f.type==='nec'?`<button class="sec" onclick="api('/api/send',{m:'POST',q:{raw:'${hex(f.raw,8)}'}})">send</button>`:(f===r.frames[r.frames.length-1]?`<button class="sec" onclick="saveNewest()">save…</button>`:'')}</td></tr>`).join(''):'<tr><td colspan="6" style="color:var(--mute)">nothing received yet</td></tr>'}catch(e){}}
for(let t=16;t<=31;t++){const o=document.createElement('option');o.textContent=t;$('actemp').appendChild(o)}$('actemp').value='24';
let acTouched=false;['acmode','actemp','acfan'].forEach(id=>$(id).addEventListener('change',()=>acTouched=true));
async function acSend(on){try{const q=on?{power:'on',mode:$('acmode').value,temp:$('actemp').value,fan:$('acfan').value}:{power:'off'};const r=await api('/api/ac',{m:'POST',q});$('acmsg').textContent=r.message;acTouched=false}catch(e){$('acmsg').textContent='error: '+e.message}}
async function sendNec(){try{const r=await api('/api/send',{m:'POST',q:{addr:$('addr').value,cmd:$('cmd').value}});$('txmsg').textContent=r.message}catch(e){$('txmsg').textContent='error: '+e.message}}
async function sendRaw(){try{const r=await api('/api/send',{m:'POST',q:{raw:$('raw').value}});$('txmsg').textContent=r.message}catch(e){$('txmsg').textContent='error: '+e.message}}
async function savePins(){const r=await api('/api/pins',{m:'POST',q:{led1:$('led1').value,led2:$('led2').value,led3:$('led3').value,rx:$('rx_pin').value,status:$('sled').value,status_invert:$('sinv').checked?1:0}});log(r.message);status()}
function btype(){const rec=$('btype').value==='rec';$('bnec1').style.display=$('bnec2').style.display=rec?'none':'';$('brec').style.display=rec?'':'none'}
let lastNec=null;
async function btns(){try{const r=await api('/api/buttons');$('btns').innerHTML=r.length?r.map((b,i)=>`<tr><td>${i+1}</td><td>${b.label}</td><td>${b.type==='nec'?'NEC '+hex(b.addr,2)+' / '+hex(b.cmd,2):'rec <code>'+b.rec+'</code>'}${b.repeat>1?' ×'+b.repeat:''}</td><td style="text-align:right"><button class="sec" onclick="api('/api/button',{m:'POST',q:{id:${i}}}).then(r=>log(r.message))">Test</button> <button class="sec" onclick="api('/api/buttons/move',{m:'POST',q:{id:${i},dir:-1}}).then(btns)">↑</button> <button class="sec" onclick="api('/api/buttons/move',{m:'POST',q:{id:${i},dir:1}}).then(btns)">↓</button> <button class="sec" onclick="if(confirm('Delete ${b.label}?'))api('/api/buttons/delete',{m:'POST',q:{id:${i}}}).then(btns)">Delete</button></td></tr>`).join(''):'<tr><td colspan="4" style="color:var(--mute)">no buttons yet</td></tr>'}catch(e){}}
async function addBtn(){const q={label:$('blabel').value.trim(),type:$('btype').value,repeat:$('brep').value};if(!q.label)return $('btnmsg').textContent='enter a label';
 if(q.type==='nec'){q.addr=$('baddr').value;q.cmd=$('bcmd').value}else q.rec=$('brecsel').value;
 try{const r=await api('/api/buttons/set',{m:'POST',q});$('btnmsg').textContent=r.message;$('blabel').value='';btns()}catch(e){$('btnmsg').textContent='error: '+e.message}}
function fromLast(){if(!lastNec)return $('btnmsg').textContent='no NEC frame received yet - press the remote at the device first';$('btype').value='nec';btype();$('baddr').value=hex(lastNec.addr,2);$('bcmd').value=hex(lastNec.cmd,2);$('btnmsg').textContent='filled from last received frame'}
async function recs(){try{const r=await api('/api/recordings');$('brecsel').innerHTML=r.map(x=>`<option>${x.name}</option>`).join('');$('recs').innerHTML=r.length?r.map(x=>`<tr><td><code>${x.name}</code></td><td>${x.pulses}</td><td>${x.ms} ms</td><td style="text-align:right"><button onclick="play('${x.name}')">Play</button> <button class="sec" onclick="if(confirm('Delete ${x.name}?'))api('/api/recordings/delete',{m:'POST',q:{name:'${x.name}'}}).then(recs)">Delete</button></td></tr>`).join(''):'<tr><td colspan="4" style="color:var(--mute)">no recordings yet</td></tr>'}catch(e){}}
async function record(){const n=$('recname').value.trim();if(!n)return $('recmsg').textContent='enter a name first';const b=$('recbtn');b.disabled=true;$('recmsg').textContent=`recording '${n}': press the key on the remote now (15 s)…`;
 try{const r=await api('/api/record',{m:'POST',q:{name:n,sec:15}});$('recmsg').textContent=r.message}catch(e){$('recmsg').textContent='error: '+e.message}b.disabled=false;recs()}
let streaming=false,streamAfter=0,streamTimer=null;
async function streamToggle(){streaming=!streaming;$('strbtn').textContent=streaming?'Stop stream':'Start stream';$('strbtn').className=streaming?'sec':'';
 if(streaming){try{const r=await api('/api/stream',{q:{after:4294967295}});streamAfter=r.last}catch(e){streamAfter=0}streamTick()}else clearTimeout(streamTimer)}
async function streamShowRecent(){const was=streaming;streaming=false;clearTimeout(streamTimer);streamAfter=0;streaming=true;await streamTick();streaming=was;if(!was)clearTimeout(streamTimer)}
async function streamClear(){$('str').innerHTML='';try{await api('/api/stream/clear',{m:'POST'})}catch(e){}}
async function streamTick(){if(!streaming)return;try{const r=await api('/api/stream',{q:{after:streamAfter}});if(r.captures.length){streamAfter=r.last;const tb=$('str');
 for(const c of r.captures){const tr=document.createElement('tr');const dec=c.type==='nec'?`NEC ${hex(c.addr,2)} / ${hex(c.cmd,2)}`:c.type;
  tr.innerHTML=`<td>${c.id}</td><td>${Math.round((r.now-c.t)/1000)}s</td><td>${dec}</td><td>${c.pulses}</td><td style="font-family:ui-monospace,Menlo,monospace;font-size:12px;max-width:380px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="${c.timings.join(',')}">${c.timings.slice(0,14).join(' ')}${c.pulses>14?' …':''}</td><td style="white-space:nowrap"><button class="sec" onclick="navigator.clipboard.writeText('${c.timings.join(',')}')">copy</button> <button class="sec" onclick="streamSave(${c.id})">save…</button></td>`;
  tb.prepend(tr)}while(tb.children.length>40)tb.lastChild.remove()}}catch(e){}streamTimer=setTimeout(streamTick,700)}
async function streamSave(id){const n=prompt('Name for this capture (no spaces):');if(!n)return;try{const r=await api('/api/stream/save',{m:'POST',q:{id,name:n.trim()}});$('recmsg').textContent=r.message;recs()}catch(e){alert(e.message)}}
async function recSeq(){const names=$('seqnames').value.split(',').map(x=>x.trim()).filter(Boolean);if(!names.length)return $('seqmsg').textContent='enter at least one name';const b=$('seqbtn');b.disabled=true;
 for(let i=0;i<names.length;i++){$('seqmsg').textContent=`(${i+1}/${names.length}) press the key for '${names[i]}' now… (20 s)`;try{const r=await api('/api/record',{m:'POST',q:{name:names[i],sec:20}});$('seqmsg').textContent=`(${i+1}/${names.length}) ${r.message}`;await new Promise(r=>setTimeout(r,600))}catch(e){$('seqmsg').textContent=`stopped at '${names[i]}': ${e.message}`;break}}
 b.disabled=false;recs()}
async function saveNewest(){const n=prompt('Name for this recording (no spaces):');if(!n)return;try{const r=await api('/api/record/last',{m:'POST',q:{name:n.trim()}});$('recmsg').textContent=r.message;recs()}catch(e){alert(e.message)}}
async function saveLast(){const n=$('recname').value.trim();if(!n)return $('recmsg').textContent='enter a name first';try{const r=await api('/api/record/last',{m:'POST',q:{name:n}});$('recmsg').textContent=r.message}catch(e){$('recmsg').textContent='error: '+e.message}recs()}
async function play(n){try{const r=await api('/api/play',{m:'POST',q:{name:n}});$('recmsg').textContent=r.message}catch(e){$('recmsg').textContent='error: '+e.message}}
async function test(n){log(`testing LED ${n} for 3 s…`);const r=await api('/api/test',{m:'POST',q:{led:n,sec:3}});log(r.message)}
async function ledScan(){const b=$('ledscan');b.disabled=true;log('LED scan: watch the LEDs through a phone camera. Each GPIO blinks for 2 s.');
 for(const p of cands){log(`  blinking P${p} …`);try{await api('/api/blink',{m:'POST',q:{pin:p,sec:2}})}catch(e){log('  error '+e.message)}}
 log('LED scan done. Pick the GPIOs that blinked above and press Save pins.');b.disabled=false}
async function rxScan(){const b=$('rxscan');b.disabled=true;log('Receiver scan: press buttons on a remote pointed at the device for 10 s…');
 try{const r=await api('/api/scan',{m:'POST',q:{sec:10}});r.pins.filter(p=>p.transitions>0).forEach(p=>log(`  P${p.pin}: ${p.transitions} transitions, idle ${p.idle?'HIGH':'LOW'}`));
 if(r.best!=null){log(`=> receiver is most likely P${r.best}`);$('rx_pin').value=r.best}else log('=> no IR activity seen on any GPIO')}catch(e){log('error '+e.message)}b.disabled=false}
async function copyLast(){try{const r=await api('/api/last');if(!r.pulses)return $('expmsg').textContent='nothing captured yet';await navigator.clipboard.writeText(JSON.stringify(r));$('expmsg').textContent=`copied ${r.pulses} pulses to clipboard`}catch(e){$('expmsg').textContent='error: '+e.message}}
async function ota(){const f=$('fw').files[0];if(!f)return $('otamsg').textContent='choose a .uf2 file first';const b=$('otabtn');b.disabled=true;$('otamsg').textContent=`uploading ${f.name} (${Math.round(f.size/1024)} KB)…`;
 const fd=new FormData();fd.append('file',f);try{const r=await fetch('/update',{method:'POST',body:fd});const j=await r.json();$('otamsg').textContent=j.message+(j.ok?' — reload this page in ~15 s':'')}catch(e){$('otamsg').textContent='upload ended: '+e.message+' (device may be rebooting)'}b.disabled=false}
let mqttTouched=false;['mhost','mport','muser','mpass'].forEach(id=>document.addEventListener('DOMContentLoaded',()=>$(id).addEventListener('input',()=>mqttTouched=true)));
async function saveMqtt(){try{const r=await api('/api/mqtt',{m:'POST',q:{host:$('mhost').value,port:$('mport').value,user:$('muser').value,pass:$('mpass').value}});$('mqttmsg').textContent=r.message;mqttTouched=false;$('mpass').value='';setTimeout(status,3000)}catch(e){$('mqttmsg').textContent='error: '+e.message}}
async function saveWifi(){const r=await api('/api/wifi',{m:'POST',q:{ssid:$('ssid').value,pass:$('pass').value}});log(r.message)}
status();poll();recs();btns();setInterval(poll,1500);setInterval(status,5000);
</script></body></html>
)HTML";

static const char REMOTE_HTML[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>Remote</title><meta name="theme-color" content="#0f1115">
<style>
:root{--bg:#0f1115;--card:#1b1f27;--line:#2a2f3a;--fg:#e6e8ec;--mute:#8b92a1;--acc:#4f8cff;--ok:#3ddc84;--bad:#ff5c5c}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}body{margin:0;background:var(--bg);color:var(--fg);font:17px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;min-height:100vh}
header{padding:14px 18px;display:flex;justify-content:space-between;align-items:center}h1{font-size:18px;margin:0}
header a{color:var(--mute);font-size:13px;text-decoration:none}
main{max-width:520px;margin:0 auto;padding:8px 14px 40px;display:grid;grid-template-columns:repeat(2,1fr);gap:12px}
button{padding:22px 10px;border:1px solid var(--line);border-radius:14px;background:var(--card);color:var(--fg);font:inherit;font-weight:600;cursor:pointer;min-height:76px;transition:transform .08s,background .15s;user-select:none}
button:active{transform:scale(.96);background:#262c38}button.ok{background:#1d3b2c;border-color:var(--ok)}button.bad{background:#3b1d1d;border-color:var(--bad)}
.empty{grid-column:1/-1;color:var(--mute);text-align:center;padding:40px 10px;font-size:15px}
#toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%);background:#262c38;padding:8px 14px;border-radius:8px;font-size:14px;opacity:0;transition:opacity .2s;pointer-events:none}
</style></head><body>
<header><h1 id="title">Remote</h1><a href="/">settings</a></header>
<main id="grid"><div class="empty">loading…</div></main>
<div id="toast"></div>
<script>
const $=id=>document.getElementById(id);let toastT;
function toast(m,bad){const t=$('toast');t.textContent=m;t.style.opacity=1;t.style.borderLeft=bad?'3px solid #ff5c5c':'3px solid #3ddc84';clearTimeout(toastT);toastT=setTimeout(()=>t.style.opacity=0,1500)}
async function load(){try{const r=await (await fetch('/api/buttons')).json();const g=$('grid');
 if(!r.length){g.innerHTML='<div class="empty">No buttons yet.<br>Add some under <a href="/" style="color:#4f8cff">settings</a>.</div>';return}
 g.innerHTML=r.map((b,i)=>`<button data-i="${i}">${b.label.replace(/[<>&]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;'}[c]))}</button>`).join('');
 g.querySelectorAll('button').forEach(el=>el.addEventListener('click',()=>press(el)))}catch(e){$('grid').innerHTML='<div class="empty">device unreachable</div>'}}
async function press(el){if(el.disabled)return;el.disabled=true;try{const r=await fetch('/api/button?id='+el.dataset.i,{method:'POST'});const j=await r.json();el.classList.add(j.ok?'ok':'bad');toast(j.message,!j.ok)}catch(e){el.classList.add('bad');toast('no response',true)}
 setTimeout(()=>{el.classList.remove('ok','bad');el.disabled=false},350)}
load();
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

// Large responses must go out in small pieces: a single multi-KB lwip_send in
// non-blocking mode fails on this port and the client gets 0 bytes.
static void sendPage(const char *html) {
    size_t len = strlen(html);
    server.setContentLength(len);
    server.send(200, "text/html", "");
    for (size_t off = 0; off < len; off += 1024)
        server.sendContent(html + off, (len - off) < 1024 ? (len - off) : 1024);
}

static void handleRoot() { sendPage(INDEX_HTML); }

static void handleStatus() {
    String j = "{";
    j += "\"firmware\":\"ir_blaster / LibreTiny " + String(lt_get_version()) + "\",";
    j += "\"hostname\":\"ir-blaster\",";
    j += "\"uptime\":" + String(millis() / 1000) + ",";
    j += "\"reboot_reason\":" + jsonStr(lt_get_reboot_reason_name(lt_get_reboot_reason())) + ",";
    j += "\"wifi\":" + String(appWifiConnected() ? "true" : "false") + ",";
    j += "\"ssid\":" + jsonStr(cfg.ssid) + ",";
    j += "\"ip\":\"" + appIp() + "\",";
    j += "\"rssi\":" + String(appWifiConnected() ? WiFi.RSSI() : 0) + ",";
    j += "\"carrier\":" + String(cfg.carrierHz) + ",";
    j += "\"ac\":" + acStateJson() + ",";
    j += "\"heap\":{\"free\":" + String(lt_heap_get_free()) + ",\"min_free\":" + String(lt_heap_get_min_free()) +
         ",\"max_alloc\":" + String(lt_heap_get_max_alloc()) + ",\"size\":" + String(lt_heap_get_size()) + "},";
    j += "\"mqtt\":{\"state\":" + jsonStr(mqttState()) + ",\"host\":" + jsonStr(cfg.mqttHost) + ",\"port\":" + String(cfg.mqttPort) +
         ",\"user\":" + jsonStr(cfg.mqttUser) + ",\"base\":" + jsonStr(mqttBaseTopic()) + "},";
    j += "\"rx_idle\":\"" + String(cfg.rxPin == NO_PIN ? "n/a" : (digitalRead(cfg.rxPin) ? "HIGH" : "LOW (no receiver?)")) + "\",";
    j += "\"pins\":{\"led1\":" + pinJson(cfg.ledPin[0]) + ",\"led2\":" + pinJson(cfg.ledPin[1]) +
         ",\"led3\":" + pinJson(cfg.ledPin[2]) + ",\"rx\":" + pinJson(cfg.rxPin) +
         ",\"status\":" + pinJson(cfg.statusPin) + ",\"status_invert\":" + String(cfg.statusInvert ? "true" : "false") + "},";
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
    appSendNec(a, c, server.arg("loopback") == "1");
    char b[48]; snprintf(b, sizeof(b), "sent NEC addr=0x%02X cmd=0x%02X%s", a & 0xFF, c & 0xFF, server.arg("loopback") == "1" ? " (receiver kept on)" : "");
    ok(b);
}

static void handlePins() {
    uint8_t led[LED_COUNT], rxp;
    const char *names[LED_COUNT] = {"led1", "led2", "led3"};
    for (int i = 0; i < LED_COUNT; i++)
        if (!parsePinArg(server.arg(names[i]), led[i])) return fail(400, String("bad pin for ") + names[i]);
    if (!parsePinArg(server.arg("rx"), rxp)) return fail(400, "bad pin for rx");
    uint8_t sp = cfg.statusPin;
    if (server.hasArg("status") && !parsePinArg(server.arg("status"), sp)) return fail(400, "bad pin for status");
    for (int i = 0; i < LED_COUNT; i++) cfg.ledPin[i] = led[i];
    cfg.rxPin = rxp;
    if (cfg.statusPin != NO_PIN && cfg.statusPin != sp) pinMode(cfg.statusPin, INPUT);
    cfg.statusPin = sp;
    if (server.hasArg("status_invert")) cfg.statusInvert = server.arg("status_invert") == "1";
    cfg.save();
    appApplyPins();
    appApplyStatusLed();
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

static void handleRecordings() {
    RecInfo info[REC_SLOTS];
    int n = recList(info, REC_SLOTS);
    String j = "[";
    for (int i = 0; i < n; i++)
        j += "{\"slot\":" + String(info[i].slot) + ",\"name\":" + jsonStr(info[i].name) +
             ",\"pulses\":" + String(info[i].pulses) + ",\"ms\":" + String(info[i].durationMs) + "}" + (i + 1 < n ? "," : "");
    server.send(200, "application/json", j + "]");
}

static void handleRecord() {
    uint32_t sec = server.arg("sec").toInt(); if (sec == 0 || sec > 60) sec = 15;
    String msg;
    int n = appRecord(server.arg("name").c_str(), sec, msg);
    if (n > 0) ok(msg); else fail(n == 0 ? 408 : 400, msg);
}

static void handleRecordLast() {
    String msg;
    if (appSaveLast(server.arg("name").c_str(), msg)) ok(msg); else fail(400, msg);
}

// Store a recording from raw timings posted by a PC tool (body: "9000,4500,562,...").
static void handleRecordingPut() {
    String name = server.arg("name"); name.trim();
    String body = server.arg("plain");
    if (!name.length()) return fail(400, "name required");
    uint32_t *buf = appScratch();
    int n = 0; const char *p = body.c_str();
    while (*p && n < REC_MAX_PULSES) {
        while (*p == ',' || *p == ' ' || *p == '[' || *p == ']' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        char *end; long v = strtol(p, &end, 10);
        if (end == p) return fail(400, "bad timings: expected comma-separated microseconds");
        if (v > 0) buf[n++] = (uint32_t)v;
        p = end;
    }
    if (n < 3) return fail(400, "need at least 3 timings");
    n = irCleanGlitches(buf, n);
    if (!recSave(name.c_str(), buf, n)) return fail(400, "save failed (bad name or 32 slots full)");
    ok("saved '" + name + "' (" + String(n) + " pulses)");
}

static void handlePlay() {
    String msg;
    if (appPlay(server.arg("name").c_str(), msg)) ok(msg); else fail(400, msg);
}

// ---- export (chunked, so 32 recordings don't need to fit in one String)
static void sendTimings(const uint32_t *t, int n) {
    String chunk = "[";
    for (int i = 0; i < n; i++) {
        chunk += String(t[i]); if (i + 1 < n) chunk += ",";
        if (chunk.length() > 1024) { server.sendContent(chunk); chunk = ""; }
    }
    server.sendContent(chunk + "]");
}


static void handleExport() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.sendHeader("Content-Disposition", "attachment; filename=\"ir_export.json\"");
    server.send(200, "application/json", "");
    server.sendContent("{\"device\":\"ir-blaster\",\"carrier_hz\":" + String(IR_CARRIER_HZ) +
                       ",\"unit\":\"us\",\"format\":\"mark,space,mark,...\",\"uptime_s\":" + String(millis() / 1000) +
                       ",\"last_capture\":{\"pulses\":" + String(appLastPulses()) + ",\"timings\":");
    sendTimings(appLastCapture(), appLastPulses());
    server.sendContent("},\"recordings\":[");
    RecInfo info[REC_SLOTS];
    int n = recList(info, REC_SLOTS);
    for (int i = 0; i < n; i++) {
        uint32_t *buf = appScratch();
        int k = recLoad(info[i].slot, buf, REC_MAX_PULSES);
        server.sendContent("{\"name\":" + jsonStr(info[i].name) + ",\"pulses\":" + String(k) + ",\"timings\":");
        sendTimings(buf, k);
        server.sendContent(i + 1 < n ? "}," : "}");
    }
    server.sendContent("]}");
    server.sendContent("");    // end of chunked body
}

static void handleLast() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    server.sendContent("{\"pulses\":" + String(appLastPulses()) + ",\"carrier_hz\":" + String(IR_CARRIER_HZ) + ",\"timings\":");
    sendTimings(appLastCapture(), appLastPulses());
    server.sendContent("}");
    server.sendContent("");
}

// ---- OTA: POST a .uf2 as multipart "file" to /update
static bool otaOk = false;
static uint32_t otaLastActivity = 0;

// A broken upload may never reach END/ABORTED; let MQTT come back after a while.
void webOtaWatchdog() {
    if (otaLastActivity && millis() - otaLastActivity > 30000) { otaLastActivity = 0; if (Update.isRunning()) Update.abort(); mqttResume(); }
}

static void handleUpdateDone() {
    if (otaOk && !Update.hasError()) {
        ok("update written, rebooting");
        delay(300);
        lt_reboot();
    } else {
        fail(500, String("update failed: ") + Update.errorString());
        mqttResume();
    }
}

static void handleUpdateUpload() {
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("ota: receiving '%s'\n", up.filename.c_str());
        mqttSuspend();                       // frees the MQTT socket + buffer: heap is tight (~16 KB free)
        if (Update.isRunning()) Update.abort();   // a previous broken upload must not leak its buffers
        otaOk = Update.begin(UPDATE_SIZE_UNKNOWN);
        if (!otaOk) Serial.printf("ota: begin failed: %s\n", Update.errorString());
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (otaOk && Update.write(up.buf, up.currentSize) != up.currentSize) {
            otaOk = false;
            Serial.printf("ota: write failed: %s\n", Update.errorString());
        }
        appStatusBlink(50);
    } else if (up.status == UPLOAD_FILE_END) {
        if (otaOk && Update.end(true)) Serial.printf("ota: done, %u bytes\n", (unsigned)up.totalSize);
        else { otaOk = false; Serial.printf("ota: end failed: %s\n", Update.errorString()); }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        otaOk = false;
        Serial.println("ota: aborted");
        Update.abort();
        mqttResume();
    }
    otaLastActivity = millis();
}

// ---- remote buttons
static void handleButtons() {
    String j = "[";
    for (int i = 0; i < btnCount(); i++) {
        const Button *b = btnGet(i);
        j += "{\"label\":" + jsonStr(b->label) + ",\"type\":\"" + (b->type == BTN_NEC ? "nec" : "rec") + "\"" +
             ",\"addr\":" + String(b->addr) + ",\"cmd\":" + String(b->cmd) + ",\"rec\":" + jsonStr(b->rec) +
             ",\"repeat\":" + String(b->repeat) + "}" + (i + 1 < btnCount() ? "," : "");
    }
    server.send(200, "application/json", j + "]");
}

static void handleButtonSet() {
    Button b = {};
    String label = server.arg("label"); label.trim();
    if (!label.length()) return fail(400, "label required");
    strncpy(b.label, label.c_str(), BTN_LABEL_LEN);
    b.repeat = server.arg("repeat").toInt(); if (b.repeat < 1) b.repeat = 1;
    if (server.arg("type") == "rec") {
        b.type = BTN_REC;
        if (recFind(server.arg("rec").c_str()) < 0) return fail(400, "no such recording");
        strncpy(b.rec, server.arg("rec").c_str(), BTN_REC_LEN);
    } else {
        unsigned a, c;
        if (sscanf(server.arg("addr").c_str(), "%i", &a) != 1 || sscanf(server.arg("cmd").c_str(), "%i", &c) != 1)
            return fail(400, "addr and cmd required, e.g. 0xBA 0x1E");
        b.type = BTN_NEC; b.addr = a & 0xFF; b.cmd = c & 0xFF;
    }
    int id = server.hasArg("id") ? server.arg("id").toInt() : btnCount();
    if (!btnSet(id, b)) return fail(400, "could not save (max 24 buttons)");
    mqttPublishDiscovery();
    ok("button '" + label + "' saved");
}

static void handleButtonPress() {
    String msg;
    if (appPressButton(server.arg("id").toInt(), msg)) ok(msg); else fail(400, msg);
}

// ---- live stream
static void handleStream() {
    uint32_t after = strtoul(server.arg("after").c_str(), nullptr, 10);
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    server.sendContent("{\"last\":" + String(appStreamLastId()) + ",\"now\":" + String(millis()) + ",\"captures\":[");
    bool first = true;
    for (int i = 0; i < STREAM_CAPS; i++) {
        const StreamCap *c = appStreamGet(i);
        if (!c || c->id <= after) continue;
        String j = String(first ? "" : ",") + "{\"id\":" + String(c->id) + ",\"t\":" + String(c->t) +
                   ",\"type\":\"" + (c->type == 1 ? "nec" : c->type == 2 ? "repeat" : "raw") + "\"";
        if (c->type == 1) j += ",\"addr\":" + String(c->address) + ",\"cmd\":" + String(c->command) + ",\"raw\":" + String(c->raw);
        j += ",\"pulses\":" + String(c->n) + ",\"timings\":[";
        server.sendContent(j); j = "";
        for (int k = 0; k < c->n; k++) {
            j += String(c->v[k]); if (k + 1 < c->n) j += ",";
            if (j.length() > 1024) { server.sendContent(j); j = ""; }
        }
        server.sendContent(j + "]}");
        first = false;
    }
    server.sendContent("]}");
    server.sendContent("");
}

static void handleStreamSave() {
    String msg;
    if (appStreamSave(strtoul(server.arg("id").c_str(), nullptr, 10), server.arg("name").c_str(), msg)) ok(msg); else fail(400, msg);
}

static void handleMqtt() {
    cfg.mqttHost = server.arg("host"); cfg.mqttHost.trim();
    cfg.mqttPort = server.arg("port").toInt(); if (!cfg.mqttPort) cfg.mqttPort = 1883;
    cfg.mqttUser = server.arg("user"); cfg.mqttUser.trim();
    if (server.hasArg("pass") && server.arg("pass").length()) cfg.mqttPass = server.arg("pass");
    if (!cfg.mqttHost.length()) { cfg.mqttUser = ""; cfg.mqttPass = ""; }
    cfg.save();
    mqttReconfigure();
    ok(cfg.mqttHost.length() ? "mqtt settings saved, connecting" : "mqtt disabled");
}

// ---- air conditioner (native protocol)
static void handleAc() {
    if (server.method() == HTTP_GET) { server.send(200, "application/json", acStateJson()); return; }
    AcState st = acState(); uint8_t key = AC_KEY_TEMP_UP; uint8_t v;
    if (server.hasArg("power")) { String pw = server.arg("power"); st.power = (pw == "on" || pw == "1" || pw == "true"); key = AC_KEY_POWER; }
    if (server.hasArg("mode")) { if (server.arg("mode") == "off") st.power = false; else if (acParseMode(server.arg("mode"), v)) { st.mode = v; key = AC_KEY_MODE; } else return fail(400, "mode: auto|cool|dry|fan|heat|off"); }
    if (server.hasArg("temp")) { long n = server.arg("temp").toInt(); if (n < 16 || n > 31) return fail(400, "temp: 16..31"); st.temp = n; }
    if (server.hasArg("fan")) { if (acParseFan(server.arg("fan"), v)) { st.fan = v; key = AC_KEY_FAN; } else return fail(400, "fan: auto|low|med|high"); }
    String msg;
    if (acApply(st, key, msg, server.arg("loopback") == "1")) server.send(200, "application/json", "{\"ok\":true,\"message\":" + jsonStr(msg) + ",\"state\":" + acStateJson() + "}");
    else fail(400, msg);
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
    server.on("/api/mqtt", HTTP_POST, handleMqtt);
    server.on("/api/ac", HTTP_GET, handleAc);
    server.on("/api/ac", HTTP_POST, handleAc);
    server.on("/api/reboot", HTTP_POST, handleReboot);
    server.on("/api/export", HTTP_GET, handleExport);
    server.on("/api/last", HTTP_GET, handleLast);
    server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
    server.on("/remote", HTTP_GET, []() { sendPage(REMOTE_HTML); });
    server.on("/api/buttons", HTTP_GET, handleButtons);
    server.on("/api/buttons/set", HTTP_POST, handleButtonSet);
    server.on("/api/buttons/delete", HTTP_POST, []() { if (btnDelete(server.arg("id").toInt())) { mqttPublishDiscovery(); ok("deleted"); } else fail(404, "no such button"); });
    server.on("/api/buttons/move", HTTP_POST, []() { if (btnMove(server.arg("id").toInt(), server.arg("dir").toInt() < 0 ? -1 : 1)) { mqttPublishDiscovery(); ok("moved"); } else fail(400, "cannot move"); });
    server.on("/api/button", HTTP_POST, handleButtonPress);
    server.on("/api/stream", HTTP_GET, handleStream);
    server.on("/api/stream/save", HTTP_POST, handleStreamSave);
    server.on("/api/stream/clear", HTTP_POST, []() { appStreamClear(); ok("stream history cleared"); });
    server.on("/api/recordings", HTTP_GET, handleRecordings);
    server.on("/api/record", HTTP_POST, handleRecord);
    server.on("/api/record/last", HTTP_POST, handleRecordLast);
    server.on("/api/play", HTTP_POST, handlePlay);
    server.on("/api/recordings/put", HTTP_POST, handleRecordingPut);
    server.on("/api/recordings/delete", HTTP_POST, []() { if (recErase(server.arg("name").c_str())) ok("deleted"); else fail(404, "not found"); });
    server.on("/api/sled/test", HTTP_POST, []() { for (int i = 0; i < 5; i++) { appStatusBlink(0); digitalWrite(cfg.statusPin, cfg.statusInvert ? LOW : HIGH); delay(200); digitalWrite(cfg.statusPin, cfg.statusInvert ? HIGH : LOW); delay(200); } ok("blinked status LED"); });
    server.onNotFound([]() { fail(404, "not found"); });
    server.begin();
    if (mdns.begin("ir-blaster")) mdns.addService((char *)"http", (char *)"tcp", 80);
    started = true;
    Serial.printf("web: http://%s/  (http://ir-blaster.local/)\n", appIp().c_str());
}

void webLoop() {
    if (started) server.handleClient();
    webOtaWatchdog();
}
