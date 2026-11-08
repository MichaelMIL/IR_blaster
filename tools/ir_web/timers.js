'use strict';
// Timers page: one-shot countdowns and weekly schedules, executed by the server.
let scenes = [], buttons = [], recordings = [], timers = [], timersNow = 0, timersAt = 0;
const schDays = new Set([0, 1, 2, 3, 4, 5, 6]);

// ------------------------------------------------------------ action pick
function fillActions(sel) {
  const g = (label, opts) => opts.length ? `<optgroup label="${label}">${opts.join('')}</optgroup>` : '';
  sel.innerHTML =
    `<option value="off">Turn the AC off</option>` +
    g('Scenes', scenes.map(s => `<option value="scene:${esc(s.name)}">${esc(s.icon || '')} ${esc(s.name)} — ${esc(describeAc(s.state))}</option>`)) +
    `<option value="custom">Custom AC state…</option>` +
    g('Buttons', buttons.map((b, i) => `<option value="button:${i}">${esc(b.label)}</option>`)) +
    g('Recordings', recordings.map(r => `<option value="rec:${esc(r.name)}">${esc(r.name)}</option>`));
}
function customBox(box) {
  box.innerHTML = `<select data-k="mode">${MODES.map(m => `<option>${m}</option>`).join('')}</select>` +
    `<select data-k="temp">${Array.from({ length: 16 }, (_, i) => `<option${i + 16 === 24 ? ' selected' : ''}>${i + 16}</option>`).join('')}</select><span class="hint" style="margin:0">°C</span>` +
    `<select data-k="fan">${FANS.map(f => `<option value="${f}">${fanName(f)}</option>`).join('')}</select>`;
  box.querySelector('[data-k=mode]').value = 'cool';
}
function readAction(selId, boxId) {
  const v = $(selId).value;
  if (v === 'off') return { type: 'ac', state: { power: 'off' } };
  if (v === 'custom') { const b = $(boxId); return { type: 'ac', state: { power: 'on', mode: b.querySelector('[data-k=mode]').value, temp: Number(b.querySelector('[data-k=temp]').value), fan: b.querySelector('[data-k=fan]').value } }; }
  if (v.startsWith('scene:')) return { type: 'scene', name: v.slice(6) };
  if (v.startsWith('button:')) { const i = Number(v.slice(7)); return { type: 'button', id: i, label: (buttons[i] || {}).label }; }
  if (v.startsWith('rec:')) return { type: 'recording', name: v.slice(4) };
  return null;
}
function wireCustom(selId, boxId) {
  const sel = $(selId), box = $(boxId); customBox(box);
  sel.onchange = () => { box.hidden = sel.value !== 'custom'; };
}

// -------------------------------------------------------------- countdown
(() => {
  const c = $('cdChips');
  [15, 30, 60, 120].forEach(m => { const b = document.createElement('button'); b.textContent = m < 60 ? m + ' min' : (m / 60) + ' h'; b.onclick = () => { $('cdMin').value = m; $('cdAt').value = ''; c.querySelectorAll('button').forEach(x => x.classList.toggle('on', x === b)); }; c.appendChild(b); });
  $('cdMin').oninput = () => { $('cdAt').value = ''; c.querySelectorAll('button').forEach(x => x.classList.remove('on')); };
  $('cdAt').oninput = () => { $('cdMin').value = ''; c.querySelectorAll('button').forEach(x => x.classList.remove('on')); };
})();
async function addCountdown() {
  const action = readAction('cdAction', 'cdCustom'); if (!action) return toast('pick an action', true);
  const body = { kind: 'countdown', action, label: '' };
  if ($('cdAt').value) {
    const [h, m] = $('cdAt').value.split(':').map(Number); const d = new Date(); d.setHours(h, m, 0, 0); if (d.getTime() <= Date.now()) d.setDate(d.getDate() + 1);
    body.due = d.getTime() / 1000;
  } else {
    body.minutes = Number($('cdMin').value); if (!(body.minutes > 0)) return toast('how many minutes?', true);
  }
  try { await api('/local/timers', { method: 'POST', json: body }); toast('countdown started'); await loadTimers(); } catch (e) { toast(e.message, true); }
}

// -------------------------------------------------------------- schedules
(() => {
  const d = $('schDays');
  DAY_NAMES.forEach((n, i) => { const b = document.createElement('button'); b.textContent = n; b.className = 'on'; b.onclick = () => { if (schDays.has(i)) schDays.delete(i); else schDays.add(i); b.classList.toggle('on', schDays.has(i)); }; d.appendChild(b); });
})();
async function addSchedule() {
  const action = readAction('schAction', 'schCustom'); if (!action) return toast('pick an action', true);
  if (!schDays.size) return toast('pick at least one weekday', true);
  try { await api('/local/timers', { method: 'POST', json: { kind: 'schedule', time: $('schTime').value, days: [...schDays], action, label: $('schLabel').value } }); toast('schedule added'); $('schLabel').value = ''; await loadTimers(); }
  catch (e) { toast(e.message, true); }
}

// ------------------------------------------------------------------ lists
async function loadTimers() { try { const r = await api('/local/timers'); timers = r.timers; timersNow = r.now; timersAt = Date.now(); } catch (e) { toast(e.message, true); } render(); }
function daysText(days) { const s = new Set(days); if (s.size === 7) return 'every day'; if (s.size === 5 && [0, 1, 2, 3, 4].every(d => s.has(d))) return 'weekdays'; if (s.size === 2 && s.has(5) && s.has(6)) return 'weekends'; return DAY_NAMES.filter((_, i) => s.has(i)).join(' '); }
function nextRun(t) {
  const [h, m] = t.time.split(':').map(Number); const now = new Date();
  for (let i = 0; i < 8; i++) { const d = new Date(now); d.setDate(now.getDate() + i); d.setHours(h, m, 0, 0); const wd = (d.getDay() + 6) % 7; if (t.days.includes(wd) && d.getTime() > now.getTime()) return d; }
  return null;
}
function render() {
  const now = timersNow + (Date.now() - timersAt) / 1000;
  const cd = $('cdList'), sch = $('schList'); cd.innerHTML = ''; sch.innerHTML = '';
  const cds = timers.filter(t => t.kind === 'countdown').sort((a, b) => a.due - b.due);
  const schs = timers.filter(t => t.kind === 'schedule').sort((a, b) => a.time.localeCompare(b.time));
  if (!cds.length) cd.innerHTML = '<div class="empty">no countdown running</div>';
  cds.forEach(t => {
    const it = document.createElement('div'); it.className = 'item';
    it.innerHTML = `<div class="num" style="font-variant-numeric:tabular-nums;font-size:20px;font-weight:600;color:var(--acc);min-width:84px">${fmtRemaining(t.due - now)}</div><div class="grow"><div class="t">${esc(t.label || describeAction(t.action))}</div><div class="s">${esc(describeAction(t.action))} · at ${fmtClock(t.due)}</div></div>`;
    it.appendChild(delBtn(t, 'cancel')); cd.appendChild(it);
  });
  if (!schs.length) sch.innerHTML = '<div class="empty">no schedules yet</div>';
  schs.forEach(t => {
    const it = document.createElement('div'); it.className = 'item'; if (!t.enabled) it.style.opacity = .55;
    const nx = t.enabled ? nextRun(t) : null;
    it.innerHTML = `<div style="font-variant-numeric:tabular-nums;font-size:20px;font-weight:600;min-width:64px">${esc(t.time)}</div><div class="grow"><div class="t">${esc(t.label || describeAction(t.action))}</div><div class="s">${esc(daysText(t.days))} · ${esc(describeAction(t.action))}${nx ? ' · next ' + fmtDateTime(nx.getTime() / 1000) : ''}</div></div>`;
    const sw = document.createElement('button'); sw.className = 'switch' + (t.enabled ? ' on' : ''); sw.title = t.enabled ? 'enabled' : 'disabled';
    sw.onclick = async () => { try { await api('/local/timers/update', { method: 'POST', json: { id: t.id, enabled: !t.enabled } }); await loadTimers(); } catch (e) { toast(e.message, true); } };
    const run = document.createElement('button'); run.className = 'sec'; run.textContent = 'Run'; run.title = 'run this action now';
    run.onclick = async () => { run.disabled = true; try { const r = await api('/local/run', { method: 'POST', json: t.action }); toast(r.message); } catch (e) { toast(e.message, true); } run.disabled = false; };
    it.appendChild(run); it.appendChild(sw); it.appendChild(delBtn(t, 'delete')); sch.appendChild(it);
  });
}
function delBtn(t, what) {
  const x = document.createElement('button'); x.className = 'x'; x.title = what; x.textContent = '×';
  x.onclick = async () => { if (what === 'delete' && !confirm('Delete this schedule?')) return; try { await api('/local/timers/delete', { method: 'POST', json: { id: t.id } }); toast(what === 'delete' ? 'deleted' : 'cancelled'); await loadTimers(); } catch (e) { toast(e.message, true); } };
  return x;
}
setInterval(() => { if (!timers.some(t => t.kind === 'countdown')) return; const now = timersNow + (Date.now() - timersAt) / 1000; if (timers.some(t => t.kind === 'countdown' && t.due <= now)) loadTimers(); else render(); }, 1000);

// ------------------------------------------------------------------- boot
(async () => {
  const s = await loadSettings();
  $('tzInfo').textContent = s.server_time ? `server clock ${s.server_time.slice(11, 16)} ${s.tz || ''}` : '';
  const [sc, bt, rc] = await Promise.all([api('/local/scenes').catch(() => []), api('/api/buttons').catch(() => []), api('/api/recordings').catch(() => [])]);
  scenes = sc; buttons = bt; recordings = rc;
  fillActions($('cdAction')); fillActions($('schAction')); wireCustom('cdAction', 'cdCustom'); wireCustom('schAction', 'schCustom');
  await loadTimers(); setInterval(loadTimers, 30000);
  startStatusPolling();
})();
