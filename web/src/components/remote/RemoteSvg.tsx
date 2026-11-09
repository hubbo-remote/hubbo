import { memo, useRef, useState, type KeyboardEvent, type ReactNode } from 'react'
import type { EventKind } from '@/api/types'
import { EVENT_ORDER } from '@/lib/activity'
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
  className?: string
  label?: string
}

const EV_COLOR: Record<EventKind, string> = { down: 'var(--ev-down)', up: 'var(--ev-up)', tap: 'var(--ev-tap)', hold: 'var(--ev-hold)' }

const ICONS: Record<IconName, ReactNode> = {
  up: <path d="M-5 2.5 0-2.5 5 2.5" />,
  down: <path d="M-5-2.5 0 2.5 5-2.5" />,
  left: <path d="M2.5-5-2.5 0 2.5 5" />,
  right: <path d="M-2.5-5 2.5 0-2.5 5" />,
  plus: <path d="M-4.5 0h9M0-4.5v9" />,
  minus: <path d="M-4.5 0h9" />,
  music: (
    <g>
      <path d="M-1.2 4V-4.6l5.2-1.4v8" />
      <circle cx="-3" cy="4" r="1.9" className="fill-current" />
      <circle cx="2.2" cy="2" r="1.9" className="fill-current" />
    </g>
  ),
  tv: (
    <g>
      <rect x="-6.5" y="-4.8" width="13" height="8.4" rx="1.4" />
      <path d="M-3 5.6h6" />
    </g>
  ),
  movie: (
    <g>
      <rect x="-6" y="-1.6" width="12" height="7" rx="1" />
      <path d="M-6.2-3.4 5.6-6.4M-2.6-4.3l1.3 2.5M1.4-5.3l1.3 2.5" />
    </g>
  ),
  bulb: (
    <g>
      <path d="M-2.2 2.2C-4.6.7-4.9-3.6-2.4-5.4c1.5-1 3.3-1 4.8 0 2.5 1.8 2.2 6.1-.2 7.6z" />
      <path d="M-1.6 4.2h3.2" />
    </g>
  ),
  socket: (
    <g>
      <rect x="-5.2" y="-5.2" width="10.4" height="10.4" rx="3" />
      <path d="M-1.9-2.4v2.2M1.9-2.4v2.2M-1.6 2.6a2 1.3 0 0 0 3.2 0" />
    </g>
  ),
  stripe: <path d="M-5 0h10" strokeWidth={2.4} />,
  play: <path d="M-3.6-5.4 5.4 0l-9 5.4z" className="fill-current" />,
  pause: <path d="M-3-4.6v9.2M3-4.6v9.2" strokeWidth={2.4} />,
  stop: <rect x="-4" y="-4" width="8" height="8" rx="1" className="fill-current" />,
  record: <circle r="4.2" fill="oklch(0.65 0.21 25)" stroke="none" />,
  next: (
    <g className="fill-current" stroke="none">
      <path d="M-10-3.6-5 0l-5 3.6zM-4.6-3.6.4 0l-5 3.6z" />
      <path d="M3 -3.6 7.6 0 3 3.6zM8.3-3.6h1.6v7.2H8.3z" />
    </g>
  ),
  prev: (
    <g className="fill-current" stroke="none">
      <path d="M-3-3.6-7.6 0-3 3.6zM-8.3-3.6h-1.6v7.2h1.6z" />
      <path d="M10-3.6 5 0l5 3.6zM4.6-3.6-.4 0l5 3.6z" />
    </g>
  ),
  mute: (
    <g>
      <path d="M-6-2h2.6L0-5v10l-3.4-3H-6z" />
      <path d="m2.4-2.4 4 4.8m0-4.8-4 4.8" />
    </g>
  ),
  back: (
    <g>
      <path d="M-5.5 1.5H3a3 3 0 0 0 0-6H-2" />
      <path d="M-3 -1-5.5 1.5-3 4" />
    </g>
  ),
}

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
export const RemoteSvg = memo(function RemoteSvg({ bound = {}, selected = null, onSelect, errors, className, label = 'Remote buttons' }: RemoteSvgProps) {
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
        const isBound = (bound[k.id]?.length ?? 0) > 0
        const isSel = selected === k.id
        const isErr = errors?.has(k.id) ?? false
        const { cx, cy } = k.shape
        const ev = bound[k.id] ?? []
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
            aria-label={`${name}${ev.length ? `, bound: ${ev.join(', ')}` : interactive ? ', unbound' : ''}${isErr ? ', has errors' : ''}`}
            aria-pressed={interactive ? isSel : undefined}
            onClick={() => onSelect?.(k.id)}
            onFocus={() => setFocusId(k.id)}
            onKeyDown={interactive ? (e) => onKeyDown(e, k.id) : undefined}
            className={cn('group outline-none', interactive && 'cursor-pointer')}
          >
            <title>{`${name} (${k.id})${ev.length ? ` · ${ev.join(', ')}` : ''}`}</title>
            {/* Focus/selection ring: the same shape stroked wide behind the key, whose fill hides the inner half. */}
            <ShapeEl
              s={k.shape}
              fill="none"
              strokeWidth={6}
              className={cn(
                'opacity-0 transition-opacity',
                isSel ? 'opacity-100 stroke-[var(--primary)]' : 'stroke-[var(--ring)] group-focus-visible:opacity-100',
                isErr && !isSel && 'opacity-100 stroke-[var(--destructive)]',
              )}
              strokeDasharray={!isSel && !isErr ? '3 2' : undefined}
            />
            <ShapeEl
              s={k.shape}
              fill="url(#rm-key)"
              stroke={isBound ? 'var(--remote-bound)' : 'oklch(1 0 0 / 0.1)'}
              strokeWidth={isBound ? 1.8 : 0.8}
              className={cn('transition-[filter] duration-100', interactive && 'group-hover:brightness-125 group-active:brightness-90')}
            />
            {isBound && <ShapeEl s={k.shape} fill="var(--remote-bound)" opacity={0.16} pointerEvents="none" />}
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
      {EVENT_ORDER.map((e) => (
        <li key={e} className="inline-flex items-center gap-1.5">
          <span className="size-2.5 rounded-full" style={{ background: EV_COLOR[e] }} aria-hidden />
          {e}
        </li>
      ))}
    </ul>
  )
}

export { EV_COLOR }
