// Flame graph: pure layout (unit-tested with node --test) + a Canvas renderer.
//
// Width of a frame = its inclusive sample count relative to the zoom root.
// Vertical position = call-stack depth. Colour only distinguishes functions
// (application vs. library); it never encodes a measurement.

/**
 * Lays out a tree of {name, value, self, children} nodes.
 * Returns rects with x/width in [0,1] relative to `zoom`, plus depth.
 * Nodes narrower than `minWidth` (fraction) are skipped with their subtrees.
 */
export function layoutFlame(root, zoom = root, minWidth = 0) {
  const rects = [];
  if (!zoom || !zoom.value) return { rects, maxDepth: 0, total: 0 };
  const total = zoom.value;
  let maxDepth = 0;
  const stack = [{ node: zoom, depth: 0, x: 0 }];
  while (stack.length) {
    const { node, depth, x } = stack.pop();
    const width = node.value / total;
    rects.push({ node, depth, x, width });
    if (depth > maxDepth) maxDepth = depth;
    let cx = x;
    const kids = [];
    for (const c of node.children || []) {
      const cw = c.value / total;
      if (cw >= minWidth) kids.push({ node: c, depth: depth + 1, x: cx });
      cx += cw;
    }
    for (let i = kids.length - 1; i >= 0; i--) stack.push(kids[i]);
  }
  return { rects, maxDepth, total };
}

/** Annotates every node with a `parent` link (non-enumerable) for breadcrumbs. */
export function linkParents(root) {
  const stack = [root];
  Object.defineProperty(root, 'parent', { value: null, enumerable: false, writable: true });
  while (stack.length) {
    const n = stack.pop();
    for (const c of n.children || []) {
      Object.defineProperty(c, 'parent', { value: n, enumerable: false, writable: true });
      stack.push(c);
    }
  }
  return root;
}

/** Path from the root to `node` (inclusive). */
export function pathTo(node) {
  const path = [];
  for (let n = node; n; n = n.parent) path.unshift(n);
  return path;
}

/** Sum of `value` over nodes whose name matches, counting each sample once (no double count under recursion). */
export function matchedShare(root, predicate) {
  let matched = 0;
  const walk = (n, inside) => {
    const hit = !inside && n !== root && predicate(n);
    if (hit) matched += n.value;
    for (const c of n.children || []) walk(c, inside || hit);
  };
  walk(root, false);
  return root.value ? matched / root.value : 0;
}

function hash(str) {
  let h = 2166136261;
  for (let i = 0; i < str.length; i++) { h ^= str.charCodeAt(i); h = Math.imul(h, 16777619); }
  return h >>> 0;
}

export function frameColor(node) {
  if (node.name === 'all') return '#4b5563';
  const h = hash(node.name);
  const lib = (node.module || '').includes('.so') || (node.module || '').startsWith('[');
  if (lib) return `hsl(${200 + (h % 40)}, ${18 + (h >> 8) % 15}%, ${42 + (h >> 16) % 12}%)`;
  return `hsl(${12 + (h % 32)}, ${62 + (h >> 8) % 20}%, ${48 + (h >> 16) % 10}%)`;
}

export class FlameGraph {
  constructor(canvas, { onHover, onSelect, onZoom, orientation = 'up' } = {}) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.onHover = onHover || (() => {});
    this.onSelect = onSelect || (() => {});
    this.onZoom = onZoom || (() => {});
    this.orientation = orientation; // 'up' = root at bottom (flame), 'down' = icicle
    this.rowHeight = 18;
    this.root = null;
    this.zoom = null;
    this.search = '';
    this.threshold = 0; // highlight frames whose self share >= threshold (fraction of root)
    this.focused = null;
    this.layout = { rects: [], maxDepth: 0, total: 0 };
    canvas.tabIndex = 0;
    canvas.setAttribute('role', 'application');
    canvas.setAttribute('aria-label', 'Flame graph. Arrow keys move between frames, Enter zooms, Escape resets zoom.');
    canvas.addEventListener('mousemove', (e) => this._hover(e));
    canvas.addEventListener('mouseleave', () => { this.hovered = null; this.onHover(null); this.draw(); });
    canvas.addEventListener('click', (e) => {
      const r = this._hit(e);
      if (r) { this.focused = r.node; this.setZoom(r.node); this.onSelect(r.node); }
    });
    canvas.addEventListener('keydown', (e) => this._key(e));
    this._resizeObserver = new ResizeObserver(() => this.draw());
    this._resizeObserver.observe(canvas.parentElement);
  }

  setData(root) {
    this.root = linkParents(root);
    this.zoom = this.root;
    this.focused = null;
    this.draw();
  }

  setZoom(node) {
    this.zoom = node || this.root;
    this.onZoom(this.zoom);
    this.draw();
  }

  setSearch(text) { this.search = (text || '').toLowerCase(); this.draw(); }
  setThreshold(t) { this.threshold = t; this.draw(); }

  _rowY(depth, height) {
    return this.orientation === 'up' ? height - (depth + 1) * this.rowHeight : depth * this.rowHeight;
  }

  draw() {
    if (!this.root) return;
    const parent = this.canvas.parentElement;
    const width = Math.max(200, parent.clientWidth);
    const minWidth = 0.5 / width;
    this.layout = layoutFlame(this.root, this.zoom, minWidth);
    const height = (this.layout.maxDepth + 1) * this.rowHeight + 2;
    const dpr = window.devicePixelRatio || 1;
    this.canvas.width = Math.floor(width * dpr);
    this.canvas.height = Math.floor(height * dpr);
    this.canvas.style.width = width + 'px';
    this.canvas.style.height = height + 'px';
    const ctx = this.ctx;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, width, height);
    ctx.font = '11px "JetBrains Mono", ui-monospace, SFMono-Regular, Menlo, monospace';
    ctx.textBaseline = 'middle';
    const total = this.root.value || 1;
    const searching = this.search.length > 0;
    for (const r of this.layout.rects) {
      const x = r.x * width;
      const w = r.width * width;
      const y = this._rowY(r.depth, height);
      const n = r.node;
      const match = searching && n.name.toLowerCase().includes(this.search);
      const hot = this.threshold > 0 && (n.self || 0) / total >= this.threshold;
      ctx.globalAlpha = searching && !match ? 0.28 : 1;
      ctx.fillStyle = match ? '#c084fc' : frameColor(n);
      ctx.fillRect(x, y, Math.max(w - 0.6, 0.4), this.rowHeight - 1);
      if (hot) {
        ctx.strokeStyle = '#fde047';
        ctx.lineWidth = 2;
        ctx.strokeRect(x + 1, y + 1, Math.max(w - 2.6, 0.4), this.rowHeight - 3);
      }
      if (n === this.focused || n === this.hovered) {
        ctx.strokeStyle = '#ffffff';
        ctx.lineWidth = 1.5;
        ctx.strokeRect(x + 0.5, y + 0.5, Math.max(w - 1.6, 0.4), this.rowHeight - 2);
      }
      if (w > 28) {
        const maxChars = Math.floor((w - 6) / 6.6);
        const label = n.name.length > maxChars ? n.name.slice(0, Math.max(1, maxChars - 1)) + '…' : n.name;
        ctx.fillStyle = '#0b0d10';
        ctx.fillText(label, x + 3, y + this.rowHeight / 2);
      }
    }
    ctx.globalAlpha = 1;
  }

  _hit(e) {
    const rect = this.canvas.getBoundingClientRect();
    const px = e.clientX - rect.left;
    const py = e.clientY - rect.top;
    const width = rect.width;
    const height = rect.height;
    const depth = this.orientation === 'up'
      ? Math.floor((height - py) / this.rowHeight)
      : Math.floor(py / this.rowHeight);
    const fx = px / width;
    // Rects at one depth do not overlap; a linear scan is fast enough for ~20k frames.
    for (const r of this.layout.rects) {
      if (r.depth === depth && fx >= r.x && fx < r.x + r.width) return r;
    }
    return null;
  }

  _hover(e) {
    const r = this._hit(e);
    const node = r ? r.node : null;
    if (node !== this.hovered) {
      this.hovered = node;
      this.draw();
    }
    this.onHover(node, e);
  }

  _key(e) {
    const n = this.focused || this.zoom;
    if (!n) return;
    let next = null;
    const siblings = n.parent ? n.parent.children : [n];
    const i = siblings.indexOf(n);
    const deeper = this.orientation === 'up' ? 'ArrowUp' : 'ArrowDown';
    const shallower = this.orientation === 'up' ? 'ArrowDown' : 'ArrowUp';
    if (e.key === deeper) next = (n.children && n.children[0]) || null;
    else if (e.key === shallower) next = n.parent;
    else if (e.key === 'ArrowLeft') next = siblings[i - 1] || null;
    else if (e.key === 'ArrowRight') next = siblings[i + 1] || null;
    else if (e.key === 'Enter') { this.setZoom(n); this.onSelect(n); e.preventDefault(); return; }
    else if (e.key === 'Escape') { this.setZoom(this.root); e.preventDefault(); return; }
    else return;
    e.preventDefault();
    if (next) {
      this.focused = next;
      this.onHover(next, null);
      this.draw();
    }
  }

  destroy() { this._resizeObserver.disconnect(); }
}
