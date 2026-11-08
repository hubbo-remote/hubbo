import type { ButtonId } from '../../api/types'

// Geometry for Hubbo's drawing of a Companion-style remote. Original vector
// artwork: rounded polygons laid out to match the real remote's key
// positions, grouping and proportions (measured by eye on a 355-unit-wide
// reference grid, then scaled into a 200-unit viewBox). No bitmap, logo or
// trademark is used. Every one of the 48 button IDs from
// core/src/remote/button.cpp appears exactly once (a test checks).

export type Shape =
  | { kind: 'circle'; cx: number; cy: number; r: number; pip: [number, number] }
  | { kind: 'path'; d: string; cx: number; cy: number; pip: [number, number] }

export type IconName =
  | 'up' | 'down' | 'left' | 'right' | 'music' | 'tv' | 'movie' | 'bulb' | 'socket'
  | 'play' | 'pause' | 'stop' | 'record' | 'prev' | 'next' | 'mute' | 'back' | 'plus' | 'minus' | 'stripe'

export type Glyph =
  | { text: string; size?: number }
  | { icon: IconName; digit?: string; color?: string; scale?: number }

export interface KeyDef {
  id: ButtonId
  shape: Shape
  glyph: Glyph
}

type Pt = [number, number]

// Reference grid: the remote body spans x 30..385, y 10..1195.
const S = 184 / 355
const v = ([x, y]: Pt): Pt => [8 + (x - 30) * S, 8 + (y - 10) * S]

export const VIEW_W = 200
export const VIEW_H = 630

const f = (n: number) => +n.toFixed(2)

/** Closed polygon with per-vertex corner radii (reference units), as an SVG path in view units. */
function roundedPoly(points: Pt[], radii: number | number[]): Shape & { kind: 'path' } {
  const pts = points.map(v)
  const rs = points.map((_, i) => (Array.isArray(radii) ? (radii[i] ?? 0) : radii) * S)
  const n = pts.length
  const along = (a: Pt, b: Pt, d: number): Pt => {
    const len = Math.hypot(b[0] - a[0], b[1] - a[1]) || 1
    const t = Math.min(d, len / 2) / len
    return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]
  }
  let d = ''
  for (let i = 0; i < n; i++) {
    const p = pts[i]!
    const prev = pts[(i - 1 + n) % n]!
    const next = pts[(i + 1) % n]!
    const a = along(p, prev, rs[i]!)
    const b = along(p, next, rs[i]!)
    d += `${i === 0 ? 'M' : 'L'}${f(a[0])} ${f(a[1])}Q${f(p[0])} ${f(p[1])} ${f(b[0])} ${f(b[1])}`
  }
  d += 'Z'
  const xs = pts.map((p) => p[0])
  const ys = pts.map((p) => p[1])
  // Binding dots sit at the right end of the top edge, which stays on the key for angled shapes too.
  const top = Math.min(...ys)
  const tr = pts.filter((p) => p[1] <= top + 0.5).reduce((a, b) => (b[0] > a[0] ? b : a))
  return { kind: 'path', d, cx: (Math.min(...xs) + Math.max(...xs)) / 2, cy: (top + Math.max(...ys)) / 2, pip: [tr[0] - 4, tr[1] + 2.5] }
}

/** Rectangle with corner radii [tl, tr, br, bl] (reference units). */
const rect = (x: number, y: number, w: number, h: number, r: number | [number, number, number, number]) =>
  roundedPoly([[x, y], [x + w, y], [x + w, y + h], [x, y + h]], r)

/** A bar split into equal keys with hairline seams; only the outer corners are rounded. */
function bar(x0: number, x1: number, y: number, h: number, n: number, r: number, gap = 2.5) {
  const w = (x1 - x0 - gap * (n - 1)) / n
  return Array.from({ length: n }, (_, i) =>
    rect(x0 + i * (w + gap), y, w, h, [i === 0 ? r : 2, i === n - 1 ? r : 2, i === n - 1 ? r : 2, i === 0 ? r : 2]),
  )
}

const mirror = (pts: Pt[]): Pt[] => pts.map(([x, y]) => [414 - x, y] as Pt).reverse()

// D-pad: a rounded diamond split into four wedges around a round OK key.
const C: Pt = [207, 629]
const R = 92
const R_IN = 42
const OK_R = 33
const GAP = 3
const u = (deg: number): Pt => [Math.cos((deg * Math.PI) / 180), Math.sin((deg * Math.PI) / 180)]
const at = (dist: number, deg: number, shiftDeg?: number, shift = 0): Pt => {
  const a = u(deg)
  const s = shiftDeg === undefined ? [0, 0] : u(shiftDeg)
  return [C[0] + a[0] * dist + s[0]! * shift, C[1] + a[1] * dist + s[1]! * shift]
}
function wedge(a: number) {
  const mid = R / Math.SQRT2
  const delta = (Math.asin(GAP / R_IN) * 180) / Math.PI
  const inner: Pt[] = []
  for (let k = 0; k <= 6; k++) {
    const ang = a + 45 - delta - ((90 - 2 * delta) * k) / 6
    inner.push(at(R_IN, ang))
  }
  const pts: Pt[] = [at(mid, a - 45, a + 45, GAP), at(R, a), at(mid, a + 45, a - 45, GAP), ...inner]
  const s = roundedPoly(pts, [3, 26, 3, ...inner.map(() => 0)])
  // Navigation/glyph centre: middle of the wedge, not of its bounding box.
  const [gx, gy] = v(at((R_IN + R * 0.75) / 2, a))
  return { ...s, cx: gx, cy: gy }
}
export const DPAD_BASE = roundedPoly([at(R + 9, -90), at(R + 9, 0), at(R + 9, 90), at(R + 9, 180)], 30)

export const BODY = roundedPoly([[30, 10], [385, 10], [385, 1195], [30, 1195]], [150, 150, 110, 110])
/** Seam where the battery bulge starts near the bottom. */
export const BODY_SEAM = (() => {
  const [x0, y] = v([34, 1070])
  const [x1] = v([381, 1070])
  return `M${f(x0)} ${f(y)}Q${f((x0 + x1) / 2)} ${f(y + 6)} ${f(x1)} ${f(y)}`
})()
export const WORDMARK_Y = v([0, 100])[1]
/** Decorative labels on the volume and channel rockers (view units). */
export const ROCKER_LABELS: { text: string; at: Pt }[] = [
  { text: 'VOL', at: v([84, 629]) },
  { text: 'CH/PG', at: v([330, 629]) },
]

const ACCENT = 'var(--remote-accent)'
const [music, tv, movie] = bar(64, 352, 205, 37, 3, 14)
const [dvr, guide, info] = bar(64, 352, 437, 41, 3, 15)
const numRows = [897, 940, 983, 1022].map((y) => bar(64, 352, y, 24, 3, 11))

const volUp: Pt[] = [[62, 557], [135, 557], [135, 598], [106, 627], [62, 627]]
const volDown: Pt[] = [[62, 631], [106, 631], [135, 660], [135, 700], [62, 700]]
const exitPts: Pt[] = [[64, 503], [192, 503], [165, 543], [64, 543]]
const mutePts: Pt[] = [[64, 712], [165, 712], [192, 752], [64, 752]]

const COLORS = { red: 'oklch(0.6 0.2 25)', green: 'oklch(0.66 0.17 150)', yellow: 'oklch(0.85 0.16 95)', blue: 'oklch(0.6 0.16 250)' }

export const KEYS: KeyDef[] = [
  { id: 'off', shape: rect(82, 157, 63, 23, 11.5), glyph: { text: 'Off', size: 6.5 } },

  // Activities
  { id: 'music', shape: music!, glyph: { icon: 'music' } },
  { id: 'tv', shape: tv!, glyph: { icon: 'tv' } },
  { id: 'movie', shape: movie!, glyph: { icon: 'movie' } },

  // Home control. Which lamp/socket is 1 vs 2 (top = 1 here) is an
  // assumption until verified on hardware.
  { id: 'light_1', shape: rect(64, 268, 96, 40, 8), glyph: { icon: 'bulb', digit: '1', color: ACCENT } },
  { id: 'light_2', shape: rect(64, 320, 96, 40, 8), glyph: { icon: 'bulb', digit: '2', color: ACCENT } },
  { id: 'plus', shape: rect(180, 265, 57, 47, [8, 8, 2, 2]), glyph: { icon: 'plus', color: ACCENT } },
  { id: 'minus', shape: rect(180, 314, 57, 47, [2, 2, 8, 8]), glyph: { icon: 'minus', color: ACCENT } },
  { id: 'switch_1', shape: rect(255, 268, 97, 40, 8), glyph: { icon: 'socket', digit: '1', color: ACCENT } },
  { id: 'switch_2', shape: rect(255, 320, 97, 40, 8), glyph: { icon: 'socket', digit: '2', color: ACCENT } },

  // Colour keys: dark keys with a coloured dash
  { id: 'red', shape: rect(64, 388, 58, 22, 9), glyph: { icon: 'stripe', color: COLORS.red } },
  { id: 'green', shape: rect(138, 388, 62, 22, 9), glyph: { icon: 'stripe', color: COLORS.green } },
  { id: 'yellow', shape: rect(216, 388, 62, 22, 9), glyph: { icon: 'stripe', color: COLORS.yellow } },
  { id: 'blue', shape: rect(293, 388, 59, 22, 9), glyph: { icon: 'stripe', color: COLORS.blue } },

  { id: 'dvr', shape: dvr!, glyph: { text: 'DVR', size: 8 } },
  { id: 'guide', shape: guide!, glyph: { text: 'Guide', size: 8 } },
  { id: 'info', shape: info!, glyph: { text: 'Info', size: 8 } },

  // Angled keys around the top of the d-pad
  { id: 'exit', shape: roundedPoly(exitPts, [10, 6, 6, 10]), glyph: { text: 'Exit', size: 8 } },
  { id: 'menu', shape: roundedPoly(mirror(exitPts), [10, 6, 6, 10]), glyph: { text: 'Menu', size: 8 } },

  // Navigation cluster: Vol rocker, d-pad, Ch/Pg rocker
  { id: 'volume_up', shape: roundedPoly(volUp, [12, 8, 4, 2, 0]), glyph: { icon: 'plus', scale: 0.7 } },
  { id: 'volume_down', shape: roundedPoly(volDown, [0, 2, 4, 8, 12]), glyph: { icon: 'minus', scale: 0.7 } },
  { id: 'up', shape: wedge(-90), glyph: { icon: 'up' } },
  { id: 'right', shape: wedge(0), glyph: { icon: 'right' } },
  { id: 'down', shape: wedge(90), glyph: { icon: 'down' } },
  { id: 'left', shape: wedge(180), glyph: { icon: 'left' } },
  { id: 'select', shape: { kind: 'circle', cx: v(C)[0], cy: v(C)[1], r: OK_R * S, pip: [v(C)[0] + OK_R * S * 0.72, v(C)[1] - OK_R * S * 0.72] }, glyph: { text: 'OK', size: 9 } },
  { id: 'channel_up', shape: roundedPoly(mirror(volUp), [0, 2, 4, 8, 12]), glyph: { icon: 'up', scale: 0.7 } },
  { id: 'channel_down', shape: roundedPoly(mirror(volDown), [12, 8, 4, 2, 0]), glyph: { icon: 'down', scale: 0.7 } },

  // Angled keys around the bottom of the d-pad
  { id: 'mute', shape: roundedPoly(mutePts, [10, 6, 6, 10]), glyph: { icon: 'mute' } },
  { id: 'back', shape: roundedPoly(mirror(mutePts), [10, 6, 6, 10]), glyph: { icon: 'back' } },

  // Transport
  { id: 'skip_backward', shape: rect(64, 778, 96, 37, 8), glyph: { icon: 'prev' } },
  { id: 'play', shape: rect(180, 776, 57, 46, [8, 8, 2, 2]), glyph: { icon: 'play' } },
  { id: 'skip_forward', shape: rect(255, 778, 97, 37, 8), glyph: { icon: 'next' } },
  { id: 'record', shape: rect(64, 830, 96, 38, 8), glyph: { icon: 'record' } },
  { id: 'pause', shape: rect(180, 824, 57, 46, [2, 2, 8, 8]), glyph: { icon: 'pause' } },
  { id: 'stop', shape: rect(255, 830, 97, 38, 8), glyph: { icon: 'stop' } },

  // Number pad (3 × 4 segmented bars)
  ...['1', '2', '3', '4', '5', '6', '7', '8', '9'].map((n, i) => ({
    id: `number_${n}` as ButtonId,
    shape: numRows[Math.floor(i / 3)]![i % 3]!,
    glyph: { text: n, size: 7 },
  })),
  { id: 'number_dot_dash', shape: numRows[3]![0]!, glyph: { text: '-‑', size: 7.5 } },
  { id: 'number_0', shape: numRows[3]![1]!, glyph: { text: '0', size: 7 } },
  { id: 'number_enter', shape: numRows[3]![2]!, glyph: { text: 'E', size: 7 } },
]

/** Spatial neighbour for arrow-key navigation between keys. */
export function neighbour(fromId: string, dir: 'up' | 'down' | 'left' | 'right'): string | null {
  const from = KEYS.find((k) => k.id === fromId)
  if (!from) return null
  const { cx: fx, cy: fy } = from.shape
  const [vx, vy] = { up: [0, -1], down: [0, 1], left: [-1, 0], right: [1, 0] }[dir] as [number, number]
  let best: { id: string; score: number } | null = null
  for (const k of KEYS) {
    if (k.id === fromId) continue
    const { cx: x, cy: y } = k.shape
    const along = (x - fx) * vx + (y - fy) * vy
    if (along <= 2) continue
    const across = Math.abs((x - fx) * vy - (y - fy) * vx)
    const score = along + across * 2.5
    if (!best || score < best.score) best = { id: k.id, score }
  }
  return best?.id ?? null
}
