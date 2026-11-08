'use strict';
const $ = id => document.getElementById(id);
const MODES = ['auto', 'cool', 'dry', 'fan', 'heat'];
const FANS = ['auto', 'low', 'med', 'high'];
const FAN_LEVEL = { auto: 'auto', low: 'l1', med: 'l2', medium: 'l2', high: 'l3' };
const ICONS = {
  auto: '<svg viewBox="0 0 24 24"><path fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" d="M12 3l7 18M12 3L5 21M7.5 15h9"/></svg>',
  cool: '<svg viewBox="0 0 24 24"><path fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" d="M12 2v20M4 7l16 10M4 17L20 7M12 2l-3 3M12 2l3 3M12 22l-3-3M12 22l3-3"/></svg>',
  dry: '<svg viewBox="0 0 24 24"><path fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round" d="M12 3s6 7 6 11.5a6 6 0 0 1-12 0C6 10 12 3 12 3z"/></svg>',
  fan: '<svg viewBox="0 0 24 24"><g fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><circle cx="12" cy="12" r="1.8"/><path d="M12 10.2c0-4 1.5-6.2 4-6.2 1.8 0 2.4 1.7 1.2 3.4C16 9.2 13.6 10.2 12 10.2zM10.4 13c-3.5 2-6.2 1.8-7.4-.3-.9-1.6.4-2.8 2.4-2.6 2.3.2 4 1.5 5 2.9zM13.6 13c3.5 2 4.6 4.5 3.4 6.6-.9 1.6-2.6 1.1-3.6-.6-1.1-2-1.1-4.2.2-6z"/></g></svg>',
  heat: '<svg viewBox="0 0 24 24"><g fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><circle cx="12" cy="12" r="4"/><path d="M12 2v2.5M12 19.5V22M2 12h2.5M19.5 12H22M4.9 4.9l1.8 1.8M17.3 17.3l1.8 1.8M4.9 19.1l1.8-1.8M17.3 6.7l1.8-1.8"/></g></svg>',
};

let ac = { power: 'off', mode: 'cool', temp: 24, fan: 'auto' };
let pending = null;          // debounce timer for temperature taps
let busy = false;
let scenes = [];
let lastRxSeq = -1;
let deviceAddr = '';

// ---------------------------------------------------------------- utils
async function api(path, { method = 'GET', q = {}, body = null, headers = {} } = {}) {
  const u = new URL(path, location.href);
  for (const k in q) if (q[k] !== undefined && q[k] !== null) u.searchParams.set(k, q[k]);
  const r = await fetch(u, { method, body, headers });
  const t = await r.text();
  let j; try { j = JSON.parse(t); } catch { j = t; }
  if (!r.ok) throw new Error((j && j.message) || t || ('HTTP ' + r.status));
  return j;
}
let toastTimer;
function toast(msg, bad = false) {
  const t = $('toast'); t.textContent = msg; t.className = 'toast' + (bad ? ' bad' : ''); t.hidden = false;
  clearTimeout(toastTimer); toastTimer = setTimeout(() => { t.hidden = true; }, bad ? 4000 : 1800);
}
const hex = (v, w) => '0x' + Number(v).toString(16).toUpperCase().padStart(w, '0');
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const fanName = f => f === 'med' || f === 'medium' ? 'medium' : f;
function setConn(ok, text) { $('dot').className = 'dot' + (ok ? ' on' : ''); $('connText').textContent = text; }

// ----------------------------------------------------------------- tabs
function showTab(name) {
  document.querySelectorAll('.tabs a').forEach(a => a.classList.toggle('on', a.dataset.tab === name));
  document.querySelectorAll('.panel').forEach(p => p.classList.toggle('on', p.id === name));
  try { localStorage.setItem('irweb.tab', name); } catch {}
}
document.querySelectorAll('.tabs a').forEach(a => a.addEventListener('click', e => { e.preventDefault(); showTab(a.dataset.tab); }));
(() => { let t = 'ac'; try { t = localStorage.getItem('irweb.tab') || (location.hash || '#ac').slice(1); } catch {} if (!$(t)) t = 'ac'; showTab(t); })();

// ------------------------------------------------------------------- AC
function renderAc() {
  const on = ac.power === 'on';
  const d = $('acdisplay'); d.className = 'acdisplay ' + (on ? ac.mode : 'off');
  $('acTemp').textContent = ac.temp;
  $('acModeIcon').innerHTML = ICONS[ac.mode] || '';
  $('acModeText').textContent = on ? ac.mode : 'standby';
  $('acFanBars').className = 'fan ' + (FAN_LEVEL[ac.fan] || 'auto');
  $('acFanText').textContent = on ? fanName(ac.fan) : '';
  $('acPowerText').textContent = on ? 'on' : 'off';
  $('acPower').classList.toggle('on', on);
  document.querySelectorAll('#modeSeg button').forEach(b => b.classList.toggle('on', b.dataset.mode === ac.mode));
  document.querySelectorAll('#fanSeg button').forEach(b => b.classList.toggle('on', b.dataset.fan === (ac.fan === 'medium' ? 'med' : ac.fan)));
  document.querySelectorAll('#tempChips button').forEach(b => b.classList.toggle('on', Number(b.dataset.t) === Number(ac.temp)));
  $('tempDown').disabled = ac.temp <= 16; $('tempUp').disabled = ac.temp >= 31;
}
async function sendAc(q, note) {
  busy = true; $('acMsg').textContent = 'sending…';
  try {
    const r = await api('/api/ac', { method: 'POST', q });
    if (r.state) ac = normalizeAc(r.state);
    $('acMsg').textContent = note || r.message || 'sent';
  } catch (e) {
    $('acMsg').textContent = ''; toast('AC: ' + e.message, true);
    await loadAc();
  } finally { busy = false; renderAc(); }
}
function normalizeAc(s) {
  return { power: s.power, mode: s.mode, temp: Number(s.temp), fan: s.fan === 'medium' ? 'med' : s.fan };
}
async function loadAc() { try { ac = normalizeAc(await api('/api/ac')); renderAc(); } catch {} }
function setTemp(t) {
  t = Math.max(16, Math.min(31, t)); ac.temp = t; ac.power = 'on'; renderAc();
  clearTimeout(pending);                       // several quick taps → one frame
  pending = setTimeout(() => sendAc({ power: 'on', temp: t }, `set ${t}°C`), 450);
}
$('tempDown').onclick = () => setTemp(ac.temp - 1);
$('tempUp').onclick = () => setTemp(ac.temp + 1);
$('acPower').onclick = () => { const on = ac.power !== 'on'; ac.power = on ? 'on' : 'off'; renderAc(); sendAc({ power: on ? 'on' : 'off' }, on ? 'turned on' : 'turned off'); };
document.querySelectorAll('#modeSeg button').forEach(b => {
  b.querySelector('.ico').innerHTML = ICONS[b.dataset.mode];
  b.onclick = () => { ac.mode = b.dataset.mode; ac.power = 'on'; renderAc(); sendAc({ power: 'on', mode: ac.mode }, ac.mode + ' mode'); };
});
document.querySelectorAll('#fanSeg button').forEach(b => {
  b.onclick = () => { ac.fan = b.dataset.fan; ac.power = 'on'; renderAc(); sendAc({ power: 'on', fan: ac.fan }, 'fan ' + fanName(ac.fan)); };
});
(() => { const c = $('tempChips'); for (let t = 18; t <= 28; t += 2) { const b = document.createElement('button'); b.textContent = t + '°'; b.dataset.t = t; b.onclick = () => setTemp(t); c.appendChild(b); } })();

// --------------------------------------------------------------- scenes
function sceneSummary(s) {
  if (s.power === 'off') return 'power off';
  return [s.mode, s.temp !== undefined ? s.temp + '°C' : null, s.fan ? 'fan ' + fanName(s.fan) : null].filter(Boolean).join(' · ');
}
function renderScenes() {
  const g = $('sceneGrid'); g.innerHTML = '';
  if (!scenes.length) { g.innerHTML = '<div class="empty">no scenes yet</div>'; return; }
  scenes.forEach((s, i) => {
    const t = document.createElement('button'); t.className = 'tile';
    const active = ac.power === (s.state.power || 'on') && (s.state.power === 'off' || (s.state.mode === ac.mode && Number(s.state.temp) === Number(ac.temp) && (!s.state.fan || (s.state.fan === 'medium' ? 'med' : s.state.fan) === ac.fan)));
    if (active) t.classList.add('on');
    t.innerHTML = `<div class="ic">${esc(s.icon || '🎛️')}</div><div class="t">${esc(s.name)}</div><div class="s">${esc(sceneSummary(s.state))}</div>`;
    t.onclick = () => applyScene(s, t);
    let press;
    t.oncontextmenu = e => { e.preventDefault(); deleteScene(i); };
    t.ontouchstart = () => { press = setTimeout(() => { press = null; deleteScene(i); }, 650); };
    t.ontouchend = t.ontouchmove = t.ontouchcancel = () => clearTimeout(press);
    g.appendChild(t);
  });
}
async function applyScene(s, tile) {
  tile.classList.add('busy');
  const q = { ...s.state }; if (q.fan === 'medium') q.fan = 'med';
  if (q.power !== 'off') q.power = 'on';
  if (q.power === 'off') { delete q.mode; delete q.temp; delete q.fan; }
  ac = { ...ac, ...q, temp: q.temp !== undefined ? Number(q.temp) : ac.temp }; renderAc();
  await sendAc(q, 'scene: ' + s.name);
  tile.classList.remove('busy'); renderScenes(); toast(s.name);
}
async function saveScene() {
  const name = $('sceneName').value.trim(); if (!name) { toast('give the scene a name', true); $('sceneName').focus(); return; }
  const icon = $('sceneIcon').value.trim() || (ac.power === 'on' ? { cool: '❄️', heat: '🔥', dry: '💧', fan: '🌀', auto: '✨' }[ac.mode] : '⏻');
  const state = ac.power === 'on' ? { power: 'on', mode: ac.mode, temp: ac.temp, fan: ac.fan } : { power: 'off' };
  const i = scenes.findIndex(s => s.name.toLowerCase() === name.toLowerCase());
  if (i >= 0) scenes[i] = { name, icon, state }; else scenes.push({ name, icon, state });
  await persistScenes(); $('sceneName').value = ''; $('sceneIcon').value = ''; toast('saved ' + name);
}
async function deleteScene(i) {
  if (!confirm(`Delete scene "${scenes[i].name}"?`)) return;
  scenes.splice(i, 1); await persistScenes();
}
async function persistScenes() {
  try { await api('/local/scenes', { method: 'POST', body: JSON.stringify(scenes), headers: { 'Content-Type': 'application/json' } }); }
  catch (e) { toast('could not save scenes: ' + e.message, true); }
  renderScenes();
}
async function loadScenes() { try { scenes = await api('/local/scenes'); } catch { scenes = []; } renderScenes(); }

// --------------------------------------------------------------- remote
async function loadButtons() {
  const g = $('btnGrid');
  try {
    const list = await api('/api/buttons'); g.innerHTML = '';
    if (!list.length) { g.innerHTML = '<div class="empty">no buttons yet — add them on the device admin page</div>'; return; }
    list.forEach((b, i) => {
      const t = document.createElement('button'); t.className = 'tile';
      const sub = b.type === 'nec' ? `NEC ${hex(b.addr, 2)} / ${hex(b.cmd, 2)}` : `recording "${b.rec}"`;
      t.innerHTML = `<div class="t">${esc(b.label)}</div><div class="s">${esc(sub)}${b.repeat > 1 ? ' ×' + b.repeat : ''}</div>`;
      t.onclick = async () => { t.classList.add('busy'); try { await api('/api/button', { method: 'POST', q: { id: i } }); toast(b.label); } catch (e) { toast(e.message, true); } t.classList.remove('busy'); };
      g.appendChild(t);
    });
  } catch (e) { g.innerHTML = `<div class="empty">${esc(e.message)}</div>`; }
}
async function loadRecordings() {
  const l = $('recList');
  try {
    const list = await api('/api/recordings'); l.innerHTML = '';
    if (!list.length) { l.innerHTML = '<div class="empty">no recordings on the device</div>'; return; }
    list.forEach(r => {
      const it = document.createElement('div'); it.className = 'item';
      it.innerHTML = `<div class="grow"><div class="t">${esc(r.name)}</div><div class="s">${r.pulses} pulses · ${r.ms} ms</div></div>`;
      const b = document.createElement('button'); b.textContent = 'Play';
      b.onclick = async () => { b.disabled = true; try { await api('/api/play', { method: 'POST', q: { name: r.name } }); toast('played ' + r.name); } catch (e) { toast(e.message, true); } b.disabled = false; };
      it.appendChild(b); l.appendChild(it);
    });
  } catch (e) { l.innerHTML = `<div class="empty">${esc(e.message)}</div>`; }
}
async function sendNec() {
  const addr = $('necAddr').value.trim(), cmd = $('necCmd').value.trim();
  if (!addr || !cmd) { toast('enter address and command', true); return; }
  try { const r = await api('/api/send', { method: 'POST', q: { addr, cmd } }); $('remoteMsg').textContent = r.message || 'sent'; }
  catch (e) { toast(e.message, true); }
}

// ------------------------------------------------------------- activity
async function pollReceived() {
  try {
    const r = await api('/api/received');
    if (r.seq === lastRxSeq) return; lastRxSeq = r.seq;
    const l = $('rxList'); l.innerHTML = '';
    if (!r.frames.length) { l.innerHTML = '<div class="empty">nothing received yet</div>'; return; }
    r.frames.slice().reverse().slice(0, 15).forEach(f => {
      const age = Math.max(0, Math.round((r.now - f.t) / 1000));
      const it = document.createElement('div'); it.className = 'item';
      const what = f.type === 'nec' ? `NEC ${hex(f.addr, 2)} / ${hex(f.cmd, 2)} <span class="s">${hex(f.raw, 8)}</span>` : f.type === 'repeat' ? 'NEC repeat' : `raw frame · ${f.pulses} pulses`;
      it.innerHTML = `<div class="grow"><div class="t">${what}</div><div class="s">${fmtAge(age)}</div></div>`;
      if (f.type === 'nec') { const b = document.createElement('button'); b.textContent = 'Send'; b.onclick = () => api('/api/send', { method: 'POST', q: { raw: hex(f.raw, 8) } }).then(() => toast('sent')).catch(e => toast(e.message, true)); it.appendChild(b); }
      l.appendChild(it);
    });
  } catch {}
}
function fmtAge(s) { return s < 60 ? s + ' s ago' : s < 3600 ? Math.round(s / 60) + ' min ago' : s < 172800 ? Math.round(s / 3600) + ' h ago' : Math.round(s / 86400) + ' d ago'; }
function fmtUptime(s) { const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60); return (d ? d + 'd ' : '') + (h ? h + 'h ' : '') + m + 'm'; }
async function pollStatus() {
  try {
    const s = await api('/api/status');
    setConn(true, (s.ip || deviceAddr) + ' · ' + s.rssi + ' dBm');
    if (!busy && !pending && s.ac) { const n = normalizeAc(s.ac); if (JSON.stringify(n) !== JSON.stringify(ac)) { ac = n; renderAc(); renderScenes(); } }
    $('statusTable').innerHTML = Object.entries({
      firmware: s.firmware, uptime: fmtUptime(s.uptime), 'last reboot': s.reboot_reason, wifi: `${s.ssid} · ${s.rssi} dBm`, ip: s.ip,
      'free heap': `${Math.round(s.heap.free / 1024)} KB (min ${Math.round(s.heap.min_free / 1024)} KB)`,
      mqtt: s.mqtt.state + (s.mqtt.host ? ` · ${s.mqtt.host}:${s.mqtt.port}` : ''), 'IR carrier': s.carrier + ' Hz', receiver: s.rx_idle,
    }).map(([k, v]) => `<tr><th>${esc(k)}</th><td>${esc(v)}</td></tr>`).join('');
  } catch (e) { setConn(false, 'device offline'); }
}

// ------------------------------------------------------------- settings
function openSettings() { $('devAddr').value = deviceAddr; $('settings').hidden = false; $('devAddr').focus(); }
function closeSettings() { $('settings').hidden = true; }
async function saveSettings() {
  try { const r = await api('/local/settings', { method: 'POST', body: JSON.stringify({ device: $('devAddr').value }), headers: { 'Content-Type': 'application/json' } }); deviceAddr = r.device; $('adminLink').href = 'http://' + deviceAddr + '/'; closeSettings(); toast('device set to ' + deviceAddr); refreshAll(); }
  catch (e) { toast(e.message, true); }
}
$('settings').addEventListener('click', e => { if (e.target === $('settings')) closeSettings(); });
async function loadSettings() { try { const s = await api('/local/settings'); deviceAddr = s.device; $('adminLink').href = 'http://' + deviceAddr + '/'; } catch {} }

// ----------------------------------------------------------------- boot
async function refreshAll() { await Promise.all([loadAc(), loadButtons(), loadRecordings(), pollStatus(), pollReceived()]); renderScenes(); }
(async () => {
  renderAc();
  await loadSettings(); await loadScenes(); await refreshAll();
  setInterval(pollStatus, 5000); setInterval(pollReceived, 2500);
  document.addEventListener('visibilitychange', () => { if (!document.hidden) refreshAll(); });
})();
