// Formatting helpers (pure; unit-tested).

export function pct(fraction, digits = 1) {
  if (fraction == null || Number.isNaN(fraction)) return '—';
  return (fraction * 100).toFixed(digits) + '%';
}

export function seconds(s) {
  if (s == null || Number.isNaN(s)) return '—';
  if (s === 0) return '0 s';
  if (s < 1e-6) return (s * 1e9).toFixed(0) + ' ns';
  if (s < 1e-3) return (s * 1e6).toFixed(1) + ' µs';
  if (s < 1) return (s * 1e3).toFixed(1) + ' ms';
  return s.toFixed(3) + ' s';
}

export function nanos(ns) { return seconds(ns / 1e9); }

export function bytes(b) {
  if (b == null) return '—';
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  let i = 0;
  let v = b;
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return (i === 0 ? v.toFixed(0) : v.toFixed(2)) + ' ' + units[i];
}

export function count(n) {
  if (n == null) return '—';
  return Number(n).toLocaleString('en-US');
}

export function signedPct(fraction, digits = 1) {
  if (fraction == null) return '—';
  const s = (fraction * 100).toFixed(digits);
  return (fraction > 0 ? '+' : '') + s + '%';
}

export function pp(fraction) {
  return (fraction > 0 ? '+' : '') + (fraction * 100).toFixed(1) + ' pp';
}

export function time(ms) {
  if (!ms) return '—';
  return new Date(ms).toLocaleString();
}

export function basename(path) {
  if (!path) return '';
  const i = path.lastIndexOf('/');
  return i >= 0 ? path.slice(i + 1) : path;
}

/** Shortens long C++ names, keeping the most identifying part. */
export function shortName(name, max = 80) {
  if (!name || name.length <= max) return name || '';
  return name.slice(0, max - 1) + '…';
}

export function escapeHtml(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

/** Parses "KEY=VALUE" lines into an object; returns {env, errors}. */
export function parseEnvLines(text) {
  const env = {};
  const errors = [];
  (text || '').split('\n').forEach((raw, i) => {
    const line = raw.trim();
    if (!line) return;
    const eq = line.indexOf('=');
    if (eq <= 0) { errors.push(`line ${i + 1}: expected KEY=VALUE`); return; }
    const k = line.slice(0, eq);
    if (!/^[A-Za-z0-9_]+$/.test(k)) { errors.push(`line ${i + 1}: invalid name "${k}"`); return; }
    env[k] = line.slice(eq + 1);
  });
  return { env, errors };
}

/** One argument per line — explicit, never shell-split. */
export function parseArgLines(text) {
  return (text || '').split('\n').map((l) => l.replace(/\r$/, '')).filter((l) => l.length > 0);
}
