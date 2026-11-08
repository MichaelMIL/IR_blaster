'use strict';
// Main page: AC remote, sleep timer, scenes, device buttons.
let ac = { power: 'off', mode: 'cool', temp: 24, fan: 'auto' };
let pending = null, busy = false, scenes = [], timers = [], timersNow = 0, timersAt = 0;

// -------------------------------------------------------------------- AC
function renderAc() {
  const on = ac.power === 'on';
  $('acdisplay').className = 'acdisplay ' + (on ? ac.mode : 'off');
  $('acTemp').textContent = ac.temp;
  $('acModeIcon').innerHTML = ICONS[ac.mode] || '';
  $('acModeText').textContent = on ? ac.mode : 'standby';
  $('acFanBars').className = 'fan ' + (FAN_LEVEL[ac.fan] || 'auto');
  $('acFanText').textContent = on ? fanName(ac.fan) : '';
  $('acPowerText').textContent = on ? 'on' : 'off';
  $('acPower').classList.toggle('on', on);
  document.querySelectorAll('#modeSeg button').forEach(b => b.classList.toggle('on', b.dataset.mode === ac.mode));
  document.querySelectorAll('#fanSeg button').forEach(b => b.classList.toggle('on', b.dataset.fan === ac.fan));
  document.querySelectorAll('#tempChips button').forEach(b => b.classList.toggle('on', Number(b.dataset.t) === Number(ac.temp)));
  $('tempDown').disabled = ac.temp <= 16; $('tempUp').disabled = ac.temp >= 31;
  renderScenes();
}
async function sendAc(q, note) {
  busy = true; $('acMsg').textContent = 'sending…';
  try {
    const r = await api('/api/ac', { method: 'POST', q });
    if (r.state) ac = normalizeAc(r.state);
    $('acMsg').textContent = note || r.message || 'sent';
  } catch (e) {
    $('acMsg').textContent = ''; toast('AC: ' + e.message, true); await loadAc();
  } finally { busy = false; renderAc(); }
}
async function loadAc() { try { ac = normalizeAc(await api('/api/ac')); renderAc(); } catch {} }
function setTemp(t) {
  t = Math.max(16, Math.min(31, t)); ac.temp = t; ac.power = 'on'; renderAc();
  clearTimeout(pending);                                   // several quick taps → one frame
  pending = setTimeout(() => { pending = null; sendAc({ power: 'on', temp: t }, `set ${t}°C`); }, 450);
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

// ------------------------------------------------------------ sleep timer
(() => {
  const c = $('sleepChips');
  [30, 60, 90, 120, 180].forEach(m => {
    const b = document.createElement('button'); b.textContent = m < 60 ? m + ' min' : (m / 60) + ' h';
    b.onclick = async () => {
      try { await api('/local/timers', { method: 'POST', json: { kind: 'countdown', minutes: m, label: 'Sleep timer', action: { type: 'ac', state: { power: 'off' } } } }); toast(`AC turns off in ${b.textContent}`); await loadTimers(); }
      catch (e) { toast(e.message, true); }
    };
    c.appendChild(b);
  });
})();
async function loadTimers() { try { const r = await api('/local/timers'); timers = r.timers; timersNow = r.now; timersAt = Date.now(); } catch { timers = []; } renderTimers(); }
function renderTimers() {
  const now = timersNow + (Date.now() - timersAt) / 1000;
  const cds = timers.filter(t => t.kind === 'countdown').sort((a, b) => a.due - b.due);
  const el = $('timerStrip'); el.innerHTML = '';
  if (!cds.length) return;
  cds.slice(0, 2).forEach(t => {
    const d = document.createElement('div'); d.className = 'strip';
    d.innerHTML = `<div class="num">${fmtRemaining(t.due - now)}</div><div class="grow"><div class="t">${esc(t.label || describeAction(t.action))}</div><div class="s">${esc(describeAction(t.action))} at ${fmtClock(t.due)}</div></div>`;
    const x = document.createElement('button'); x.className = 'sec'; x.textContent = 'Cancel';
    x.onclick = async () => { try { await api('/local/timers/delete', { method: 'POST', json: { id: t.id } }); toast('timer cancelled'); await loadTimers(); } catch (e) { toast(e.message, true); } };
    d.appendChild(x); el.appendChild(d);
  });
  if (cds.length > 2) { const m = document.createElement('div'); m.className = 'hint'; m.innerHTML = `+${cds.length - 2} more on the <a href="/timers">Timers</a> page`; el.appendChild(m); }
}
setInterval(() => { if (timers.some(t => t.kind === 'countdown')) { const now = timersNow + (Date.now() - timersAt) / 1000; if (timers.some(t => t.kind === 'countdown' && t.due <= now)) loadTimers(); else renderTimers(); } }, 1000);

// ---------------------------------------------------------------- scenes
function renderScenes() {
  const g = $('sceneGrid'); g.innerHTML = '';
  if (!scenes.length) { g.innerHTML = '<div class="empty">no scenes yet</div>'; return; }
  scenes.forEach((s, i) => {
    const st = s.state || {}; const fan = st.fan === 'medium' ? 'med' : st.fan;
    const active = st.power === 'off' ? ac.power === 'off' : (ac.power === 'on' && st.mode === ac.mode && Number(st.temp) === Number(ac.temp) && (!fan || fan === ac.fan));
    const t = document.createElement('button'); t.className = 'tile' + (active ? ' on' : '');
    t.innerHTML = `<div class="ic">${esc(s.icon || '🎛️')}</div><div class="t">${esc(s.name)}</div><div class="s">${esc(describeAc(st))}</div>`;
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
  if (q.power === 'off') { delete q.mode; delete q.temp; delete q.fan; } else q.power = 'on';
  ac = { ...ac, ...q, temp: q.temp !== undefined ? Number(q.temp) : ac.temp }; renderAc();
  await sendAc(q, 'scene: ' + s.name);
  tile.classList.remove('busy'); toast(s.name);
}
async function saveScene() {
  const name = $('sceneName').value.trim(); if (!name) { toast('give the scene a name', true); $('sceneName').focus(); return; }
  const icon = $('sceneIcon').value.trim() || (ac.power === 'on' ? MODE_EMOJI[ac.mode] : '⏻');
  const state = ac.power === 'on' ? { power: 'on', mode: ac.mode, temp: ac.temp, fan: ac.fan } : { power: 'off' };
  const i = scenes.findIndex(s => s.name.toLowerCase() === name.toLowerCase());
  if (i >= 0) scenes[i] = { name, icon, state }; else scenes.push({ name, icon, state });
  await persistScenes(); $('sceneName').value = ''; $('sceneIcon').value = ''; toast('saved ' + name);
}
async function deleteScene(i) { if (!confirm(`Delete scene "${scenes[i].name}"?`)) return; scenes.splice(i, 1); await persistScenes(); }
async function persistScenes() {
  try { await api('/local/scenes', { method: 'POST', json: scenes }); } catch (e) { toast('could not save scenes: ' + e.message, true); }
  renderScenes();
}
async function loadScenes() { try { scenes = await api('/local/scenes'); } catch { scenes = []; } renderScenes(); }

// --------------------------------------------------------------- buttons
async function loadButtons() {
  const g = $('btnGrid');
  try {
    const list = await api('/api/buttons'); g.innerHTML = '';
    if (!list.length) { g.innerHTML = '<div class="empty">no buttons yet — add them on the device admin page (Advanced)</div>'; return; }
    list.forEach((b, i) => {
      const t = document.createElement('button'); t.className = 'tile';
      const sub = b.type === 'nec' ? `NEC ${hex(b.addr, 2)} / ${hex(b.cmd, 2)}` : `recording "${b.rec}"`;
      t.innerHTML = `<div class="t">${esc(b.label)}</div><div class="s">${esc(sub)}${b.repeat > 1 ? ' ×' + b.repeat : ''}</div>`;
      t.onclick = async () => { t.classList.add('busy'); try { await api('/api/button', { method: 'POST', q: { id: i } }); toast(b.label); } catch (e) { toast(e.message, true); } t.classList.remove('busy'); };
      g.appendChild(t);
    });
  } catch (e) { g.innerHTML = `<div class="empty">${esc(e.message)}</div>`; }
}

// ------------------------------------------------------------------ boot
(async () => {
  renderAc();
  await loadSettings();
  await Promise.all([loadScenes(), loadAc(), loadButtons(), loadTimers()]);
  startStatusPolling(s => { if (s && s.ac && !busy && !pending) { const n = normalizeAc(s.ac); if (JSON.stringify(n) !== JSON.stringify(ac)) { ac = n; renderAc(); } } });
  setInterval(loadTimers, 30000);
  document.addEventListener('visibilitychange', () => { if (!document.hidden) { loadTimers(); loadButtons(); } });
})();
