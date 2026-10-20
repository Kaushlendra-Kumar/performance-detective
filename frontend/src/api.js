// API client. `DemoApi` serves the explicitly labeled synthetic demo file with the same
// interface, so demo data never flows through (or mixes with) real session storage.

export class ApiError extends Error {
  constructor(status, code, message) {
    super(message);
    this.status = status;
    this.code = code;
  }
}

async function request(method, path, body) {
  const opts = { method, headers: {} };
  if (body !== undefined) {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }
  let res;
  try {
    res = await fetch(path, opts);
  } catch (e) {
    throw new ApiError(0, 'network', 'Cannot reach the Performance Detective server: ' + e.message);
  }
  const text = await res.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch { data = null; }
  if (!res.ok) {
    const err = data && data.error ? data.error : { code: 'http_' + res.status, message: text || res.statusText };
    throw new ApiError(res.status, err.code, err.message);
  }
  return data;
}

const enc = encodeURIComponent;

export const api = {
  demo: false,
  health: () => request('GET', '/api/health'),
  targets: () => request('GET', '/api/targets'),
  sessions: () => request('GET', '/api/sessions'),
  session: (id) => request('GET', `/api/sessions/${enc(id)}`),
  create: (cfg, start = true) => request('POST', '/api/sessions' + (start ? '?start=1' : ''), cfg),
  start: (id) => request('POST', `/api/sessions/${enc(id)}/start`),
  stop: (id) => request('POST', `/api/sessions/${enc(id)}/stop`),
  remove: (id) => request('DELETE', `/api/sessions/${enc(id)}`),
  summary: (id) => request('GET', `/api/sessions/${enc(id)}/summary`),
  output: (id) => request('GET', `/api/sessions/${enc(id)}/output`),
  functions: (id, { sort = 'self', q = '', limit = 300, tid = '' } = {}) =>
    request('GET', `/api/sessions/${enc(id)}/functions?sort=${enc(sort)}&q=${enc(q)}&limit=${limit}&tid=${enc(tid)}`),
  func: (id, fid) => request('GET', `/api/sessions/${enc(id)}/functions/${enc(fid)}`),
  flamegraph: (id, { view = '', tid = '', module = '' } = {}) =>
    request('GET', `/api/sessions/${enc(id)}/flamegraph?view=${enc(view)}&tid=${enc(tid)}&module=${enc(module)}`),
  findings: (id) => request('GET', `/api/sessions/${enc(id)}/findings`),
  instrumentation: (id) => request('GET', `/api/sessions/${enc(id)}/instrumentation`),
  memory: (id) => request('GET', `/api/sessions/${enc(id)}/memory`),
  compare: (a, b) => request('GET', `/api/compare?baseline=${enc(a)}&candidate=${enc(b)}`),
  exportUrl: (id, format) => `/api/sessions/${enc(id)}/export?format=${enc(format)}`,
};

/** Read-only API over the static demo file (frontend/demo/demo-session.json). */
export class DemoApi {
  constructor(data) {
    this.d = data;
    this.demo = true;
    this.id = data.id;
  }
  _check(id) { if (id !== this.id) throw new ApiError(404, 'not_found', 'demo mode has a single session'); }
  async health() { return { status: 'demo', capabilities: { summary: 'DEMO MODE — synthetic data, no profiler attached' } }; }
  async sessions() {
    return { sessions: [{ ...this.d, summary: this.d.result.summary, process: this.d.result.process,
      top_findings: this.d.result.findings.slice(0, 3) }] };
  }
  async session(id) { this._check(id); return { ...this.d, summary: this.d.result.summary, process: this.d.result.process }; }
  async summary(id) {
    this._check(id);
    return { ...this.d.result.summary, process: this.d.result.process, threads: this.d.views.threads, modules: this.d.views.modules };
  }
  async output(id) { this._check(id); return { stdout: this.d.result.process.stdout, stderr: '' }; }
  async functions(id, { sort = 'self', q = '', limit = 300 } = {}) {
    this._check(id);
    let rows = this.d.views.functions.functions.slice();
    if (q) rows = rows.filter((r) => (r.name + ' ' + r.module).toLowerCase().includes(q.toLowerCase()));
    const key = sort === 'total' ? 'total_samples' : 'self_samples';
    if (sort === 'name') rows.sort((a, b) => a.name.localeCompare(b.name));
    else rows.sort((a, b) => b[key] - a[key] || a.id.localeCompare(b.id));
    return { ...this.d.views.functions, matched: rows.length, functions: rows.slice(0, limit) };
  }
  async func(id, fid) {
    this._check(id);
    const f = this.d.views.function_details[fid];
    if (!f) throw new ApiError(404, 'not_found', 'no such function');
    return f;
  }
  async flamegraph(id, { view = '' } = {}) {
    this._check(id);
    return JSON.parse(JSON.stringify(view === 'inverted' ? this.d.views.flamegraph_inverted : this.d.views.flamegraph));
  }
  async findings(id) { this._check(id); return { findings: this.d.result.findings }; }
  async instrumentation() { throw new ApiError(404, 'no_instrumentation', 'not part of the demo'); }
  async memory() { throw new ApiError(404, 'no_memory_data', 'not part of the demo'); }
  exportUrl() { return 'demo/demo-session.json'; }
}
