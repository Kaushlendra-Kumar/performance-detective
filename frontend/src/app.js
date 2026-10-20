import { api as liveApi, ApiError, DemoApi } from './api.js';
import { FlameGraph, matchedShare } from './flamegraph.js';
import {
  basename, bytes, count, escapeHtml as h, nanos, parseArgLines, parseEnvLines, pct, pp, seconds, shortName,
  signedPct, time,
} from './format.js';

const $ = (sel, root = document) => root.querySelector(sel);
const view = $('#view');
let api = liveApi;
let cleanup = [];
const TERMINAL = new Set(['COMPLETED', 'FAILED', 'CANCELLED']);

function onCleanup(fn) { cleanup.push(fn); }
function runCleanup() { cleanup.forEach((f) => { try { f(); } catch { /* ignore */ } }); cleanup = []; }

function stateBadge(state) {
  const cls = { COMPLETED: 'ok', FAILED: 'bad', CANCELLED: 'warn', RUNNING: 'run', ANALYZING: 'run', STARTING: 'run',
    QUEUED: 'muted', CREATED: 'muted', STOPPING: 'warn' }[state] || 'muted';
  return `<span class="badge ${cls}">${h(state)}</span>`;
}
function sevBadge(sev) { return `<span class="sev sev-${h(sev)}">${h(sev)}</span>`; }

function errorBox(err, title = 'Something went wrong') {
  const msg = err instanceof ApiError ? `${err.message} (${err.code})` : String(err && err.message ? err.message : err);
  return `<div class="error-box" role="alert"><strong>${h(title)}</strong><div>${h(msg)}</div></div>`;
}
const loading = (what = 'Loading') => `<div class="loading" aria-busy="true">${h(what)}…</div>`;
const empty = (msg) => `<div class="empty">${h(msg)}</div>`;

function setDemoBanner(on) {
  $('#demo-banner').hidden = !on;
  document.body.classList.toggle('demo', on);
}

// ------------------------------------------------------------------ router
const LOOPBACK = new Set(['127.0.0.1', 'localhost', '[::1]']);
let liveServer = null;
async function liveServerReachable() {
  if (liveServer === null) liveServer = liveApi.health().then(() => true, () => false);
  return liveServer;
}

async function route() {
  runCleanup();
  const hash = location.hash || '#/';
  const [path, query] = hash.slice(1).split('?');
  const params = new URLSearchParams(query || '');
  const parts = path.split('/').filter(Boolean);
  document.querySelectorAll('nav a').forEach((a) => a.classList.toggle('active', a.getAttribute('href') === '#/' + (parts[0] || '')));
  try {
    if (parts[0] === 'demo') {
      if (!(api instanceof DemoApi)) {
        view.innerHTML = loading('Loading demo data');
        const res = await fetch('demo/demo-session.json');
        if (!res.ok) throw new Error('demo data not found — run `detective demo-data --out frontend/demo/demo-session.json`');
        api = new DemoApi(await res.json());
      }
      setDemoBanner(true);
      return renderSession(api.id, params);
    }
    api = liveApi;
    setDemoBanner(false);
    // Hosted as a static site (no profiler server behind it): fall back to the labelled demo.
    if (!LOOPBACK.has(location.hostname) && !(await liveServerReachable())) return location.replace('#/demo');
    if (!parts.length) return renderDashboard();
    if (parts[0] === 'new') return renderNew(params);
    if (parts[0] === 's' && parts[1]) return renderSession(parts[1], params);
    if (parts[0] === 'compare') return renderCompare(params);
    view.innerHTML = empty('Page not found.');
  } catch (e) {
    view.innerHTML = errorBox(e);
  }
}

// --------------------------------------------------------------- dashboard
async function renderDashboard() {
  view.innerHTML = loading('Loading sessions');
  const [health, list] = await Promise.all([api.health().catch((e) => ({ error: e })), api.sessions()]);
  const sessions = list.sessions;
  const done = sessions.filter((s) => s.state === 'COMPLETED');
  const failed = sessions.filter((s) => s.state === 'FAILED');
  const running = sessions.filter((s) => !TERMINAL.has(s.state));
  const walls = done.map((s) => s.summary?.wall_seconds).filter((x) => x != null);
  const topFns = [];
  for (const s of done.slice(0, 10)) {
    const n = s.summary?.sampling?.total_samples || 0;
    if (n < 50) continue;  // too few samples for a meaningful share
    for (const f of s.summary?.top_functions || []) topFns.push({ ...f, session: s, n });
  }
  topFns.sort((a, b) => b.self_share - a.self_share);
  const findings = [];
  for (const s of sessions.slice(0, 20)) for (const f of s.top_findings || []) if (f.severity === 'high') findings.push({ ...f, session: s });

  const caps = health.capabilities;
  view.innerHTML = `
    <section class="page-head">
      <div><h1>Dashboard</h1><p class="sub">${caps ? h(caps.summary) : errorBox(health.error, 'Server health unavailable')}</p></div>
      <div class="actions"><a class="btn primary" href="#/new">New profiling session</a>
        <a class="btn" href="#/compare">Compare runs</a><a class="btn ghost" href="#/demo">Open demo</a></div>
    </section>
    <section class="cards">
      <div class="card stat"><span class="label">Sessions</span><span class="value">${sessions.length}</span></div>
      <div class="card stat"><span class="label">Completed</span><span class="value ok-text">${done.length}</span></div>
      <div class="card stat"><span class="label">Failed / cancelled</span><span class="value bad-text">${failed.length + sessions.filter((s) => s.state === 'CANCELLED').length}</span></div>
      <div class="card stat"><span class="label">In progress</span><span class="value">${running.length}</span></div>
      <div class="card stat"><span class="label">Median wall time (completed)</span><span class="value">${walls.length ? seconds(walls.sort((a, b) => a - b)[Math.floor(walls.length / 2)]) : '—'}</span></div>
    </section>
    <section class="grid-2">
      <div class="card"><h2>High-priority findings</h2>
        ${findings.length ? `<ul class="plain">${findings.slice(0, 8).map((f) => `<li>${sevBadge(f.severity)} <a href="#/s/${h(f.session.id)}?tab=overview">${h(shortName(f.title, 90))}</a>
          <span class="muted"> · ${h(basename(f.session.config.executable))}</span></li>`).join('')}</ul>` : empty('No high-severity findings yet.')}
      </div>
      <div class="card"><h2>Most expensive recent functions <span class="hint" title="Self share of samples in each session (sampling estimate)">self, est.</span></h2>
        ${topFns.length ? `<table class="tbl"><tbody>${topFns.slice(0, 8).map((f) => `<tr><td class="num">${pct(f.self_share)}</td>
          <td class="mono"><a href="#/s/${h(f.session.id)}?tab=functions&fn=${h(f.id)}">${h(shortName(f.name, 60))}</a></td>
          <td class="muted">${h(basename(f.session.config.executable))} <span class="small">n=${count(f.n)}</span></td></tr>`).join('')}</tbody></table>` : empty('Run a sampling session (≥50 samples) to see hot functions here.')}
      </div>
    </section>
    <section class="card"><h2>Recent sessions</h2>
      ${sessions.length ? sessionTable(sessions) : empty('No sessions yet. Start one with “New profiling session”.')}
    </section>`;
  if (running.length) {
    const t = setTimeout(() => route(), 1500);
    onCleanup(() => clearTimeout(t));
  }
}

function sessionTable(sessions) {
  return `<table class="tbl sessions"><thead><tr><th>State</th><th>Program</th><th>Label</th><th class="num">Wall</th>
    <th class="num">Samples</th><th>Top finding</th><th>Created</th></tr></thead><tbody>
    ${sessions.map((s) => `<tr>
      <td>${stateBadge(s.state)}</td>
      <td class="mono"><a href="#/s/${h(s.id)}">${h(basename(s.config.executable))}</a> <span class="muted">${h((s.config.args || []).join(' ').slice(0, 50))}</span></td>
      <td>${h(s.config.label || '')}${s.imported ? ' <span class="badge muted">imported</span>' : ''}</td>
      <td class="num">${s.summary ? seconds(s.summary.wall_seconds) : '—'}</td>
      <td class="num">${s.summary?.sampling ? count(s.summary.sampling.total_samples) : '—'}</td>
      <td>${s.top_findings?.length ? sevBadge(s.top_findings[0].severity) + ' ' + h(shortName(s.top_findings[0].title, 50)) : '<span class="muted">—</span>'}</td>
      <td class="muted">${time(s.created_ms)}</td></tr>`).join('')}
    </tbody></table>`;
}

// ------------------------------------------------------------- new session
async function renderNew(params) {
  view.innerHTML = loading('Loading targets');
  let targets = { targets: [], allowed_dirs: [] };
  try { targets = await api.targets(); } catch (e) { /* shown below */ }
  const health = await api.health().catch(() => null);
  const allocOk = health?.capabilities?.allocation_tracking;
  view.innerHTML = `
    <section class="page-head"><div><h1>New profiling session</h1>
      <p class="sub">Targets must live in an allowed directory: ${targets.allowed_dirs.map((d) => `<code>${h(d)}</code>`).join(', ') || '(none configured)'}</p></div></section>
    <form id="new-form" class="card form" novalidate>
      <label>Executable
        <input name="executable" list="targets" required placeholder="/absolute/path/to/program" value="${h(params.get('exe') || '')}" autocomplete="off">
        <datalist id="targets">${targets.targets.map((t) => `<option value="${h(t)}">`).join('')}</datalist>
      </label>
      <label>Arguments <span class="hint">one per line — passed verbatim, no shell</span>
        <textarea name="args" rows="3" class="mono" placeholder="--mode\nslow"></textarea></label>
      <label>Environment <span class="hint">KEY=VALUE per line</span>
        <textarea name="env" rows="2" class="mono"></textarea></label>
      <div class="row">
        <label>Mode
          <select name="mode">
            <option value="sampling">Sampling (perf_event_open cpu-clock)</option>
            <option value="instrumentation">Instrumentation (target linked with detective_instr)</option>
            <option value="none">None (timing / allocations only)</option>
          </select></label>
        <label>Sampling frequency (Hz)<input name="frequency_hz" type="number" min="1" max="10000" value="999"></label>
        <label>Timeout (s)<input name="timeout" type="number" min="1" max="3600" value="120"></label>
      </div>
      <div class="row">
        <label>Warm-up runs<input name="warmup_runs" type="number" min="0" max="20" value="0"></label>
        <label>Benchmark runs <span class="hint">unprofiled, ≥5 for comparisons</span><input name="benchmark_runs" type="number" min="0" max="100" value="0"></label>
        <label>Label<input name="label" maxlength="200" placeholder="optional"></label>
      </div>
      <label class="check"><input type="checkbox" name="track_allocations" ${allocOk ? '' : 'disabled'}>
        Track heap allocations (LD_PRELOAD; adds overhead) ${allocOk ? '' : '<span class="hint">— tracker library not available</span>'}</label>
      <div id="form-error"></div>
      <div class="actions"><button class="btn primary" type="submit">Start profiling</button></div>
    </form>`;
  $('#new-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const fd = new FormData(e.target);
    const envParsed = parseEnvLines(fd.get('env'));
    if (envParsed.errors.length) { $('#form-error').innerHTML = errorBox(envParsed.errors.join('; '), 'Invalid environment'); return; }
    const cfg = {
      executable: fd.get('executable').trim(),
      args: parseArgLines(fd.get('args')),
      env: envParsed.env,
      mode: fd.get('mode'),
      frequency_hz: Number(fd.get('frequency_hz')),
      timeout_ms: Number(fd.get('timeout')) * 1000,
      warmup_runs: Number(fd.get('warmup_runs')),
      benchmark_runs: Number(fd.get('benchmark_runs')),
      track_allocations: fd.get('track_allocations') === 'on',
      label: fd.get('label'),
    };
    try {
      const s = await api.create(cfg, true);
      location.hash = `#/s/${s.id}`;
    } catch (err) {
      $('#form-error').innerHTML = errorBox(err, 'Could not start the session');
    }
  });
}

// ----------------------------------------------------------------- session
const TABS = [
  ['overview', 'Overview'], ['flame', 'Flame graph'], ['functions', 'Functions'],
  ['instrumentation', 'Instrumentation'], ['memory', 'Memory'], ['output', 'Output'],
];

async function renderSession(id, params) {
  view.innerHTML = loading('Loading session');
  const s = await api.session(id);
  const tab = params.get('tab') || 'overview';
  const base = api.demo ? '#/demo' : `#/s/${encodeURIComponent(id)}`;
  const cfg = s.config;
  const p = s.process || {};
  const sum = s.summary;
  const col = sum?.collection;
  const profilingStatus = !col ? '—'
    : col.sampling_requested ? (col.sampling_collected ? `sampled (${h(col.event)}, ${col.frequency_hz} Hz)` : `<span class="bad-text">not collected: ${h(col.sampling_error)}</span>`)
    : cfg.mode;
  view.innerHTML = `
    <section class="page-head">
      <div>
        <h1 class="mono">${h(basename(cfg.executable))} ${stateBadge(s.state)} ${s.demo ? '<span class="badge warn">DEMO</span>' : ''}</h1>
        <p class="sub">${h(s.message || '')}${cfg.label ? ' · ' + h(cfg.label) : ''}</p>
      </div>
      <div class="actions">
        ${!TERMINAL.has(s.state) && !api.demo ? '<button class="btn danger" id="stop-btn">Stop</button>' : ''}
        ${s.state === 'CREATED' && !api.demo ? '<button class="btn primary" id="start-btn">Start</button>' : ''}
        ${!api.demo ? `<a class="btn" href="#/new?exe=${encodeURIComponent(cfg.executable)}">Run again…</a>
          <a class="btn" href="#/compare?a=${encodeURIComponent(id)}">Compare…</a>` : ''}
        <details class="export"><summary class="btn">Export</summary><div class="menu">
          <a href="${api.exportUrl(id, 'json')}" download>Session (JSON)</a>
          ${!api.demo ? `<a href="${api.exportUrl(id, 'csv')}" download>Function table (CSV)</a>
          <a href="${api.exportUrl(id, 'svg')}" download>Flame graph (SVG)</a>` : ''}
        </div></details>
      </div>
    </section>
    <section class="meta card">
      <dl>
        <div><dt>Executable</dt><dd class="mono">${h(cfg.executable)}</dd></div>
        <div><dt>Arguments</dt><dd class="mono">${(cfg.args || []).map((a) => `<span class="arg">${h(a)}</span>`).join(' ') || '<span class="muted">none</span>'}</dd></div>
        <div><dt>PID</dt><dd class="mono">${s.pid > 0 ? s.pid : (p.pid > 0 ? p.pid : '—')}</dd></div>
        <div><dt>Started</dt><dd>${time(p.start_unix_ms)}</dd></div>
        <div><dt>Ended</dt><dd>${time(p.end_unix_ms)}</dd></div>
        <div><dt>Exit</dt><dd>${p.started ? (p.exited_normally ? `code ${p.exit_code}` : `<span class="bad-text">signal ${p.term_signal}</span>`) + (p.timed_out ? ' (timeout)' : '') + (p.cancelled ? ' (cancelled)' : '') : (p.start_error ? `<span class="bad-text">${h(p.start_error)}</span>` : '—')}</dd></div>
        <div><dt>Wall-clock</dt><dd>${sum ? seconds(sum.wall_seconds) : '—'}</dd></div>
        <div><dt>Profiling</dt><dd>${profilingStatus}</dd></div>
        <div><dt>Mode</dt><dd>${h(cfg.mode)}${cfg.track_allocations ? ' + allocations' : ''}</dd></div>
      </dl>
    </section>
    ${TERMINAL.has(s.state) || s.summary ? `
    <nav class="tabs" role="tablist">${TABS.map(([k, label]) => `<a role="tab" aria-selected="${k === tab}" class="${k === tab ? 'active' : ''}" href="${base}?tab=${k}">${label}</a>`).join('')}</nav>
    <section id="tab-body"></section>` : `<section class="card running">${loading('Profiling in progress — this page updates automatically')}</section>`}
    <aside id="fn-panel" class="panel" hidden aria-label="Function details"></aside>`;

  $('#stop-btn')?.addEventListener('click', async () => {
    try { await api.stop(id); } catch (e) { alert(e.message); }
    route();
  });
  $('#start-btn')?.addEventListener('click', async () => {
    try { await api.start(id); } catch (e) { alert(e.message); }
    route();
  });
  if (!TERMINAL.has(s.state)) {
    const t = setTimeout(() => route(), 700);
    onCleanup(() => clearTimeout(t));
    if (!s.summary) return;
  }
  const body = $('#tab-body');
  if (!body) return;
  const ctx = { id, base, session: s };
  const renderers = { overview: tabOverview, flame: tabFlame, functions: tabFunctions, instrumentation: tabInstrumentation, memory: tabMemory, output: tabOutput };
  body.innerHTML = loading();
  try {
    await (renderers[tab] || tabOverview)(body, ctx, params);
  } catch (e) {
    body.innerHTML = errorBox(e, 'Could not load this view');
  }
  if (params.get('fn')) openFunction(ctx, params.get('fn'));
}

function warningsBlock(col) {
  const items = [...(col?.warnings || [])];
  if (col?.instrumentation_error) items.push('Instrumentation: ' + col.instrumentation_error);
  if (col?.allocation_error) items.push('Allocation tracking: ' + col.allocation_error);
  if (!items.length) return '';
  return `<div class="card warn-box"><h3>Collection notes</h3><ul>${items.map((w) => `<li>${h(w)}</li>`).join('')}</ul></div>`;
}

async function tabOverview(body, ctx) {
  const [sum, fr] = await Promise.all([api.summary(ctx.id), api.findings(ctx.id)]);
  const smp = sum.sampling;
  const bm = sum.benchmark;
  body.innerHTML = `
    <section class="cards">
      <div class="card stat"><span class="label">Wall-clock time</span><span class="value">${seconds(sum.wall_seconds)}</span><span class="foot">steady_clock, profiled run</span></div>
      <div class="card stat"><span class="label">CPU time (user+sys)</span><span class="value">${seconds(sum.cpu_seconds)}</span><span class="foot">getrusage</span></div>
      <div class="card stat"><span class="label">Avg. cores busy</span><span class="value">${sum.avg_cores_busy.toFixed(2)}</span><span class="foot">CPU ÷ wall</span></div>
      <div class="card stat"><span class="label">Samples</span><span class="value">${smp ? count(smp.total_samples) : '—'}</span><span class="foot">${smp ? '≈ ' + seconds(smp.estimated_cpu_seconds) + ' CPU (estimate)' : 'no sampling data'}</span></div>
      <div class="card stat"><span class="label">Peak RSS</span><span class="value">${bytes(sum.max_rss_kb * 1024)}</span><span class="foot">resident memory, getrusage</span></div>
      <div class="card stat"><span class="label">Stack quality</span><span class="value">${smp ? pct(smp.complete_stack_fraction, 0) : '—'}</span><span class="foot">stacks reaching an entry point</span></div>
    </section>
    ${warningsBlock(sum.collection)}
    ${bm ? benchmarkBlock(bm) : ''}
    <section class="grid-2 wide-left">
      <div><h2>Findings <span class="hint" title="${h(fr.scoring || 'score = 100 × impact × evidence weight')}">ranked by score</span></h2>
        ${fr.findings.length ? fr.findings.map((f) => findingCard(f, ctx)).join('') : empty('No credible bottleneck was identified from the collected evidence.')}
      </div>
      <div>
        <div class="card"><h2>Threads <span class="hint">share of samples</span></h2>${threadBars(sum.threads)}</div>
        <div class="card"><h2>Modules <span class="hint">self share</span></h2>${moduleTable(sum.modules)}</div>
      </div>
    </section>`;
  bindFnLinks(body, ctx);
}

function benchmarkBlock(b) {
  return `<div class="card"><h2>Benchmark <span class="hint">${b.wall_seconds.length} unprofiled runs, ${b.warmup_runs} warm-up${b.failed_runs ? `, ${b.failed_runs} failed` : ''}</span></h2>
    <table class="tbl"><thead><tr><th></th><th class="num">median</th><th class="num">95% CI (bootstrap)</th><th class="num">MAD</th><th class="num">min</th><th class="num">max</th></tr></thead><tbody>
    ${['wall', 'cpu'].map((k) => `<tr><td>${k === 'wall' ? 'Wall' : 'CPU'}</td><td class="num">${seconds(b[k].median)}</td>
      <td class="num">${seconds(b[k].ci95_low)} – ${seconds(b[k].ci95_high)}</td><td class="num">${seconds(b[k].mad)}</td>
      <td class="num">${seconds(b[k].min)}</td><td class="num">${seconds(b[k].max)}</td></tr>`).join('')}
    </tbody></table>${dotPlot(b.wall_seconds)}</div>`;
}

function dotPlot(values) {
  if (!values.length) return '';
  const min = Math.min(...values), max = Math.max(...values);
  const span = max - min || max || 1;
  const w = 600, hgt = 46;
  const dots = values.map((v, i) => `<circle cx="${10 + ((v - min) / span) * (w - 20)}" cy="${8 + (i % 3) * 8}" r="4"><title>run ${i + 1}: ${seconds(v)}</title></circle>`).join('');
  return `<svg class="dotplot" viewBox="0 0 ${w} ${hgt}" role="img" aria-label="Wall time of each benchmark run">${dots}
    <text x="10" y="${hgt - 1}">${seconds(min)}</text><text x="${w - 10}" y="${hgt - 1}" text-anchor="end">${seconds(max)}</text></svg>`;
}

function findingCard(f, ctx) {
  const fnLinks = f.source === 'sampling' && !f.id.startsWith('instr')
    ? (f.affected_functions || []).map((id, i) => `<button class="link fn-link mono" data-fn="${h(id)}">${h(shortName(f.affected_names?.[i] || id, 60))}</button>`).join(', ')
    : (f.affected_names || []).map((n) => `<code>${h(shortName(n, 60))}</code>`).join(', ');
  return `<article class="finding card sev-border-${h(f.severity)}">
    <header>${sevBadge(f.severity)} <span class="conf">confidence: ${h(f.confidence)}</span> <span class="score" title="100 × impact × evidence weight">score ${f.score.toFixed(1)}</span> <span class="src">${h(f.source)}</span></header>
    <h3>${h(f.title)}</h3>
    ${fnLinks ? `<p class="affected">Affected: ${fnLinks}</p>` : ''}
    <div class="three">
      <div><h4>Observed facts</h4><ul>${f.facts.map((x) => `<li>${h(x)}</li>`).join('')}</ul></div>
      <div><h4>Interpretation <span class="hint">heuristic</span></h4><p>${h(f.interpretation)}</p></div>
      <div><h4>Next actions</h4><ul>${f.next_actions.map((x) => `<li>${h(x)}</li>`).join('')}</ul></div>
    </div>
    ${f.measurements?.length ? `<details><summary>Measurements</summary><table class="tbl compact">${f.measurements.map((m) => `<tr><td class="mono">${h(m.name)}</td><td class="num">${fmtMeasure(m)}</td><td class="muted">${h(m.unit)}</td></tr>`).join('')}</table></details>` : ''}
    ${f.recommendations?.length ? f.recommendations.map(recBlock).join('') : ''}
    ${f.limitations?.length ? `<details><summary>Limitations</summary><ul>${f.limitations.map((x) => `<li>${h(x)}</li>`).join('')}</ul></details>` : ''}
  </article>`;
}

function fmtMeasure(m) {
  if (m.unit === 'fraction' || m.unit === 'ratio') return pct(m.value);
  if (m.unit === 's') return seconds(m.value);
  if (m.unit === 'ns') return nanos(m.value);
  if (m.unit === 'bytes') return bytes(m.value);
  return Number.isInteger(m.value) ? count(m.value) : Number(m.value).toPrecision(4);
}

function recBlock(r) {
  return `<details class="rec"><summary>Recommendation: ${h(r.title)}</summary><dl class="rec-grid">
    <dt>Observed</dt><dd>${h(r.observed)}</dd>
    <dt>Why it may matter</dt><dd>${h(r.why_it_matters)}</dd>
    <dt>Change to investigate</dt><dd>${h(r.change_to_investigate)}</dd>
    <dt>Expected benefit</dt><dd>${h(r.expected_benefit)} <span class="hint">(qualitative — measure it)</span></dd>
    <dt>Trade-offs</dt><dd>${h(r.tradeoffs)}</dd>
    <dt>How to validate</dt><dd>${h(r.how_to_validate)}</dd>
    <dt>Would disprove it</dt><dd>${h(r.would_disprove)}</dd></dl></details>`;
}

function threadBars(threads) {
  if (!threads?.length) return empty('No sampling data.');
  return `<div class="bars">${threads.slice(0, 16).map((t) => `<div class="bar-row" title="${h(t.name)} (tid ${t.tid}): ${count(t.samples)} samples ≈ ${seconds(t.cpu_seconds_est)} CPU">
    <span class="bar-label mono"><span class="muted">${t.tid}</span> ${h(t.name)}</span>
    <span class="bar"><span style="width:${(t.share * 100).toFixed(2)}%"></span></span><span class="num">${pct(t.share)}</span></div>`).join('')}</div>`;
}

function moduleTable(mods) {
  if (!mods?.length) return empty('No sampling data.');
  return `<table class="tbl compact">${mods.slice(0, 10).map((m) => `<tr><td class="mono">${h(m.name)}</td><td class="num">${pct(m.self_share)}</td></tr>`).join('')}</table>`;
}

function bindFnLinks(root, ctx) {
  root.querySelectorAll('.fn-link').forEach((b) => b.addEventListener('click', () => openFunction(ctx, b.dataset.fn)));
}

// -------- flame graph tab
async function tabFlame(body, ctx, params) {
  const sum = await api.summary(ctx.id);
  if (!sum.sampling) { body.innerHTML = empty('This session has no sampling profile, so there is no flame graph.'); return; }
  const threads = sum.threads || [];
  const modules = sum.modules || [];
  body.innerHTML = `
    <div class="flame-controls card">
      <label>View <select id="fg-view"><option value="">Inclusive (callers → callees)</option><option value="inverted">Self cost (inverted: hot functions → their callers)</option></select></label>
      ${api.demo ? '' : `<label>Thread <select id="fg-thread"><option value="">All threads</option>${threads.map((t) => `<option value="${t.tid}">${h(t.name)} (${t.tid}) — ${pct(t.share)}</option>`).join('')}</select></label>
      <label>Module <select id="fg-module"><option value="">All modules</option>${modules.map((m) => `<option value="${h(m.name)}">${h(m.name)}</option>`).join('')}</select></label>`}
      <label>Search <input id="fg-search" type="search" placeholder="function name" autocomplete="off"></label>
      <label>Highlight self ≥ <select id="fg-threshold"><option value="0">off</option><option value="0.01">1%</option><option value="0.05" selected>5%</option><option value="0.1">10%</option><option value="0.25">25%</option></select></label>
    </div>
    <div class="flame-legend muted">Width = share of samples (estimate). Warm = application code, cool grey-blue = libraries. Yellow outline = self cost above threshold. <span id="fg-match"></span></div>
    <nav id="fg-crumbs" class="crumbs" aria-label="Zoom path"></nav>
    <div class="flame-wrap card"><canvas id="fg-canvas"></canvas><div id="fg-tip" class="tooltip" hidden></div></div>
    <div id="fg-meta" class="muted small"></div>
    <div class="card"><h2>Functions</h2><input id="fg-table-search" type="search" placeholder="filter functions" class="wide"><div id="fg-table"></div></div>`;

  const canvas = $('#fg-canvas');
  const tip = $('#fg-tip');
  let total = 1;
  const fg = new FlameGraph(canvas, {
    onHover: (node, e) => {
      if (!node) { tip.hidden = true; return; }
      tip.hidden = false;
      tip.innerHTML = `<div class="mono strong">${h(node.name)}</div><div class="muted">${h(node.module || '')}</div>
        <div>inclusive: ${count(node.value)} samples · ${pct(node.value / total)}</div>
        <div>self: ${count(node.self)} samples · ${pct(node.self / total)}</div>
        <div class="muted">of zoom root: ${pct(node.value / (fg.zoom.value || 1))}</div>`;
      if (e) {
        const x = Math.min(e.clientX + 14, window.innerWidth - tip.offsetWidth - 4);
        const y = e.clientY + 16 + tip.offsetHeight > window.innerHeight ? e.clientY - tip.offsetHeight - 8 : e.clientY + 16;
        tip.style.left = Math.max(4, x) + 'px';
        tip.style.top = Math.max(4, y) + 'px';
      }
    },
    onSelect: (node) => { if (node.id) openFunction(ctx, node.id); },
    onZoom: (node) => renderCrumbs(node),
  });
  onCleanup(() => fg.destroy());

  function renderCrumbs(node) {
    const path = [];
    for (let n = node; n; n = n.parent) path.unshift(n);
    $('#fg-crumbs').innerHTML = path.map((n, i) => `<button class="crumb mono" data-i="${i}">${h(shortName(n.name, 40))}</button>`).join('<span class="sep">›</span>');
    $('#fg-crumbs').querySelectorAll('.crumb').forEach((b) => b.addEventListener('click', () => fg.setZoom(path[Number(b.dataset.i)])));
  }

  async function load() {
    const v = $('#fg-view').value;
    fg.orientation = v === 'inverted' ? 'down' : 'up';
    const data = await api.flamegraph(ctx.id, { view: v, tid: $('#fg-thread')?.value || '', module: $('#fg-module')?.value || '' });
    total = data.root.value || 1;
    fg.setData(data.root);
    renderCrumbs(fg.root);
    $('#fg-meta').textContent = `${count(data.total)} samples · ${count(data.nodes)} frames drawn` +
      (data.pruned ? ` · ${count(data.pruned)} subtrees below ${pct(data.min_fraction, 2)} omitted` : '');
    applySearch();
  }
  function applySearch() {
    const q = $('#fg-search').value.trim().toLowerCase();
    fg.setSearch(q);
    $('#fg-match').textContent = q && fg.root ? `Matched: ${pct(matchedShare(fg.root, (n) => n.name.toLowerCase().includes(q)))} of samples.` : '';
  }
  $('#fg-view').addEventListener('change', load);
  $('#fg-thread')?.addEventListener('change', load);
  $('#fg-module')?.addEventListener('change', load);
  $('#fg-search').addEventListener('input', applySearch);
  $('#fg-threshold').addEventListener('change', (e) => fg.setThreshold(Number(e.target.value)));
  fg.setThreshold(0.05);
  await load();

  const renderTable = async () => {
    const q = $('#fg-table-search').value.trim();
    const fr = await api.functions(ctx.id, { q, limit: 100, tid: $('#fg-thread')?.value || '' });
    $('#fg-table').innerHTML = functionRows(fr);
    bindFnLinks($('#fg-table'), ctx);
    $('#fg-table').querySelectorAll('tr[data-name]').forEach((tr) => tr.addEventListener('mouseenter', () => {
      $('#fg-search').value = tr.dataset.name;
      applySearch();
    }));
  };
  let deb;
  $('#fg-table-search').addEventListener('input', () => { clearTimeout(deb); deb = setTimeout(renderTable, 150); });
  $('#fg-thread')?.addEventListener('change', renderTable);
  await renderTable();
  if (params.get('q')) { $('#fg-search').value = params.get('q'); applySearch(); }
}

function functionRows(fr, sortable = false, sort = 'self') {
  if (!fr.functions.length) return empty('No matching functions.');
  const th = (key, label) => sortable ? `<th class="num"><button class="link sort ${sort === key ? 'active' : ''}" data-sort="${key}">${label}${sort === key ? ' ▼' : ''}</button></th>` : `<th class="num">${label}</th>`;
  return `<table class="tbl fn-table"><thead><tr>${th('self', 'Self')}${th('total', 'Inclusive')}<th class="num">Self samples</th>
    <th class="num">Self time (est.)</th>${sortable ? '<th>Function <button class="link sort" data-sort="name">A–Z</button></th>' : '<th>Function</th>'}<th>Module</th><th>Source</th></tr></thead><tbody>
    ${fr.functions.map((f) => `<tr data-name="${h(f.name)}">
      <td class="num"><span class="minibar" style="--w:${(f.self_share * 100).toFixed(1)}%"></span>${pct(f.self_share)}</td>
      <td class="num">${pct(f.total_share)}</td><td class="num">${count(f.self_samples)}</td><td class="num">${seconds(f.self_seconds_est)}</td>
      <td class="mono"><button class="link fn-link" data-fn="${h(f.id)}" title="${h(f.name)}">${h(shortName(f.name, 90))}</button>${f.resolved ? '' : ' <span class="badge muted" title="No symbol: shown as module+offset">unresolved</span>'}</td>
      <td class="muted mono">${h(f.module)}</td><td class="muted mono small">${f.source_file ? h(basename(f.source_file)) + ':' + f.source_line : ''}</td></tr>`).join('')}
    </tbody></table><p class="muted small">${count(fr.matched)} functions matched · ${h(fr.units || '')}</p>`;
}

// -------- functions tab
async function tabFunctions(body, ctx, params) {
  let sort = params.get('sort') || 'self';
  body.innerHTML = `<div class="card"><div class="row tight"><input id="fn-search" type="search" placeholder="Search function or module" class="wide" value="${h(params.get('q') || '')}"></div><div id="fn-table">${loading()}</div></div>`;
  const render = async () => {
    const fr = await api.functions(ctx.id, { sort, q: $('#fn-search').value.trim(), limit: 500 });
    $('#fn-table').innerHTML = functionRows(fr, true, sort);
    bindFnLinks($('#fn-table'), ctx);
    $('#fn-table').querySelectorAll('.sort').forEach((b) => b.addEventListener('click', () => { sort = b.dataset.sort; render(); }));
  };
  let deb;
  $('#fn-search').addEventListener('input', () => { clearTimeout(deb); deb = setTimeout(() => render().catch((e) => { $('#fn-table').innerHTML = errorBox(e); }), 150); });
  try { await render(); } catch (e) { $('#fn-table').innerHTML = errorBox(e, 'No function data'); }
}

// -------- function detail panel
async function openFunction(ctx, fid) {
  const panel = $('#fn-panel');
  if (!panel) return;
  panel.hidden = false;
  panel.innerHTML = loading();
  try {
    const f = await api.func(ctx.id, fid);
    const list = (items) => items.length ? `<table class="tbl compact">${items.map((x) => `<tr><td class="num">${pct(x.share)}</td><td class="mono"><button class="link fn-link" data-fn="${h(x.id)}">${h(shortName(x.name, 60))}</button></td></tr>`).join('')}</table>` : empty('none observed');
    panel.innerHTML = `<header><h2 class="mono">${h(shortName(f.name, 120))}</h2><button class="btn ghost" id="fn-close" aria-label="Close">✕</button></header>
      <p class="muted mono small">${h(f.module)}${f.source_file ? ' · ' + h(f.source_file) + ':' + f.source_line : ' · source location unavailable'}</p>
      <div class="cards small-cards">
        <div class="card stat"><span class="label">Self</span><span class="value">${pct(f.self_share)}</span><span class="foot">${count(f.self_samples)} samples ≈ ${seconds(f.self_seconds_est)}</span></div>
        <div class="card stat"><span class="label">Inclusive</span><span class="value">${pct(f.total_share)}</span><span class="foot">${count(f.total_samples)} samples ≈ ${seconds(f.total_seconds_est)}</span></div>
        <div class="card stat"><span class="label">Category</span><span class="value small-value">${h(f.category)}</span><span class="foot">max recursion ${f.max_recursion_depth}</span></div>
      </div>
      <h3>Callers <span class="hint">share of all samples</span></h3>${list(f.callers)}
      <h3>Callees</h3>${list(f.callees)}
      <h3>Threads</h3>${f.threads.length ? `<table class="tbl compact">${f.threads.map((t) => `<tr><td class="mono">${h(t.name)} ${t.tid}</td><td class="num">${count(t.samples)}</td></tr>`).join('')}</table>` : empty('—')}
      <h3>Related findings</h3>${f.related_findings.length ? f.related_findings.map((x) => findingCard(x, ctx)).join('') : empty('No findings reference this function.')}
      <p class="muted small">Sampling estimates: shares reflect on-CPU time only.</p>`;
    $('#fn-close').addEventListener('click', () => { panel.hidden = true; });
    bindFnLinks(panel, ctx);
    panel.focus();
  } catch (e) {
    panel.innerHTML = `<header><h2>Function</h2><button class="btn ghost" id="fn-close">✕</button></header>${errorBox(e)}`;
    $('#fn-close').addEventListener('click', () => { panel.hidden = true; });
  }
}

// -------- instrumentation tab
async function tabInstrumentation(body, ctx) {
  let r;
  try { r = await api.instrumentation(ctx.id); } catch (e) {
    body.innerHTML = empty(e.message + ' — instrumentation needs a target linked with detective_instr and run in instrumentation mode.');
    return;
  }
  const totalSelf = r.scopes.reduce((a, s) => a + s.self_ns, 0) || 1;
  body.innerHTML = `<div class="card"><h2>Instrumented scopes <span class="hint">${h(r.clock)} · ${r.threads} thread(s) · clock overhead ≈ ${nanos(r.clock_overhead_ns)} per scope</span></h2>
    <table class="tbl"><thead><tr><th>Scope</th><th class="num">Calls</th><th class="num">Self share</th><th class="num">Total</th><th class="num">Self</th>
    <th class="num">Mean</th><th class="num">Min</th><th class="num">p50</th><th class="num">p90</th><th class="num">p99</th><th class="num">Max</th><th class="num">Threads</th></tr></thead><tbody>
    ${r.scopes.map((s) => `<tr><td class="mono">${h(s.name)}</td><td class="num">${count(s.count)}</td>
      <td class="num"><span class="minibar" style="--w:${(s.self_ns / totalSelf * 100).toFixed(1)}%"></span>${pct(s.self_ns / totalSelf)}</td>
      <td class="num">${nanos(s.total_ns)}</td><td class="num">${nanos(s.self_ns)}</td><td class="num">${nanos(s.mean_ns)}</td><td class="num">${nanos(s.min_ns)}</td>
      ${s.percentiles_valid ? `<td class="num">${nanos(s.p50_ns)}</td><td class="num">${nanos(s.p90_ns)}</td><td class="num">${s.p99_ns >= 0 ? nanos(s.p99_ns) : '<span class="muted" title="needs ≥1000 durations">n/a</span>'}</td>`
        : '<td class="num muted" colspan="3" title="needs ≥100 retained durations">too few calls</td>'}
      <td class="num">${nanos(s.max_ns)}</td><td class="num">${Object.keys(s.threads).length}</td></tr>`).join('')}
    </tbody></table>
    <p class="muted small">Totals are summed across threads and measure wall-clock time (including waiting). They are kept separate from sampling estimates.</p></div>
    <div class="grid-2"><div class="card"><h2>Parent → child</h2><table class="tbl compact">${r.edges.map((e) => `<tr><td class="mono">${h(e.parent)} → ${h(e.child)}</td><td class="num">${count(e.count)}</td><td class="num">${nanos(e.total_ns)}</td></tr>`).join('')}</table></div>
    <div class="card"><h2>Marks</h2>${Object.keys(r.marks).length ? `<table class="tbl compact">${Object.entries(r.marks).map(([k, v]) => `<tr><td class="mono">${h(k)}</td><td class="num">${count(v)}</td></tr>`).join('')}</table>` : empty('No DETECTIVE_MARK counters.')}</div></div>`;
}

// -------- memory tab
async function tabMemory(body, ctx) {
  let m;
  try { m = await api.memory(ctx.id); } catch (e) {
    body.innerHTML = empty(e.message + ' — enable “Track heap allocations” when starting a session.');
    return;
  }
  const hist = m.size_histogram;
  const maxH = Math.max(1, ...hist);
  body.innerHTML = `<section class="cards">
      <div class="card stat"><span class="label">Heap allocations</span><span class="value">${count(m.total_allocations)}</span><span class="foot">malloc ${count(m.malloc_calls)} · new ${count(m.new_calls)} · calloc ${count(m.calloc_calls)} · realloc ${count(m.realloc_calls)}</span></div>
      <div class="card stat"><span class="label">Frees</span><span class="value">${count(m.free_calls)}</span></div>
      <div class="card stat"><span class="label">Allocation volume</span><span class="value">${bytes(m.bytes_requested)}</span><span class="foot">${h(m.definitions.bytes_requested)}</span></div>
      <div class="card stat"><span class="label">Peak live heap</span><span class="value">${bytes(m.peak_live_bytes)}</span><span class="foot">${h(m.definitions.peak_live_bytes)}${m.peak_error_bytes_per_thread ? ` · accurate to ±${bytes(m.peak_error_bytes_per_thread)} per thread` : ''}</span></div>
      <div class="card stat"><span class="label">Peak RSS</span><span class="value">${bytes(m.max_rss_kb * 1024)}</span><span class="foot">${h(m.definitions.max_rss_kb)}</span></div>
    </section>
    <div class="grid-2 wide-left">
      <div class="card"><h2>Allocation call sites <span class="hint">immediate caller of malloc / operator new</span></h2>
        <table class="tbl"><thead><tr><th class="num">Allocations</th><th class="num">Bytes</th><th>Function</th><th>Module</th></tr></thead><tbody>
        ${m.sites.slice(0, 50).map((s) => `<tr><td class="num">${count(s.count)}</td><td class="num">${bytes(s.bytes)}</td><td class="mono">${h(shortName(s.function, 90))}</td><td class="muted mono">${h(s.module)}</td></tr>`).join('')}
        </tbody></table>${m.sites_overflowed ? `<p class="muted small">${count(m.sites_overflowed)} allocations had call sites that did not fit the site table.</p>` : ''}</div>
      <div class="card"><h2>Allocation sizes</h2><div class="bars">${hist.map((c, i) => c ? `<div class="bar-row"><span class="bar-label mono">${bytes(2 ** i)}–${bytes(2 ** (i + 1))}</span><span class="bar"><span style="width:${(c / maxH * 100).toFixed(1)}%"></span></span><span class="num">${count(c)}</span></div>` : '').join('')}</div></div>
    </div><p class="muted small">${h(m.scope_note)}</p>`;
}

// -------- output tab
async function tabOutput(body, ctx) {
  const o = await api.output(ctx.id);
  body.innerHTML = `<div class="card"><h2>stdout ${o.stdout_truncated ? '<span class="badge warn">truncated</span>' : ''}</h2><pre class="out">${h(o.stdout) || '<span class="muted">(empty)</span>'}</pre></div>
    <div class="card"><h2>stderr ${o.stderr_truncated ? '<span class="badge warn">truncated</span>' : ''}</h2><pre class="out">${h(o.stderr) || '<span class="muted">(empty)</span>'}</pre></div>`;
}

// ----------------------------------------------------------------- compare
async function renderCompare(params) {
  view.innerHTML = loading('Loading sessions');
  const { sessions } = await api.sessions();
  const usable = sessions.filter((s) => s.summary);
  const a = params.get('a') || '';
  const b = params.get('b') || '';
  const opt = (sel) => usable.map((s) => `<option value="${h(s.id)}" ${s.id === sel ? 'selected' : ''}>${h(basename(s.config.executable))} ${h((s.config.args || []).join(' ').slice(0, 40))} ${s.config.label ? '· ' + h(s.config.label) : ''} — ${time(s.created_ms)}</option>`).join('');
  view.innerHTML = `<section class="page-head"><div><h1>Compare runs</h1><p class="sub">Baseline vs candidate. Timing verdicts need ≥5 benchmark runs per session; function deltas compare sampling shares.</p></div></section>
    <form id="cmp-form" class="card row">
      <label>Baseline<select name="a" required><option value="">choose…</option>${opt(a)}</select></label>
      <label>Candidate<select name="b" required><option value="">choose…</option>${opt(b)}</select></label>
      <div class="actions"><button class="btn primary" type="submit">Compare</button></div></form>
    <section id="cmp-body">${a && b ? loading('Comparing') : empty('Choose two sessions.')}</section>`;
  $('#cmp-form').addEventListener('submit', (e) => {
    e.preventDefault();
    const fd = new FormData(e.target);
    location.hash = `#/compare?a=${encodeURIComponent(fd.get('a'))}&b=${encodeURIComponent(fd.get('b'))}`;
  });
  if (!(a && b)) return;
  try {
    const c = await api.compare(a, b);
    const verdictCls = { regression: 'bad', improvement: 'ok', no_significant_change: 'muted', inconclusive: 'warn' };
    const maxAbs = Math.max(0.01, ...c.functions.map((f) => Math.abs(f.delta_self_share)));
    $('#cmp-body').innerHTML = `<div class="card"><h2>Timing</h2><table class="tbl"><thead><tr><th>Metric</th><th class="num">Baseline (median, n)</th><th class="num">Candidate (median, n)</th>
      <th class="num">Absolute</th><th class="num">Relative</th><th class="num">p-value</th><th>Verdict</th></tr></thead><tbody>
      ${c.metrics.map((m) => `<tr><td>${m.metric === 'wall_seconds' ? 'Wall-clock' : 'CPU time'}</td>
        <td class="num">${seconds(m.baseline_median)} <span class="muted">n=${m.n_baseline}</span><div class="muted small">CI ${seconds(m.baseline_ci[0])}–${seconds(m.baseline_ci[1])}</div></td>
        <td class="num">${seconds(m.candidate_median)} <span class="muted">n=${m.n_candidate}</span><div class="muted small">CI ${seconds(m.candidate_ci[0])}–${seconds(m.candidate_ci[1])}</div></td>
        <td class="num">${(m.absolute_change >= 0 ? '+' : '−') + seconds(Math.abs(m.absolute_change))}</td><td class="num">${signedPct(m.relative_change)}</td>
        <td class="num">${m.test_valid ? m.p_value.toPrecision(3) : '—'}</td>
        <td><span class="badge ${verdictCls[m.verdict] || 'muted'}">${h(m.verdict.replaceAll('_', ' '))}</span><div class="muted small">${h(m.explanation)}</div></td></tr>`).join('')}
      </tbody></table></div>
      <div class="card"><h2>Function cost changes <span class="hint">self share, percentage points (candidate − baseline)</span></h2>
      ${c.functions.length ? `<div class="diverging">${c.functions.filter((f) => Math.abs(f.delta_self_share) >= 0.0005 || Math.abs(f.candidate_total_share - f.baseline_total_share) >= 0.005).slice(0, 30).map((f) => {
        const w = Math.abs(f.delta_self_share) / maxAbs * 50;
        const cls = f.delta_self_share > 0 ? 'up' : 'down';
        return `<div class="dv-row" title="${h(f.name)}: ${pct(f.baseline_self_share)} → ${pct(f.candidate_self_share)} (p=${f.p_value.toPrecision(2)})">
          <span class="dv-name mono">${h(shortName(f.name, 60))}</span>
          <span class="dv-track"><span class="dv-bar ${cls}" style="${f.delta_self_share > 0 ? 'left:50%' : `left:${50 - w}%`};width:${w}%"></span><span class="dv-axis"></span></span>
          <span class="num">${pp(f.delta_self_share)} ${f.significant ? '<span class="badge ok" title="significant after Bonferroni correction">sig.</span>' : ''}</span>
          <span class="num muted small" title="inclusive share, baseline → candidate">incl. ${pct(f.baseline_total_share)} → ${pct(f.candidate_total_share)}</span></div>`;
      }).join('')}</div>` : empty('Function-level comparison needs sampling profiles in both sessions.')}
      </div>
      <div class="card"><ul class="muted small">${c.notes.map((n) => `<li>${h(n)}</li>`).join('')}</ul></div>`;
  } catch (e) {
    $('#cmp-body').innerHTML = errorBox(e, 'Comparison failed');
  }
}

window.addEventListener('hashchange', route);
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') { const p = $('#fn-panel'); if (p && !p.hidden) p.hidden = true; }
});
route();
