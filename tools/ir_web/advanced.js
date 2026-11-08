'use strict';
// Advanced page: device status, recordings, NEC send, received log, server settings.
let lastRxSeq = -1;

function renderStatus(s) {
  if (!s) { $('statusTable').innerHTML = `<tr><th>device</th><td style="color:var(--bad)">offline (${esc(deviceAddr)})</td></tr>`; return; }
  $('statusTable').innerHTML = Object.entries({
    firmware: s.firmware, uptime: fmtUptime(s.uptime), 'last reboot': s.reboot_reason, wifi: `${s.ssid} · ${s.rssi} dBm`, ip: s.ip,
    'free heap': `${Math.round(s.heap.free / 1024)} KB (min ${Math.round(s.heap.min_free / 1024)} KB of ${Math.round(s.heap.size / 1024)} KB)`,
    mqtt: s.mqtt.state + (s.mqtt.host ? ` · ${s.mqtt.host}:${s.mqtt.port}` : ''), 'IR carrier': s.carrier + ' Hz', receiver: s.rx_idle,
    pins: `rx ${s.pins.rx ?? '—'} · led ${[s.pins.led1, s.pins.led2, s.pins.led3].filter(p => p != null).map(p => 'P' + p).join(', ') || '—'}`,
    'AC state': describeAc(s.ac),
  }).map(([k, v]) => `<tr><th>${esc(k)}</th><td>${esc(v)}</td></tr>`).join('');
}
async function reboot() {
  if (!confirm('Reboot the blaster now?')) return;
  try { await api('/api/reboot', { method: 'POST' }); toast('rebooting… back in ~10 s'); } catch (e) { toast(e.message, true); }
}

async function loadRecordings() {
  const l = $('recList');
  try {
    const list = await api('/api/recordings'); l.innerHTML = '';
    if (!list.length) { l.innerHTML = '<div class="empty">no recordings on the device</div>'; return; }
    list.forEach(r => {
      const it = document.createElement('div'); it.className = 'item';
      it.innerHTML = `<div class="grow"><div class="t">${esc(r.name)}</div><div class="s">${r.pulses} pulses · ${r.ms} ms · slot ${r.slot}</div></div>`;
      const b = document.createElement('button'); b.textContent = 'Play';
      b.onclick = async () => { b.disabled = true; try { await api('/api/play', { method: 'POST', q: { name: r.name } }); toast('played ' + r.name); } catch (e) { toast(e.message, true); } b.disabled = false; };
      const x = document.createElement('button'); x.className = 'x'; x.title = 'delete'; x.textContent = '×';
      x.onclick = async () => { if (!confirm(`Delete recording "${r.name}" from the blaster?`)) return; try { await api('/api/recordings/delete', { method: 'POST', q: { name: r.name } }); toast('deleted'); loadRecordings(); } catch (e) { toast(e.message, true); } };
      it.appendChild(b); it.appendChild(x); l.appendChild(it);
    });
  } catch (e) { l.innerHTML = `<div class="empty">${esc(e.message)}</div>`; }
}
async function recordNew() {
  const name = $('recName').value.trim(); if (!name) { toast('name the recording first', true); $('recName').focus(); return; }
  const b = $('recBtn'); b.disabled = true; b.textContent = 'waiting for a key press…';
  try { const r = await api('/api/record', { method: 'POST', q: { name, sec: 15 } }); toast(r.message || 'recorded'); $('recName').value = ''; loadRecordings(); }
  catch (e) { toast(e.message, true); }
  finally { b.disabled = false; b.textContent = 'Record (15 s)'; }
}
async function sendNec() {
  const addr = $('necAddr').value.trim(), cmd = $('necCmd').value.trim();
  if (!addr || !cmd) { toast('enter address and command', true); return; }
  try { const r = await api('/api/send', { method: 'POST', q: { addr, cmd } }); $('remoteMsg').textContent = r.message || 'sent'; }
  catch (e) { toast(e.message, true); }
}

async function pollReceived() {
  try {
    const r = await api('/api/received');
    if (r.seq === lastRxSeq) return; lastRxSeq = r.seq;
    const l = $('rxList'); l.innerHTML = '';
    if (!r.frames.length) { l.innerHTML = '<div class="empty">nothing received yet</div>'; return; }
    r.frames.slice().reverse().slice(0, 12).forEach(f => {
      const age = Math.max(0, (r.now - f.t) / 1000);
      const it = document.createElement('div'); it.className = 'item';
      const what = f.type === 'nec' ? `NEC ${hex(f.addr, 2)} / ${hex(f.cmd, 2)} <span class="s">${hex(f.raw, 8)}</span>` : f.type === 'repeat' ? 'NEC repeat' : `raw frame · ${f.pulses} pulses`;
      it.innerHTML = `<div class="grow"><div class="t">${what}</div><div class="s">${fmtAge(age)}</div></div>`;
      if (f.type === 'nec') { const b = document.createElement('button'); b.textContent = 'Send'; b.onclick = () => api('/api/send', { method: 'POST', q: { raw: hex(f.raw, 8) } }).then(() => toast('sent')).catch(e => toast(e.message, true)); it.appendChild(b); }
      if (f.type === 'raw' && f === r.frames[r.frames.length - 1]) { const b = document.createElement('button'); b.textContent = 'Save…'; b.onclick = async () => { const n = prompt('Name for this recording (max 15 chars):'); if (!n) return; try { const x = await api('/api/record/last', { method: 'POST', q: { name: n.trim().slice(0, 15) } }); toast(x.message || 'saved'); loadRecordings(); } catch (e) { toast(e.message, true); } }; it.appendChild(b); }
      l.appendChild(it);
    });
  } catch {}
}

async function saveSettings() {
  try { const r = await api('/local/settings', { method: 'POST', json: { device: $('devAddr').value } }); toast('device set to ' + r.device); await loadSettings(); fillServerInfo(); }
  catch (e) { toast(e.message, true); }
}
let settings = {};
function fillServerInfo() {
  $('devAddr').value = deviceAddr;
  $('serverInfo').textContent = `Requests are forwarded to http://${deviceAddr}/. Server time ${settings.server_time || ''} ${settings.tz || ''}; history is kept for ${settings.history_days || '?'} days.`;
}

(async () => {
  settings = await loadSettings(); fillServerInfo();
  startStatusPolling(renderStatus);
  loadRecordings(); pollReceived(); setInterval(pollReceived, 2500);
})();
