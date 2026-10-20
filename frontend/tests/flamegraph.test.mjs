import test from 'node:test';
import assert from 'node:assert/strict';
import { layoutFlame, linkParents, pathTo, matchedShare, frameColor } from '../src/flamegraph.js';
import { pct, seconds, bytes, parseEnvLines, parseArgLines, escapeHtml, signedPct } from '../src/format.js';

const tree = () => ({
  name: 'all', value: 10, self: 0, children: [
    { name: 'main', value: 10, self: 0, children: [
      { name: 'a', value: 6, self: 2, children: [{ name: 'x', value: 4, self: 4, children: [] }] },
      { name: 'b', value: 4, self: 4, children: [] },
    ] },
  ],
});

const byName = (l) => Object.fromEntries(l.rects.map((r) => [r.node.name, r]));

test('layout: widths, offsets, depths', () => {
  const l = layoutFlame(tree());
  const m = byName(l);
  assert.equal(l.total, 10);
  assert.equal(l.maxDepth, 3);
  assert.equal(m.main.depth, 1);
  assert.equal(m.a.x, 0);
  assert.equal(m.a.width, 0.6);
  assert.equal(m.b.x, 0.6);
  assert.equal(m.b.width, 0.4);
  assert.equal(m.x.depth, 3);
  assert.equal(m.x.width, 0.4);
});

test('layout: children stay inside parents and leftover equals self', () => {
  const root = { name: 'all', value: 0, self: 0, children: [] };
  let seed = 7;
  const rnd = () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648;
  const build = (node, depth) => {
    if (depth > 5) return;
    const k = Math.floor(rnd() * 4);
    for (let i = 0; i < k; i++) {
      const c = { name: `f${depth}_${i}`, value: 0, self: Math.floor(rnd() * 5), children: [] };
      node.children.push(c);
      build(c, depth + 1);
    }
  };
  build(root, 0);
  const fix = (n) => { n.value = n.self + n.children.reduce((a, c) => a + fix(c), 0); return n.value; };
  if (fix(root) === 0) root.self = root.value = 1;
  const l = layoutFlame(root);
  const rectOf = new Map(l.rects.map((r) => [r.node, r]));
  for (const r of l.rects) {
    let used = 0;
    for (const c of r.node.children) {
      const cr = rectOf.get(c);
      if (!cr) continue;
      assert.equal(cr.depth, r.depth + 1);
      assert.ok(cr.x >= r.x - 1e-12);
      assert.ok(cr.x + cr.width <= r.x + r.width + 1e-9);
      used += cr.width;
    }
    assert.ok(Math.abs(r.width - used - r.node.self / root.value) < 1e-9);
  }
});

test('layout: zoom and pruning', () => {
  const t = tree();
  const a = t.children[0].children[0];
  const z = layoutFlame(t, a);
  assert.equal(z.total, 6);
  assert.equal(z.rects[0].node, a);
  assert.equal(z.rects[0].width, 1);
  assert.equal(byName(z).x.width, 4 / 6);
  const p = layoutFlame(t, t, 0.5);
  assert.deepEqual(p.rects.map((r) => r.node.name).sort(), ['a', 'all', 'main']);
  assert.equal(layoutFlame({ name: 'all', value: 0, children: [] }).rects.length, 0);
});

test('large trees lay out quickly', () => {
  const root = { name: 'all', value: 0, self: 0, children: [] };
  for (let i = 0; i < 200; i++) {
    const c = { name: 'f' + i, value: 0, self: 1, children: [] };
    for (let j = 0; j < 100; j++) c.children.push({ name: `g${i}_${j}`, value: 3, self: 3, children: [] });
    c.value = 1 + 300;
    root.children.push(c);
  }
  root.value = root.children.reduce((a, c) => a + c.value, 0);
  const t0 = performance.now();
  const l = layoutFlame(root);
  const ms = performance.now() - t0;
  assert.equal(l.rects.length, 1 + 200 + 20000);
  assert.ok(ms < 200, `layout of 20k frames took ${ms} ms`);
});

test('parents, paths and matched share (no double counting under recursion)', () => {
  const t = linkParents({ name: 'all', value: 10, children: [
    { name: 'fib', value: 10, children: [{ name: 'fib', value: 8, children: [] }] },
  ] });
  const leaf = t.children[0].children[0];
  assert.deepEqual(pathTo(leaf).map((n) => n.name), ['all', 'fib', 'fib']);
  assert.equal(matchedShare(t, (n) => n.name === 'fib'), 1);
  assert.equal(matchedShare(tree(), (n) => n.name === 'b'), 0.4);
});

test('colors distinguish app vs library frames only', () => {
  assert.match(frameColor({ name: 'main', module: 'app' }), /^hsl\(\d+/);
  assert.notEqual(frameColor({ name: 'x', module: 'libc.so.6' }), frameColor({ name: 'x', module: 'app' }));
  assert.equal(frameColor({ name: 'f', module: 'app' }), frameColor({ name: 'f', module: 'app' }));
});

test('formatting', () => {
  assert.equal(pct(0.1234), '12.3%');
  assert.equal(pct(null), '—');
  assert.equal(seconds(0.0005), '500.0 µs');
  assert.equal(seconds(1.5), '1.500 s');
  assert.equal(bytes(1536), '1.50 KiB');
  assert.equal(signedPct(0.05), '+5.0%');
  assert.equal(escapeHtml('<a href="x">&</a>'), '&lt;a href=&quot;x&quot;&gt;&amp;&lt;/a&gt;');
});

test('argument and environment parsing never shell-splits', () => {
  assert.deepEqual(parseArgLines('--mode\nslow and steady\n\n$(rm -rf /)'), ['--mode', 'slow and steady', '$(rm -rf /)']);
  const { env, errors } = parseEnvLines('A=1\nB=x=y\nbad line\n9X Y=2');
  assert.deepEqual(env, { A: '1', B: 'x=y' });
  assert.equal(errors.length, 2);
});
