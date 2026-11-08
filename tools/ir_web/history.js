'use strict';
// History page: samples (1/min) and events recorded by the server, drawn as SVG charts.
const C = { series: '#3987e5', on: 'rgba(57,135,229,.16)', good: '#0ca30c', warn: '#fab219', crit: '#d03b3b' };
let hours = 24, rows = [], evType = '', tableOn = false;

// --------------------------------------------------------------- loading
async function load() {
  try { const r = await api('/local/history', { q: { hours } }); rows = r.rows; }
  catch (e) { toast(e.message, true); rows = []; }
  draw();
}
document.querySelectorAll('#ranges button[data-h]').forEach(b => b.onclick = () => { hours = Number(b.dataset.h); document.querySelectorAll('#ranges button[data-h]').forEach(x => x.classList.toggle('on', x === b)); load(); });
document.querySelectorAll('#evFilter button').forEach(b => b.onclick = () => { evType = b.dataset.t; document.querySelectorAll('#evFilter button').forEach(x => x.classList.toggle('on', x === b)); drawEvents(); });
$('tableToggle').onclick = () => { tableOn = !tableOn; $('tableToggle').classList.toggle('on', tableOn); $('tableWrap').hidden = !tableOn; if (tableOn) drawTable(); };

// ------------------------------------------------------------ chart core
function fmtTick(t, span) {
  const d = new Date(t * 1000);
  if (span <= 36 * 3600) return pad2(d.getHours()) + ':' + pad2(d.getMinutes());
  return `${pad2(d.getDate())}/${pad2(d.getMonth() + 1)}`;
}
function niceTicks(min, max, n) {
  const span = max - min || 1, raw = span / n, mag = Math.pow(10, Math.floor(Math.log10(raw))), f = raw / mag;
  const step = (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * mag;
  const out = []; for (let v = Math.ceil(min / step) * step; v <= max + 1e-9; v += step) out.push(+v.toFixed(6)); return out;
}
// Time ticks on round hours / days (local time), 4-6 of them.
function timeTicks(t0, t1) {
  const span = t1 - t0, stepH = span <= 7 * 3600 ? 1 : span <= 13 * 3600 ? 2 : span <= 30 * 3600 ? 4 : span <= 4 * 86400 ? 12 : span <= 9 * 86400 ? 24 : span <= 20 * 86400 ? 48 : 24 * 6;
  const out = []; const d = new Date(t0 * 1000); d.setMinutes(0, 0, 0);
  if (stepH >= 24) d.setHours(0); else d.setHours(Math.ceil(d.getHours() / stepH) * stepH);
  for (let t = d.getTime() / 1000; t <= t1; t += stepH * 3600) if (t >= t0) out.push(t);
  return out;
}
// One chart: a single series over time (line or step), optional shaded bands, crosshair tooltip.
function timeChart(el, { title, pts, fmt, unit, step = false, bands = [], yMin, yMax, latest }) {
  const W = Math.max(280, el.clientWidth - 28), H = 180, L = 40, R = 8, T = 10, B = 24;
  el.innerHTML = `<div class="head"><h3>${esc(title)}</h3><span class="val">${latest != null ? esc(latest) : ''}</span></div>`;
  if (pts.length < 2) { el.innerHTML += '<div class="empty">not enough data yet</div>'; return; }
  const t0 = Date.now() / 1000 - hours * 3600, t1 = Date.now() / 1000;
  const ys = pts.map(p => p.y);
  let lo = yMin != null ? yMin : Math.min(...ys), hi = yMax != null ? yMax : Math.max(...ys);
  if (hi === lo) { hi += 1; lo -= 1; }
  const padY = (hi - lo) * 0.08; if (yMin == null) lo -= padY; if (yMax == null) hi += padY;
  const x = t => L + (t - t0) / (t1 - t0) * (W - L - R), y = v => T + (hi - v) / (hi - lo) * (H - T - B);
  const yt = niceTicks(lo, hi, 4), xt = timeTicks(t0, t1);
  let d = '';
  pts.forEach((p, i) => { const px = x(p.t), py = y(p.y); if (i === 0) d += `M${px},${py}`; else d += step ? `H${px}V${py}` : `L${px},${py}`; });
  const svg = `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none">
    ${bands.map(b => `<rect x="${x(b.from)}" y="${T}" width="${Math.max(1, x(b.to) - x(b.from))}" height="${H - T - B}" fill="${C.on}"/>`).join('')}
    ${yt.map(v => `<line class="grid" x1="${L}" x2="${W - R}" y1="${y(v)}" y2="${y(v)}"/><text class="ax" x="${L - 6}" y="${y(v) + 4}" text-anchor="end">${fmt ? fmt(v) : v}</text>`).join('')}
    ${xt.map(v => `<text class="ax" x="${x(v)}" y="${H - 6}" text-anchor="middle">${fmtTick(v, t1 - t0)}</text>`).join('')}
    <path d="${d}" fill="none" stroke="${C.series}" stroke-width="2" stroke-linejoin="round" stroke-linecap="round" vector-effect="non-scaling-stroke"/>
    <line id="xh" x1="0" x2="0" y1="${T}" y2="${H - B}" stroke="var(--mute)" stroke-dasharray="3 3" visibility="hidden"/>
    <circle id="dotm" r="4" fill="${C.series}" stroke="var(--card)" stroke-width="2" visibility="hidden"/>
    <rect x="${L}" y="${T}" width="${W - L - R}" height="${H - T - B}" fill="transparent" id="hit"/>
  </svg><div class="tip" hidden></div>`;
  el.insertAdjacentHTML('beforeend', svg);
  const s = el.querySelector('svg'), hit = s.querySelector('#hit'), xh = s.querySelector('#xh'), dot = s.querySelector('#dotm'), tip = el.querySelector('.tip');
  const move = ev => {
    const r = s.getBoundingClientRect(); const cx = ((ev.touches ? ev.touches[0].clientX : ev.clientX) - r.left) / r.width * W;
    const tt = t0 + (cx - L) / (W - L - R) * (t1 - t0);
    let best = pts[0]; for (const p of pts) if (Math.abs(p.t - tt) < Math.abs(best.t - tt)) best = p;
    xh.setAttribute('x1', x(best.t)); xh.setAttribute('x2', x(best.t)); xh.setAttribute('visibility', 'visible');
    dot.setAttribute('cx', x(best.t)); dot.setAttribute('cy', y(best.y)); dot.setAttribute('visibility', 'visible');
    tip.hidden = false; tip.innerHTML = `<b>${fmt ? fmt(best.y) : best.y}${unit || ''}</b>${best.extra ? ' · ' + esc(best.extra) : ''}<br>${fmtDateTime(best.t)}`;
    const er = el.getBoundingClientRect(); tip.style.left = (x(best.t) / W * r.width + (r.left - er.left)) + 'px'; tip.style.top = (y(best.y) / H * r.height + (r.top - er.top)) + 'px';
  };
  const leave = () => { xh.setAttribute('visibility', 'hidden'); dot.setAttribute('visibility', 'hidden'); tip.hidden = true; };
  hit.addEventListener('mousemove', move); hit.addEventListener('touchstart', move, { passive: true }); hit.addEventListener('touchmove', move, { passive: true });
  hit.addEventListener('mouseleave', leave); hit.addEventListener('touchend', leave);
}
// Bars: one value per day (hours the AC was on).
function barChart(el, { title, bars, fmt, latest }) {
  const W = Math.max(280, el.clientWidth - 28), H = 180, L = 36, R = 8, T = 10, B = 24;
  el.innerHTML = `<div class="head"><h3>${esc(title)}</h3><span class="val">${latest != null ? esc(latest) : ''}</span></div>`;
  if (!bars.length) { el.innerHTML += '<div class="empty">not enough data yet</div>'; return; }
  const hi = Math.max(1, ...bars.map(b => b.v)) * 1.1, yt = niceTicks(0, hi, 4);
  const bw = (W - L - R) / bars.length, y = v => T + (hi - v) / hi * (H - T - B);
  const svg = `<svg viewBox="0 0 ${W} ${H}">
    ${yt.map(v => `<line class="grid" x1="${L}" x2="${W - R}" y1="${y(v)}" y2="${y(v)}"/><text class="ax" x="${L - 6}" y="${y(v) + 4}" text-anchor="end">${fmt ? fmt(v) : v}</text>`).join('')}
    ${bars.map((b, i) => { const x0 = L + i * bw + 2, w = Math.max(2, bw - 4), top = y(b.v), h = Math.max(0, H - B - top); if (h < 0.5) return `<rect class="bar" data-i="${i}" x="${x0}" y="${H - B - 1}" width="${w}" height="1" fill="var(--line)"/>`; return `<path class="bar" data-i="${i}" d="M${x0},${H - B}v${-h + Math.min(4, h)}a4,4 0 0 1 4,-4h${w - 8}a4,4 0 0 1 4,4v${h - Math.min(4, h)}z" fill="${C.series}"/>`; }).join('')}
    ${bars.map((b, i) => bars.length <= 16 || i % Math.ceil(bars.length / 10) === 0 ? `<text class="ax" x="${L + i * bw + bw / 2}" y="${H - 6}" text-anchor="middle">${esc(b.label)}</text>` : '').join('')}
  </svg><div class="tip" hidden></div>`;
  el.insertAdjacentHTML('beforeend', svg);
  const tip = el.querySelector('.tip'), s = el.querySelector('svg');
  s.querySelectorAll('.bar').forEach(p => {
    const b = bars[Number(p.dataset.i)];
    const show = ev => { tip.hidden = false; tip.innerHTML = `<b>${fmt ? fmt(b.v) : b.v}</b><br>${esc(b.full || b.label)}`; const r = s.getBoundingClientRect(), er = el.getBoundingClientRect(); const bb = p.getBBox(); tip.style.left = ((bb.x + bb.width / 2) / W * r.width + r.left - er.left) + 'px'; tip.style.top = (bb.y / H * r.height + r.top - er.top) + 'px'; p.style.opacity = .8; };
    const hide = () => { tip.hidden = true; p.style.opacity = 1; };
    p.addEventListener('mouseenter', show); p.addEventListener('mouseleave', hide); p.addEventListener('touchstart', show, { passive: true }); p.addEventListener('touchend', hide);
  });
}

// ------------------------------------------------------------------ draw
function draw() {
  const samples = rows.filter(r => r.k === 's'), online = samples.filter(r => r.online !== false);
  const now = Date.now() / 1000, t0 = now - hours * 3600;
  // AC on/off bands and setpoint series
  const bands = []; let onFrom = null, onSec = 0;
  const tempPts = [];
  online.forEach((r, i) => {
    const on = r.ac && r.ac.power === 'on';
    if (on && onFrom == null) onFrom = r.t;
    if (!on && onFrom != null) { bands.push({ from: onFrom, to: r.t }); onSec += r.t - onFrom; onFrom = null; }
    if (r.ac && r.ac.temp != null) tempPts.push({ t: r.t, y: Number(r.ac.temp), extra: describeAc(r.ac) });
  });
  if (onFrom != null) { bands.push({ from: onFrom, to: now }); onSec += now - onFrom; }
  const last = online[online.length - 1];
  const onlinePct = samples.length ? Math.round(online.length / samples.length * 100) : null;
  const rxCount = rows.filter(r => r.k === 'e' && r.type === 'rx').length, cmdCount = rows.filter(r => r.k === 'e' && r.type === 'cmd').length;
  $('stats').innerHTML = [
    ['AC on', onSec ? (onSec / 3600).toFixed(1) : '0', 'h'],
    ['commands', cmdCount, ''], ['IR received', rxCount, ''],
    ['online', onlinePct == null ? '—' : onlinePct, onlinePct == null ? '' : '%'],
    ['free heap', last && last.heap != null ? Math.round(last.heap / 1024) : '—', 'KB'],
  ].map(([k, v, u]) => `<div class="stat"><div class="k">${k}</div><div class="v">${v}<small>${u}</small></div></div>`).join('');

  timeChart($('chTemp'), { title: 'AC setpoint', pts: tempPts, step: true, bands, fmt: v => Math.round(v) + '°', unit: '°C', yMin: 16, yMax: 31, latest: last && last.ac ? describeAc(last.ac) : null });
  // hours on per day
  const days = new Map();
  for (let i = 0; i < online.length - 1; i++) {
    const r = online[i], nxt = online[i + 1]; if (!(r.ac && r.ac.power === 'on')) continue;
    const dur = Math.min(nxt.t - r.t, SAMPLE_GAP_MAX); const d = new Date(r.t * 1000); const key = `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`;
    days.set(key, (days.get(key) || 0) + dur / 3600);
  }
  const dayKeys = []; for (let t = t0; t <= now; t += 86400) { const d = new Date(t * 1000); dayKeys.push(`${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`); }
  const todayKey = (() => { const d = new Date(); return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`; })();
  if (!dayKeys.includes(todayKey)) dayKeys.push(todayKey);
  const bars = hours <= 24 ? hourlyBars(online, t0, now) : dayKeys.map(k => ({ label: k.slice(8) + '/' + k.slice(5, 7), full: k, v: +(days.get(k) || 0).toFixed(2) }));
  barChart($('chOn'), { title: hours <= 24 ? 'AC on, minutes per hour' : 'AC on, hours per day', bars, fmt: v => hours <= 24 ? Math.round(v) + 'm' : v.toFixed(1) + 'h' });
  timeChart($('chRssi'), { title: 'WiFi signal', pts: online.filter(r => r.rssi != null).map(r => ({ t: r.t, y: r.rssi })), fmt: v => Math.round(v), unit: ' dBm', latest: last && last.rssi != null ? last.rssi + ' dBm' : null });
  timeChart($('chHeap'), { title: 'Free heap', pts: online.filter(r => r.heap != null).map(r => ({ t: r.t, y: r.heap / 1024, extra: r.heap_min != null ? 'min ' + Math.round(r.heap_min / 1024) + ' KB' : '' })), fmt: v => Math.round(v), unit: ' KB', yMin: 0, latest: last && last.heap != null ? Math.round(last.heap / 1024) + ' KB' : null });
  drawEvents(); if (tableOn) drawTable();
}
const SAMPLE_GAP_MAX = 5 * 60;   // a gap longer than this means the server was down; don't count it as "on"
function hourlyBars(online, t0, now) {
  const start = Math.floor(t0 / 3600) * 3600, out = [];
  for (let h = start; h <= now; h += 3600) out.push({ t: h, label: pad2(new Date(h * 1000).getHours()), full: fmtDateTime(h) + ' – ' + fmtClock(h + 3600), v: 0 });
  for (let i = 0; i < online.length - 1; i++) {
    const r = online[i]; if (!(r.ac && r.ac.power === 'on')) continue;
    const dur = Math.min(online[i + 1].t - r.t, SAMPLE_GAP_MAX) / 60; const b = out[Math.floor((r.t - start) / 3600)]; if (b) b.v += dur;
  }
  return out.map(b => ({ ...b, v: Math.round(b.v) }));
}
function drawEvents() {
  const ev = rows.filter(r => r.k === 'e' && (!evType || r.type === evType)).sort((a, b) => b.t - a.t).slice(0, 200);
  const el = $('events');
  if (!ev.length) { el.innerHTML = '<div class="empty">no events in this range</div>'; return; }
  el.innerHTML = ev.map(e => `<div class="ev ${esc(e.type)}"><span class="when">${fmtDateTime(e.t)}</span><span class="tag">${esc(e.type)}</span><span>${esc(e.text)}${e.data && e.data.source ? ` <span class="hint" style="margin:0">(${esc(e.data.source)})</span>` : ''}</span></div>`).join('');
}
function drawTable() {
  const s = rows.filter(r => r.k === 's').slice().reverse().slice(0, 1000);
  $('tableWrap').innerHTML = `<table class="tbl"><tr><th>time</th><th>online</th><th>AC</th><th>RSSI</th><th>heap KB</th><th>min KB</th><th>uptime</th></tr>` +
    s.map(r => `<tr><td>${fmtDateTime(r.t)}</td><td>${r.online === false ? 'no' : 'yes'}</td><td>${esc(describeAc(r.ac))}</td><td>${r.rssi ?? ''}</td><td>${r.heap != null ? Math.round(r.heap / 1024) : ''}</td><td>${r.heap_min != null ? Math.round(r.heap_min / 1024) : ''}</td><td>${r.up != null ? fmtUptime(r.up) : ''}</td></tr>`).join('') + '</table>';
}
let resizeT; window.addEventListener('resize', () => { clearTimeout(resizeT); resizeT = setTimeout(draw, 150); });

(async () => { await loadSettings(); startStatusPolling(); await load(); setInterval(load, 60000); })();
