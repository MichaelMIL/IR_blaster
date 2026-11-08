'use strict';
// Shared helpers for every page of the IR web remote.
const $ = id => document.getElementById(id);
const MODES = ['auto', 'cool', 'dry', 'fan', 'heat'];
const FANS = ['auto', 'low', 'med', 'high'];
const FAN_LEVEL = { auto: 'auto', low: 'l1', med: 'l2', medium: 'l2', high: 'l3' };
const MODE_EMOJI = { cool: '❄️', heat: '🔥', dry: '💧', fan: '🌀', auto: '✨' };
const ICONS = {
  auto: '<svg viewBox="0 0 24 24"><path fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" d="M12 3l7 18M12 3L5 21M7.5 15h9"/></svg>',
  cool: '<svg viewBox="0 0 24 24"><path fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" d="M12 2v20M4 7l16 10M4 17L20 7M12 2l-3 3M12 2l3 3M12 22l-3-3M12 22l3-3"/></svg>',
  dry: '<svg viewBox="0 0 24 24"><path fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round" d="M12 3s6 7 6 11.5a6 6 0 0 1-12 0C6 10 12 3 12 3z"/></svg>',
  fan: '<svg viewBox="0 0 24 24"><g fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><circle cx="12" cy="12" r="1.8"/><path d="M12 10.2c0-4 1.5-6.2 4-6.2 1.8 0 2.4 1.7 1.2 3.4C16 9.2 13.6 10.2 12 10.2zM10.4 13c-3.5 2-6.2 1.8-7.4-.3-.9-1.6.4-2.8 2.4-2.6 2.3.2 4 1.5 5 2.9zM13.6 13c3.5 2 4.6 4.5 3.4 6.6-.9 1.6-2.6 1.1-3.6-.6-1.1-2-1.1-4.2.2-6z"/></g></svg>',
  heat: '<svg viewBox="0 0 24 24"><g fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><circle cx="12" cy="12" r="4"/><path d="M12 2v2.5M12 19.5V22M2 12h2.5M19.5 12H22M4.9 4.9l1.8 1.8M17.3 17.3l1.8 1.8M4.9 19.1l1.8-1.8M17.3 6.7l1.8-1.8"/></g></svg>',
};

async function api(path, { method = 'GET', q = {}, body = null, json = null } = {}) {
  const u = new URL(path, location.href);
  for (const k in q) if (q[k] !== undefined && q[k] !== null) u.searchParams.set(k, q[k]);
  const headers = {};
  if (json !== null) { body = JSON.stringify(json); headers['Content-Type'] = 'application/json'; }
  const r = await fetch(u, { method, body, headers });
  const t = await r.text();
  let j; try { j = JSON.parse(t); } catch { j = t; }
  if (!r.ok) throw new Error((j && j.message) || t || ('HTTP ' + r.status));
  return j;
}
let toastTimer;
function toast(msg, bad = false) {
  const t = $('toast'); if (!t) return; t.textContent = msg; t.className = 'toast' + (bad ? ' bad' : ''); t.hidden = false;
  clearTimeout(toastTimer); toastTimer = setTimeout(() => { t.hidden = true; }, bad ? 4000 : 1800);
}
const hex = (v, w) => '0x' + Number(v).toString(16).toUpperCase().padStart(w, '0');
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const fanName = f => f === 'med' || f === 'medium' ? 'medium' : f;
const normalizeAc = s => ({ power: s.power, mode: s.mode, temp: Number(s.temp), fan: s.fan === 'medium' ? 'med' : s.fan });
function describeAc(s) {
  if (!s || s.power === 'off') return 'off';
  return [s.mode, s.temp !== undefined && s.temp !== null ? s.temp + '°C' : null, s.fan ? 'fan ' + fanName(s.fan) : null].filter(Boolean).join(' · ');
}
function describeAction(a) {
  if (!a) return '—';
  if (a.type === 'ac') return 'AC ' + describeAc(a.state);
  if (a.type === 'scene') return 'scene “' + a.name + '”';
  if (a.type === 'button') return 'button “' + (a.label || a.id) + '”';
  if (a.type === 'recording') return 'recording “' + a.name + '”';
  return '?';
}
function fmtAge(s) { return s < 60 ? Math.round(s) + ' s ago' : s < 3600 ? Math.round(s / 60) + ' min ago' : s < 172800 ? Math.round(s / 3600) + ' h ago' : Math.round(s / 86400) + ' d ago'; }
function fmtUptime(s) { const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60); return (d ? d + 'd ' : '') + (h ? h + 'h ' : '') + m + 'm'; }
function fmtRemaining(sec) { sec = Math.max(0, Math.round(sec)); const h = Math.floor(sec / 3600), m = Math.floor(sec % 3600 / 60), s = sec % 60; return h ? `${h}h ${String(m).padStart(2, '0')}m` : m ? `${m}m ${String(s).padStart(2, '0')}s` : `${s}s`; }
const pad2 = n => String(n).padStart(2, '0');
function fmtClock(t) { const d = new Date(t * 1000); return pad2(d.getHours()) + ':' + pad2(d.getMinutes()); }
function fmtDateTime(t) { const d = new Date(t * 1000), now = new Date(); const same = d.toDateString() === now.toDateString(); return (same ? '' : `${pad2(d.getDate())}/${pad2(d.getMonth() + 1)} `) + fmtClock(t); }
const DAY_NAMES = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];

function setConn(ok, text) { const d = $('dot'), c = $('connText'); if (d) d.className = 'dot' + (ok ? ' on' : ''); if (c) c.textContent = text; }
let deviceAddr = '';
async function loadSettings() {
  try { const s = await api('/local/settings'); deviceAddr = s.device; document.querySelectorAll('a[data-admin]').forEach(a => { a.href = 'http://' + deviceAddr + '/'; }); return s; } catch { return {}; }
}
// Poll /api/status every 5 s, keep the header dot current, hand the status to the page.
function startStatusPolling(onStatus, every = 5000) {
  const tick = async () => {
    try { const s = await api('/api/status'); setConn(true, (s.ip || deviceAddr) + ' · ' + s.rssi + ' dBm'); if (onStatus) onStatus(s); }
    catch (e) { setConn(false, 'device offline'); if (onStatus) onStatus(null); }
  };
  tick(); const id = setInterval(tick, every);
  document.addEventListener('visibilitychange', () => { if (!document.hidden) tick(); });
  return id;
}
// Mark the current page in the nav.
(() => { const here = location.pathname.replace(/\/index\.html$/, '/'); document.querySelectorAll('.tabs a').forEach(a => a.classList.toggle('on', a.getAttribute('href') === here)); })();
