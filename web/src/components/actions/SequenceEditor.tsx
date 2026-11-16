import { AlertTriangle, Clock, Copy, GripVertical, Pencil, Plus, Trash2 } from 'lucide-react'
import { useEffect, useId, useRef, useState, type KeyboardEvent } from 'react'
import type { Action, ParamSpec, ValidationIssue } from '@/api/types'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { Field, Input } from '@/components/ui/input'
import { callReady } from '@/lib/haCall'
import { commandFor, isWait, LIMITS, mostUsedDevice, waitAction, type Catalog } from '@/lib/activity'
import { WAIT_CONNECTED_MS, WAIT_MS } from '@/api/constants'
import { cn } from '@/lib/utils'
import { CommandPicker } from './CommandPicker'
import { startAction } from './start'
import { commandLabel, deviceName, paramSummary } from './describe'

/**
 * A lifted copy of a step row that follows the pointer while the row is dragged (the row itself stays, dimmed, and
 * the drop line shows where it lands). A clone, so it looks exactly like the row; inert and hidden from the pointer. No scale with reduced motion.
 */
function liftGhost(row: HTMLElement, pointerY: number) {
  const r = row.getBoundingClientRect()
  const el = row.cloneNode(true) as HTMLElement
  el.querySelectorAll('[id]').forEach((n) => n.removeAttribute('id'))
  el.inert = true // out of the accessibility tree and the tab order
  el.dataset.dragGhost = ''
  const still = matchMedia('(prefers-reduced-motion: reduce)').matches
  Object.assign(el.style, {
    position: 'fixed',
    left: `${r.left}px`,
    top: `${r.top}px`,
    width: `${r.width}px`,
    margin: '0',
    zIndex: '50',
    pointerEvents: 'none',
    opacity: '0.8',
    boxShadow: '0 10px 24px -6px rgb(0 0 0 / 0.25)',
    transform: still ? '' : 'scale(1.02)',
  })
  document.body.append(el)
  const dy = pointerY - r.top
  return { move: (y: number) => void (el.style.top = `${y - dy}px`), remove: () => el.remove() }
}

/** Ordered list of actions (startup/shutdown) with add, edit, duplicate, wait, and reorder (drag, or Alt+↑/↓). */
export function SequenceEditor({ label, actions, onChange, catalog, issues, pathPrefix, lastRun }: { label: string; actions: Action[]; onChange: (a: Action[]) => void; catalog: Catalog; issues: ValidationIssue[]; pathPrefix: string; /** Why steps failed the last time this ran, by index. */ lastRun?: ReadonlyMap<number, string> }) {
  const [editing, setEditing] = useState<{ index: number; action: Action | null } | null>(null)
  const [timeOk, setTimeOk] = useState(true) // the time field's say, while one is open
  // `slot`: the gap a drop inserts into (0 = before the first step, n = after the last).
  const [drag, setDrag] = useState<{ from: number; slot: number } | null>(null)
  const list = useRef<HTMLOListElement>(null)
  const ghost = useRef<ReturnType<typeof liftGhost> | null>(null)
  useEffect(() => () => ghost.current?.remove(), [])
  const drop = () => {
    ghost.current?.remove()
    ghost.current = null
    setDrag(null)
  }
  const refocus = useRef<number | null>(null)
  const hint = useId()
  const moveTo = (from: number, to: number) => {
    if (from === to || to < 0 || to >= actions.length) return false
    const next = [...actions]
    const [x] = next.splice(from, 1)
    next.splice(to, 0, x!)
    onChange(next)
    return true
  }
  // The gap under a pointer: before the first step whose top half it's in, else after the last.
  const slotAt = (y: number) => {
    const steps = [...(list.current?.children ?? [])]
    const i = steps.findIndex((el) => {
      const r = el.getBoundingClientRect()
      return y - r.top <= r.height / 2
    })
    return i < 0 ? steps.length : i
  }
  const onStepKey = (e: KeyboardEvent<HTMLLIElement>, i: number) => {
    if (e.target !== e.currentTarget || !e.altKey || (e.key !== 'ArrowUp' && e.key !== 'ArrowDown')) return
    e.preventDefault()
    const to = i + (e.key === 'ArrowUp' ? -1 : 1)
    if (moveTo(i, to)) refocus.current = to
  }
  // Keep focus on the step that moved.
  useEffect(() => {
    if (refocus.current === null) return
    ;(list.current?.children[refocus.current] as HTMLElement | undefined)?.focus()
    refocus.current = null
  }, [actions])
  const issueFor = (i: number) => issues.filter((x) => x.path === `${pathPrefix}/${i}` || x.path.startsWith(`${pathPrefix}/${i}/`))
  const full = actions.length >= LIMITS.maxSequence

  const timeEdit = !!editing?.action && editing.index < actions.length && !!timed(editing.action)
  return (
    <div className="grid gap-3">
      {actions.length === 0 ? (
        <p className="rounded-lg border border-dashed px-4 py-6 text-center text-sm text-muted-foreground">No steps.</p>
      ) : (
        <ol ref={list} className="grid gap-2" aria-label={`${label} steps`}>
          {actions.map((a, i) => {
            const errs = issueFor(i)
            const failed = lastRun?.get(i)
            const wait = isWait(a)
            return (
              <li
                key={i}
                tabIndex={0}
                aria-describedby={hint}
                onKeyDown={(e) => onStepKey(e, i)}
                className={cn(
                  'group relative flex items-center gap-2 rounded-lg border bg-card py-2 pr-2 pl-1.5 text-sm outline-none focus-visible:ring-2 focus-visible:ring-ring',
                  errs.length ? 'border-destructive/60 bg-destructive/5' : failed && 'border-warning/70',
                  drag?.from === i && 'opacity-50',
                )}
              >
                {/* Where a drop goes: a line in the gap before this step (or after the last), unless it'd stay put. */}
                {drag && drag.slot !== drag.from && drag.slot !== drag.from + 1 && (drag.slot === i || (drag.slot === actions.length && i === actions.length - 1)) && (
                  <span aria-hidden className={cn('pointer-events-none absolute inset-x-0 h-0.5 rounded-full bg-primary', drag.slot === i ? '-top-[5px]' : '-bottom-[5px]')} />
                )}
                {/* Pointer events, so a mouse and a finger drag the same way (HTML drag and drop has no touch). */}
                <span
                  data-grip
                  aria-hidden
                  className="-my-2 shrink-0 cursor-grab touch-none py-2 text-muted-foreground group-hover:text-foreground active:cursor-grabbing"
                  onPointerDown={(e) => {
                    if (e.button !== 0) return
                    e.preventDefault()
                    e.currentTarget.setPointerCapture(e.pointerId)
                    ghost.current = liftGhost(e.currentTarget.closest('li')!, e.clientY)
                    setDrag({ from: i, slot: i })
                  }}
                  onPointerMove={(e) => {
                    const slot = drag && slotAt(e.clientY) // read the rows before the ghost moves (no forced layout)
                    ghost.current?.move(e.clientY)
                    if (drag && slot !== drag.slot) setDrag({ ...drag, slot: slot! })
                  }}
                  onPointerUp={() => {
                    if (drag) moveTo(drag.from, drag.slot > drag.from ? drag.slot - 1 : drag.slot)
                    drop()
                  }}
                  onPointerCancel={drop}
                >
                  <GripVertical className="size-4" />
                </span>
                <span className="w-5 shrink-0 text-right font-mono text-xs text-muted-foreground tabular-nums">{i + 1}</span>
                {wait ? <Clock className="size-4 shrink-0" aria-hidden /> : null}
                <div className="min-w-0 flex-1">
                  <p className="truncate">
                    {wait ? (
                      <span className="font-medium">Wait {paramSummary(a)}</span>
                    ) : (
                      <>
                        <span className="font-medium">{deviceName(a, catalog)}</span>
                        <span className="text-muted-foreground"> · </span>
                        {commandLabel(a, catalog)}
                        {paramSummary(a, catalog) && <span className="text-muted-foreground"> {paramSummary(a, catalog)}</span>}
                      </>
                    )}
                  </p>
                  {errs.map((e) => (
                    <p key={e.path + e.code} className="text-xs text-destructive">
                      {e.message}
                    </p>
                  ))}
                  {failed && (
                    <p className="flex items-center gap-1 text-xs text-muted-foreground">
                      <AlertTriangle className="size-3 shrink-0 text-warning" aria-hidden />
                      Failed last time: {failed}
                    </p>
                  )}
                </div>
                <div className="flex shrink-0 items-center">
                  <Button variant="ghost" size="icon" className="size-8" aria-label={`Edit step ${i + 1}`} onClick={() => setEditing({ index: i, action: a })}>
                    <Pencil />
                  </Button>
                  <Button variant="ghost" size="icon" className="size-8" aria-label={`Duplicate step ${i + 1}`} disabled={full} title={full ? `At most ${LIMITS.maxSequence} steps` : undefined} onClick={() => onChange([...actions.slice(0, i + 1), a, ...actions.slice(i + 1)])}>
                    <Copy />
                  </Button>
                  <Button variant="ghost" size="icon" className="size-8 hover:text-destructive" aria-label={`Remove step ${i + 1}`} onClick={() => onChange(actions.filter((_, j) => j !== i))}>
                    <Trash2 />
                  </Button>
                </div>
              </li>
            )
          })}
        </ol>
      )}
      <p id={hint} className="sr-only">
        Alt+Up and Alt+Down move a step.
      </p>
      <div className="flex flex-wrap gap-2">
        <Button variant="outline" size="sm" disabled={full} onClick={() => setEditing({ index: actions.length, action: startAction(catalog, 'sequence', mostUsedDevice(actions)) })}>
          <Plus /> Add step
        </Button>
        <Button variant="outline" size="sm" disabled={full} onClick={() => onChange([...actions, waitAction()])}>
          <Clock /> Add wait
        </Button>
      </div>
      <Dialog
        open={!!editing}
        onClose={() => setEditing(null)}
        title={editing && editing.index < actions.length ? `Edit step ${editing.index + 1}` : `Add ${label.toLowerCase()} step`}
        footer={
          <>
            <Button variant="ghost" onClick={() => setEditing(null)}>
              Cancel
            </Button>
            <Button
              disabled={!editing?.action || !editing.action.command_id || !callReady(editing.action) || (timeEdit && !timeOk)}
              onClick={() => {
                if (!editing?.action) return
                const next = [...actions]
                next[editing.index] = editing.action
                onChange(next)
                setEditing(null)
              }}
            >
              {editing && editing.index < actions.length ? 'Done' : 'Add step'}
            </Button>
          </>
        }
      >
        {editing && (timeEdit && editing.action ? (
          <TimeField action={editing.action} catalog={catalog} onChange={(action) => setEditing({ ...editing, action })} onValid={setTimeOk} />
        ) : (
          <CommandPicker value={editing.action} onChange={(action) => setEditing({ ...editing, action })} catalog={catalog} context="sequence" />
        ))}
      </Dialog>
    </div>
  )
}

/** A step that's only a time: a wait (its duration), or a Bluetooth wait until connected (its timeout). */
const timed = (a: Action) => (isWait(a) ? 'duration_ms' : a.command_id === 'wait_connected' ? 'timeout_ms' : null)

/** Editing a timed step changes only its time: a wait in ms, a connect timeout in seconds (the device stays as it is). */
/** `onChange` gets each good time; `onValid` whether the field can be saved, from the start (a stored value may be out of range). */
function TimeField({ action, catalog, onChange, onValid }: { action: Action; catalog: Catalog; onChange: (a: Action) => void; onValid: (ok: boolean) => void }) {
  const param = timed(action)!
  const wait = param === 'duration_ms'
  const scale = wait ? 1 : 1000 // stored ms per shown unit
  const unit = wait ? 'ms' : 's'
  // The catalog's bounds; the known ones until it has loaded this device's commands.
  const spec: Pick<ParamSpec, 'minimum' | 'maximum' | 'default'> = commandFor(action, catalog)?.params.find((p) => p.name === param) ?? (wait ? WAIT_MS : WAIT_CONNECTED_MS)
  const ms = action.params[param] ?? spec.default
  const [text, setText] = useState(typeof ms === 'number' ? String(ms / scale) : '')
  const range = spec.minimum !== undefined && spec.maximum !== undefined ? ([spec.minimum / scale, spec.maximum / scale] as const) : null
  const span = range && `${range[0]}–${range[1]} ${unit}`
  const errorOf = (t: string) => {
    const n = Number(t)
    if (t.trim() === '' || !Number.isFinite(n)) return wait ? 'Enter a number of milliseconds' : 'Enter a number of seconds'
    return range && (n < range[0] || n > range[1]) ? span : null
  }
  const error = errorOf(text)
  useEffect(() => onValid(!error), [error, onValid])
  const id = useId()
  return (
    <Field label={wait ? 'Wait (ms)' : `Wait for ${deviceName(action, catalog)} to connect, at most (s)`} htmlFor={id} error={error} hint={span ?? undefined}>
      <Input
        id={id}
        type="number"
        inputMode={wait ? 'numeric' : 'decimal'}
        min={range?.[0]}
        max={range?.[1]}
        step={wait ? 100 : 0.1}
        value={text}
        autoFocus
        aria-invalid={!!error}
        className="w-28"
        onChange={(e) => {
          const t = e.target.value
          setText(t)
          if (!errorOf(t)) onChange({ ...action, params: { ...action.params, [param]: Math.round(Number(t) * scale) } })
        }}
      />
    </Field>
  )
}
