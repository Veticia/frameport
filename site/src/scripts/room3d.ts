// The Room tab's 3D view in the clickable app (mock.ts): the demo room (public/demo/room.json, written by
// scripts/showcase/site_room.py from the showcase's made-up bedroom, no real scan) drawn on a 2D canvas like the app's
// in-window view: the far walls and the scanned surfaces in blue, furniture in orange, doors and windows in green, the
// play area's middle as an orange arrow. Surfaces facing away are left out, so the near walls open the room up. Drag
// to turn it, click a box to pick it; it sways slowly on its own when left alone (not with reduced motion).

export interface Box { label: string; from?: string; reason: string; cx: number; cz: number; top: number; yaw: number; w: number; d: number; h: number }
export interface RoomData {
  outline: [number, number][];
  floor: number;
  ceiling: number;
  walls: [number, number, number, number, number, number][];
  planes: { label: string; corners: number[] }[];
  boxes: Box[];
  removed: Box[];
  suggestion: Box & { kind: string; detail: string };
  mesh: { v: number[]; t: number[]; raw_from: number; junk: Record<string, [number, number]> };
}
export interface RoomState {
  raw: boolean;           // the scan before the clean-up: what it removes is shown in red
  selected: number | null;
  reveal: number;         // 0..1 while a scan runs (the surfaces appear in order), else 1
  door: 'pending' | 'door' | 'closet' | 'dismissed';
}
export interface RoomView { state: RoomState; host: HTMLElement; redraw(): void; dispose(): void; onPick: (i: number | null) => void }

let cached: Promise<RoomData> | null = null;
export function loadRoom(src: string): Promise<RoomData> {
  cached ??= fetch(src).then((r) => { if (!r.ok) throw new Error(String(r.status)); return r.json() as Promise<RoomData>; });
  cached.catch(() => { cached = null; });
  return cached;
}

type V3 = [number, number, number];
const BLUE = '58, 168, 255', ORANGE = '255, 138, 31', GREEN = '61, 214, 140', RED = '255, 95, 95';

export function boxCorners(b: Box): V3[] {
  const a = (b.yaw * Math.PI) / 180, c = Math.cos(a), s = Math.sin(a);
  const foot = [[-b.w / 2, -b.d / 2], [b.w / 2, -b.d / 2], [b.w / 2, b.d / 2], [-b.w / 2, b.d / 2]]
    .map(([x, z]) => [b.cx + c * x + s * z, b.cz - s * x + c * z]);
  return [...foot.map(([x, z]) => [x, b.top - b.h, z] as V3), ...foot.map(([x, z]) => [x, b.top, z] as V3)];
}
// faces of boxCorners' points with their outward order (counter-clockwise seen from outside)
const BOX_FACES = [[7, 6, 5, 4], [0, 1, 2, 3], [4, 5, 1, 0], [5, 6, 2, 1], [6, 7, 3, 2], [7, 4, 0, 3]];
const BOX_EDGES = [[0, 1], [1, 2], [2, 3], [3, 0], [4, 5], [5, 6], [6, 7], [7, 4], [0, 4], [1, 5], [2, 6], [3, 7]];

export function mountRoom(host: HTMLElement, data: RoomData, init: Partial<RoomState> = {}): RoomView {
  const canvas = document.createElement('canvas');
  canvas.className = 'r3d-canvas';
  host.prepend(canvas);
  const ctx = canvas.getContext('2d')!;
  const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const state: RoomState = { raw: false, selected: null, reveal: 1, door: 'pending', ...init };
  const V = data.mesh.v.map((c) => c / 100);
  const T = data.mesh.t;
  const target: V3 = [0, (data.ceiling - data.floor) * 0.4, 0];
  const BASE_YAW = 0.62, BASE_PITCH = 0.5;
  let yaw = BASE_YAW, pitch = BASE_PITCH, w = 0, h = 0, f = 1, ox = 0, oy = 0, dist = 7.2;
  let lastInput = -1e9, raf = 0, visible = true, dead = false;
  const view: RoomView = { state, host, redraw: () => draw(), dispose, onPick: () => {} };

  // camera: orbit about `target`, looking at it from `dist`
  let cy = 1, sy = 0, cp = 1, sp = 0, cam: V3 = [0, 0, 0];
  function setCamera() {
    cy = Math.cos(yaw); sy = Math.sin(yaw); cp = Math.cos(pitch); sp = Math.sin(pitch);
    cam = [target[0] + dist * cp * sy, target[1] + dist * sp, target[2] + dist * cp * cy];
  }
  function project(p: V3): [number, number, number] {
    const x = p[0] - target[0], y = p[1] - target[1], z = p[2] - target[2];
    const x1 = cy * x - sy * z, z1 = sy * x + cy * z;          // turn about +y
    const y2 = cp * y - sp * z1, z2 = sp * y + cp * z1;        // tilt about the camera's x
    const depth = dist - z2;
    return [ox + (f * x1) / depth, oy - (f * y2) / depth, depth];
  }
  const facing = (p: V3, n: V3) => (cam[0] - p[0]) * n[0] + (cam[1] - p[1]) * n[1] + (cam[2] - p[2]) * n[2] > 0;
  const vtx = (i: number): V3 => [V[i * 3], V[i * 3 + 1], V[i * 3 + 2]];
  function normal(a: V3, b: V3, c: V3): V3 {
    const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]], v = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
    return [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]];
  }

  function poly(pts: [number, number, number][], fill: string | null, stroke: string | null, width = 1, dash: number[] = []) {
    ctx.beginPath();
    pts.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
    ctx.closePath();
    if (fill) { ctx.fillStyle = fill; ctx.fill(); }
    if (stroke) { ctx.setLineDash(dash); ctx.strokeStyle = stroke; ctx.lineWidth = width; ctx.stroke(); ctx.setLineDash([]); }
  }
  function drawBox(b: Box, fill: string, stroke: string, width: number, dash: number[] = []) {
    const c = boxCorners(b), p = c.map(project);
    const faces = BOX_FACES.map((q) => ({ q, depth: q.reduce((s, i) => s + p[i][2], 0) / 4 }))
      .filter(({ q }) => facing(c[q[0]], normal(c[q[0]], c[q[1]], c[q[2]])));
    faces.sort((a, b2) => b2.depth - a.depth).forEach(({ q }) => poly(q.map((i) => p[i]), fill, null));
    ctx.setLineDash(dash);
    ctx.strokeStyle = stroke; ctx.lineWidth = width;
    ctx.beginPath();
    for (const [a, b2] of BOX_EDGES) { ctx.moveTo(p[a][0], p[a][1]); ctx.lineTo(p[b2][0], p[b2][1]); }
    ctx.stroke();
    ctx.setLineDash([]);
  }

  function draw() {
    if (dead || !w) return;
    setCamera();
    const dpr = canvas.width / w;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    ctx.lineJoin = 'round';
    const shown = state.reveal;
    const fade = Math.min(1, shown * 1.5);

    // the far walls (their inside faces the camera)
    for (const [x0, z0, x1, z1, y0, y1] of data.walls) {
      const c: V3[] = [[x0, y0, z0], [x1, y0, z1], [x1, y1, z1], [x0, y1, z0]];
      if (!facing(c[0], normal(c[0], c[1], c[2]))) continue;
      poly(c.map(project), `rgba(${BLUE}, ${0.06 * fade})`, `rgba(${BLUE}, ${0.55 * fade})`, 1.2);
    }
    // the floor's outline
    poly(data.outline.map(([x, z]) => project([x, data.floor, z])), `rgba(${BLUE}, ${0.04 * fade})`, `rgba(${BLUE}, ${0.4 * fade})`, 1);

    // scanned surfaces: the edges of every triangle facing the camera (the clean-up's leftovers from both sides)
    const count = T.length / 3;
    const upto = Math.floor(count * shown);
    const keep = new Set<number>();
    const kept = new Path2D(), junk = new Path2D();
    for (let t = 0; t < upto; t++) {
      const isJunk = t >= data.mesh.raw_from;
      if (isJunk && !state.raw) continue;
      const i0 = T[t * 3], i1 = T[t * 3 + 1], i2 = T[t * 3 + 2];
      const a = vtx(i0);
      if (!isJunk && !facing(a, normal(a, vtx(i1), vtx(i2)))) continue;
      const path = isJunk ? junk : kept;
      for (const [p, q] of [[i0, i1], [i1, i2], [i2, i0]]) {
        const key = p < q ? p * 65536 + q : q * 65536 + p;
        if (keep.has(key)) continue;
        keep.add(key);
        const A = project(vtx(p)), B = project(vtx(q));
        path.moveTo(A[0], A[1]); path.lineTo(B[0], B[1]);
      }
    }
    ctx.lineWidth = 0.8;
    ctx.strokeStyle = `rgba(${BLUE}, 0.42)`;
    ctx.stroke(kept);
    if (state.raw) { ctx.lineWidth = 1.4; ctx.strokeStyle = `rgba(${RED}, 0.95)`; ctx.stroke(junk); }

    if (shown >= 1) {
      // doors and windows
      for (const pl of data.planes) {
        const c = [0, 1, 2, 3].map((k) => pl.corners.slice(k * 3, k * 3 + 3).map((n) => n / 100) as V3);
        poly(c.map(project), `rgba(${GREEN}, 0.32)`, `rgba(${GREEN}, 0.9)`, 1.4);
      }
      const sug = data.suggestion;
      if (state.door === 'door') drawBox({ ...sug, d: 0.02 }, `rgba(${GREEN}, 0.32)`, `rgba(${GREEN}, 0.9)`, 1.4);
      if (state.door === 'closet') drawBox(sug, `rgba(${ORANGE}, 0.10)`, `rgba(${ORANGE}, 0.95)`, 1.4);
      if (state.door === 'pending') drawBox(sug, `rgba(${GREEN}, 0.06)`, `rgba(${GREEN}, 0.6)`, 1.1, [5, 4]);
      // furniture, far ones first
      const order = data.boxes.map((b, i) => ({ i, d: project([b.cx, b.top - b.h / 2, b.cz])[2] })).sort((a, b) => b.d - a.d);
      for (const { i } of order) {
        const sel = state.selected === i;
        drawBox(data.boxes[i], `rgba(${ORANGE}, ${sel ? 0.34 : 0.09})`, sel ? '#ffc58a' : `rgba(${ORANGE}, 0.95)`, sel ? 2.4 : 1.4);
      }
      if (state.raw) for (const b of data.removed) drawBox(b, `rgba(${RED}, 0.08)`, `rgba(${RED}, 0.9)`, 1.3, [5, 4]);
      // the play area's middle, facing forward (-z)
      const arrow: V3[] = [[0, data.floor + 0.01, -0.32], [0.17, data.floor + 0.01, 0.12], [-0.17, data.floor + 0.01, 0.12]];
      poly(arrow.map(project), `rgb(${ORANGE})`, null);
    }
  }

  // ------------------------------------------------------------------ size, motion, input
  function resize() {
    const r = host.getBoundingClientRect();
    w = Math.max(1, r.width); h = Math.max(1, r.height);
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr);
    canvas.style.width = `${w}px`; canvas.style.height = `${h}px`;
    fit();
    draw();
  }
  /** Scale and place the view so the room fills the space above the legend, with room to sway. */
  function fit() {
    const top = 26, bottom = (host.querySelector<HTMLElement>('.r-legend')?.offsetHeight ?? 0) + 16;
    const keep = [yaw, pitch];
    let lo = [Infinity, Infinity], hi = [-Infinity, -Infinity];
    f = 1; ox = 0; oy = 0;
    for (const y0 of [BASE_YAW - 0.4, BASE_YAW, BASE_YAW + 0.4]) {
      yaw = y0; pitch = BASE_PITCH; setCamera();
      for (const [x, z] of data.outline) for (const y of [data.floor, data.ceiling]) {
        const [px, py] = project([x, y, z]);
        lo = [Math.min(lo[0], px), Math.min(lo[1], py)]; hi = [Math.max(hi[0], px), Math.max(hi[1], py)];
      }
    }
    [yaw, pitch] = keep;
    f = Math.min((w - 24) / (hi[0] - lo[0]), (h - top - bottom) / (hi[1] - lo[1]));
    ox = 12 + ((w - 24) - (hi[0] - lo[0]) * f) / 2 - lo[0] * f;
    oy = top + ((h - top - bottom) - (hi[1] - lo[1]) * f) / 2 - lo[1] * f;
  }
  const ro = new ResizeObserver(resize);
  ro.observe(host);
  const io = new IntersectionObserver((es) => { visible = es.some((e) => e.isIntersecting); if (visible) loop(); });
  io.observe(host);

  function loop() {
    cancelAnimationFrame(raf);
    if (dead || !visible || document.hidden) return;
    raf = requestAnimationFrame((now) => {
      if (!reduce && now - lastInput > 2500) {
        const t = now / 1000;
        yaw += (BASE_YAW + Math.sin(t * 0.25) * 0.38 - yaw) * 0.02;
        pitch += (BASE_PITCH - pitch) * 0.02;
      }
      draw();
      loop();
    });
  }
  const onVis = () => loop();
  document.addEventListener('visibilitychange', onVis);

  let drag: { x: number; y: number; moved: number; id: number } | null = null;
  canvas.addEventListener('pointerdown', (e) => {
    drag = { x: e.clientX, y: e.clientY, moved: 0, id: e.pointerId };
    canvas.setPointerCapture(e.pointerId);
    lastInput = performance.now();
  });
  canvas.addEventListener('pointermove', (e) => {
    if (!drag || drag.id !== e.pointerId) return;
    const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    drag.moved += Math.abs(dx) + Math.abs(dy);
    drag.x = e.clientX; drag.y = e.clientY;
    yaw -= dx * 0.009;
    pitch = Math.max(0.12, Math.min(1.3, pitch + dy * 0.006));
    lastInput = performance.now();
    host.classList.add('turned');
    if (reduce) draw();
  });
  const end = (e: PointerEvent) => {
    if (!drag || drag.id !== e.pointerId) return;
    const click = drag.moved < 6;
    drag = null;
    lastInput = performance.now();
    if (click && state.reveal >= 1) {
      const r = canvas.getBoundingClientRect();
      view.onPick(pick(e.clientX - r.left, e.clientY - r.top));
    }
  };
  canvas.addEventListener('pointerup', end);
  canvas.addEventListener('pointercancel', (e) => { if (drag?.id === e.pointerId) drag = null; });

  /** The nearest box under the point (its projected outline), else null. */
  function pick(x: number, y: number): number | null {
    setCamera();
    let best: number | null = null, bestDepth = Infinity;
    data.boxes.forEach((b, i) => {
      const p = boxCorners(b).map(project);
      if (!inHull(p.map(([px, py]) => [px, py] as [number, number]), x, y)) return;
      const depth = p.reduce((s, q) => s + q[2], 0) / 8 - Math.min(0.6, b.w * b.d * b.h);  // small ones win ties
      if (depth < bestDepth) { bestDepth = depth; best = i; }
    });
    return best;
  }

  function dispose() {
    dead = true;
    cancelAnimationFrame(raf);
    ro.disconnect(); io.disconnect();
    document.removeEventListener('visibilitychange', onVis);
    canvas.remove();
  }

  resize();
  loop();
  return view;
}

/** Whether (x, y) is inside the convex hull of the points (monotone chain). */
function inHull(pts: [number, number][], x: number, y: number): boolean {
  const s = pts.slice().sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  const cross = (o: number[], a: number[], b: number[]) => (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
  const lower: [number, number][] = [], upper: [number, number][] = [];
  for (const p of s) { while (lower.length >= 2 && cross(lower[lower.length - 2], lower[lower.length - 1], p) <= 0) lower.pop(); lower.push(p); }
  for (const p of s.reverse()) { while (upper.length >= 2 && cross(upper[upper.length - 2], upper[upper.length - 1], p) <= 0) upper.pop(); upper.push(p); }
  const hull = lower.slice(0, -1).concat(upper.slice(0, -1));
  if (hull.length < 3) return false;
  return hull.every((a, i) => cross(a, hull[(i + 1) % hull.length], [x, y]) >= 0);
}
