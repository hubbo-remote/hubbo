import { Circle, Clapperboard, Lightbulb, Music, Pause, Play, Plug, SkipBack, SkipForward, Square, Tv, Undo2, VolumeX, type LucideIcon, type LucideProps } from 'lucide-react'
import { memo, useRef, useState, type KeyboardEvent, type ReactNode } from 'react'
import type { EventKind } from '@/api/types'
import { buttonLabel } from '@/lib/buttons'
import { cn } from '@/lib/utils'
import { BODY, BODY_SEAM, DPAD_BASE, KEYS, ROCKER_LABELS, VIEW_H, VIEW_W, WORDMARK_Y, neighbour, type Glyph, type IconName, type Shape } from './layout'

export interface RemoteSvgProps {
  /** Bound event kinds per button ID. */
  bound?: Record<string, EventKind[]>
  selected?: string | null
  onSelect?: (id: string) => void
  /** Buttons with validation errors. */
  errors?: ReadonlySet<string>
  /** Whose bindings these are: an activity's (blue) or the global buttons' (amber). */
  source?: BindingSource
  /** Buttons that fall back to a global binding (this set binds none of them): its event kinds and what it does. */
  inherited?: Readonly<Record<string, { kinds: EventKind[]; text: string }>>
  className?: string
  label?: string
}

/** Where a set of bindings lives: amber always means the global buttons. */
export type BindingSource = 'activity' | 'global'
const SOURCE_COLOR: Record<BindingSource, string> = { activity: 'var(--remote-bound)', global: 'var(--remote-global)' }

const EV_COLOR: Record<EventKind, string> = { down: 'var(--ev-down)', up: 'var(--ev-up)', tap: 'var(--ev-tap)', hold: 'var(--ev-hold)' }
/** How each mark reads: Press, Long press, and raw down/up triggers as they are. */
const EV_LABEL: Record<EventKind, string> = { tap: 'Press', down: 'Down', up: 'Up', hold: 'Long press' }
const LEGEND: readonly EventKind[] = ['tap', 'hold'] // Press and Long press (raw down/up only show under Advanced triggers)

/**
 * A Lucide icon centred on the key: a nested 13-unit svg (Lucide's 24-unit grid at about half scale), its stroke
 * thickened so it matches the hand-drawn arrows' 1.7 here. It takes the glyph colour like the rest (currentColor).
 */
const icon = (Icon: LucideIcon, props?: LucideProps) => <Icon x={-6.5} y={-6.5} width={13} height={13} strokeWidth={3} aria-hidden {...props} />
/** Media keys read as solid shapes, as on a real remote. */
const FILLED: LucideProps = { fill: 'currentColor' }

// Arrows, plus/minus and the stripe stay plain strokes; everything with a picture is a Lucide icon.
const ICONS: Record<IconName, ReactNode> = {
  up: <path d="M-5 2.5 0-2.5 5 2.5" />,
  down: <path d="M-5-2.5 0 2.5 5-2.5" />,
  left: <path d="M2.5-5-2.5 0 2.5 5" />,
  right: <path d="M-2.5-5 2.5 0-2.5 5" />,
  plus: <path d="M-4.5 0h9M0-4.5v9" />,
  minus: <path d="M-4.5 0h9" />,
  music: icon(Music),
  tv: icon(Tv),
  movie: icon(Clapperboard),
  bulb: icon(Lightbulb),
  socket: icon(Plug),
  stripe: <path d="M-5 0h10" strokeWidth={2.4} />,
  play: icon(Play, FILLED),
  pause: icon(Pause, FILLED),
  stop: icon(Square, FILLED),
  record: icon(Circle, { fill: 'oklch(0.65 0.21 25)', stroke: 'none' }), // a red dot, as on any recorder
  next: icon(SkipForward, FILLED),
  prev: icon(SkipBack, FILLED),
  mute: icon(VolumeX),
  back: icon(Undo2),
}

/** Minimum hit area per key, in viewBox units (≥ 24 px once the remote is ~1.5 px per unit, as on a phone). */
const HIT = 16

function ShapeEl({ s, ...rest }: { s: Shape } & React.SVGProps<SVGPathElement & SVGCircleElement>) {
  return s.kind === 'circle' ? <circle cx={s.cx} cy={s.cy} r={s.r} {...rest} /> : <path d={s.d} {...rest} />
}

function GlyphEl({ g, cx, cy }: { g: Glyph; cx: number; cy: number }) {
  if ('text' in g) {
    return (
      <text x={cx} y={cy} textAnchor="middle" dominantBaseline="central" fontSize={g.size ?? 9} fontWeight={600} fill="var(--remote-glyph)" style={{ letterSpacing: '0.02em' }}>
        {g.text}
      </text>
    )
  }
  const color = g.color ?? 'var(--remote-glyph)'
  return (
    <g transform={`translate(${cx} ${cy}) scale(${g.scale ?? 0.85})`} fill="none" stroke="currentColor" strokeWidth={1.7} strokeLinecap="round" strokeLinejoin="round" style={{ color }}>
      {g.digit ? <g transform="translate(-2.4 0)">{ICONS[g.icon]}</g> : ICONS[g.icon]}
      {g.digit && (
        <text x={6.8} y={4.4} fontSize={6} fontWeight={700} textAnchor="middle" dominantBaseline="central" fill="var(--remote-glyph)" stroke="none" opacity={0.75}>
          {g.digit}
        </text>
      )}
    </g>
  )
}

/**
 * Hubbo's clickable remote. Each key is a focusable SVG button with an
 * accessible name; arrow keys move between keys spatially, Enter/Space selects.
 */
export const RemoteSvg = memo(function RemoteSvg({ bound = {}, selected = null, onSelect, errors, source = 'activity', inherited, className, label = 'Remote buttons' }: RemoteSvgProps) {
  const [focusId, setFocusId] = useState<string>(selected ?? 'select')
  const refs = useRef(new Map<string, SVGGElement>())
  const interactive = !!onSelect
  const tabStop = selected && KEYS.some((k) => k.id === selected) ? selected : focusId

  const focusKey = (id: string) => refs.current.get(id)?.focus()

  const onKeyDown = (e: KeyboardEvent, id: string) => {
    const dir = ({ ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' } as const)[e.key as 'ArrowUp']
    if (dir) {
      e.preventDefault()
      const n = neighbour(id, dir)
      if (n) focusKey(n)
    } else if (e.key === 'Home' || e.key === 'End') {
      e.preventDefault()
      focusKey(e.key === 'Home' ? KEYS[0]!.id : KEYS[KEYS.length - 1]!.id)
    } else if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault()
      onSelect?.(id)
    }
  }

  return (
    <svg
      viewBox={`0 0 ${VIEW_W} ${VIEW_H}`}
      role="group"
      aria-label={label}
      className={cn('h-auto w-full select-none [-webkit-tap-highlight-color:transparent]', className)}
    >
      <defs>
        <linearGradient id="rm-body" x1="0" x2="1" y1="0" y2="0">
          <stop offset="0" stopColor="var(--remote-body-2)" />
          <stop offset="0.5" stopColor="var(--remote-body)" />
          <stop offset="1" stopColor="var(--remote-body-2)" />
        </linearGradient>
        <linearGradient id="rm-key" x1="0" x2="0" y1="0" y2="1">
          <stop offset="0" stopColor="var(--remote-key-hi)" />
          <stop offset="1" stopColor="var(--remote-key)" />
        </linearGradient>
      </defs>

      {/* Body */}
      <path d={BODY.d} transform="translate(1 4)" fill="oklch(0 0 0 / 0.18)" />
      <path d={BODY.d} fill="url(#rm-body)" stroke="var(--remote-edge)" strokeWidth={1.2} />
      <g aria-hidden>
        <path d={BODY_SEAM} fill="none" stroke="oklch(1 0 0 / 0.07)" strokeWidth={1} />
        <path d={DPAD_BASE.d} fill="oklch(0 0 0 / 0.28)" />
        <text x={VIEW_W / 2} y={WORDMARK_Y} textAnchor="middle" dominantBaseline="central" fontSize={10} fontWeight={600} fill="var(--remote-glyph)" opacity={0.85} style={{ letterSpacing: '0.18em' }}>
          HUBBO
        </text>
      </g>

      {KEYS.map((k) => {
        // One mark for every bound key, own or inherited: its source decides the colour, its bindings the dots.
        const own = bound[k.id]?.length ? bound[k.id]! : null
        const global = own ? undefined : inherited?.[k.id]
        const ev = own ?? global?.kinds ?? []
        const color = own ? SOURCE_COLOR[source] : global ? SOURCE_COLOR.global : null
        const isSel = selected === k.id
        const isErr = errors?.has(k.id) ?? false
        const { cx, cy } = k.shape
        const name = buttonLabel(k.id)
        const [px, py] = k.shape.pip
        return (
          <g
            key={k.id}
            ref={(el) => {
              if (el) refs.current.set(k.id, el)
              else refs.current.delete(k.id)
            }}
            data-key={k.id}
            role={interactive ? 'button' : 'img'}
            tabIndex={interactive ? (k.id === tabStop ? 0 : -1) : undefined}
            aria-label={`${name}${ev.length ? `, ${global ? 'global' : 'bound'}: ${ev.map((e) => EV_LABEL[e].toLowerCase()).join(', ')}` : interactive ? ', unbound' : ''}${isErr ? ', has errors' : ''}`}
            aria-pressed={interactive ? isSel : undefined}
            onClick={() => onSelect?.(k.id)}
            onFocus={() => setFocusId(k.id)}
            onKeyDown={interactive ? (e) => onKeyDown(e, k.id) : undefined}
            className={cn('group outline-none', interactive && 'cursor-pointer')}
          >
            <title>{global ? `${name}, Global: ${global.text}` : `${name} (${k.id})${ev.length ? ` · ${ev.join(', ')}` : ''}`}</title>
            {/* An invisible minimum hit area: small keys (numbers, colours) stay tappable on a phone. */}
            {interactive && <rect x={cx - HIT / 2} y={cy - HIT / 2} width={HIT} height={HIT} fill="transparent" />}
            {/* Focus/error ring: the same shape stroked wide behind the key, whose fill hides the inner half. */}
            <ShapeEl
              s={k.shape}
              fill="none"
              strokeWidth={6}
              className={cn(
                'opacity-0 transition-opacity',
                'stroke-[var(--ring)] group-focus-visible:opacity-100',
                isErr && 'opacity-100 stroke-[var(--destructive)]',
              )}
              strokeDasharray={!isErr ? '3 2' : undefined}
            />
            <ShapeEl
              s={k.shape}
              fill="url(#rm-key)"
              // The key's one border: pink and thick when selected; else its source's colour.
              stroke={isSel ? 'var(--remote-selected)' : (color ?? 'oklch(1 0 0 / 0.1)')}
              strokeWidth={isSel ? 2.6 : color ? 1.8 : 0.8}
              className={cn('transition-[filter] duration-100', interactive && 'group-hover:brightness-125 group-active:brightness-90')}
            />
            {color && <ShapeEl s={k.shape} fill={color} opacity={0.16} pointerEvents="none" />}
            <GlyphEl g={k.glyph} cx={cx} cy={cy} />
            {ev.length > 0 && (
              <g aria-hidden pointerEvents="none">
                {ev.map((e, i) => (
                  <circle key={e} cx={px - (ev.length - 1 - i) * 4.6} cy={py} r={2.3} fill={EV_COLOR[e]} stroke="var(--remote-body)" strokeWidth={0.9} />
                ))}
              </g>
            )}
          </g>
        )
      })}
      <g aria-hidden pointerEvents="none" fontSize={5.5} fontWeight={700} fill="var(--remote-glyph)" textAnchor="middle" dominantBaseline="central" opacity={0.7} style={{ letterSpacing: '0.06em' }}>
        {ROCKER_LABELS.map((l) => (
          <text key={l.text} x={l.at[0]} y={l.at[1]}>
            {l.text}
          </text>
        ))}
      </g>
    </svg>
  )
})


export function EventLegend({ className }: { className?: string }) {
  return (
    <ul className={cn('flex flex-wrap items-center gap-x-3 gap-y-1 text-xs text-muted-foreground', className)} aria-label="Binding legend">
      {LEGEND.map((e) => (
        <li key={e} className="inline-flex items-center gap-1.5">
          <span className="size-2.5 rounded-full" style={{ background: EV_COLOR[e] }} aria-hidden />
          {EV_LABEL[e]}
        </li>
      ))}
    </ul>
  )
}

export { EV_COLOR, EV_LABEL }

/** Where a key's binding comes from, for a set with globals behind it: blue, or amber. */
export function SourceLegend({ className }: { className?: string }) {
  return (
    <ul className={cn('flex flex-wrap items-center gap-x-3 gap-y-1 text-xs text-muted-foreground', className)} aria-label="Where bindings come from">
      <li className="inline-flex items-center gap-1.5">
        <span className="h-2.5 w-3.5 rounded-sm border-[1.5px] border-[var(--remote-bound)] bg-[var(--remote-bound)]/20" aria-hidden />
        This activity
      </li>
      <li className="inline-flex items-center gap-1.5">
        <span className="h-2.5 w-3.5 rounded-sm border-[1.5px] border-[var(--global)] bg-[var(--global)]/20" aria-hidden />
        Global
      </li>
    </ul>
  )
}
