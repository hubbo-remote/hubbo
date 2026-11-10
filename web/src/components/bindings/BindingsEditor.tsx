import { AlertTriangle, Eraser, Pencil, Plus } from 'lucide-react'
import { useEffect, useMemo, useState } from 'react'
import type { Action, Bindings, EventKind, ValidationIssue } from '@/api/types'
import { ActionPicker, TestButton } from '@/components/actions/ActionPicker'
import { commandLabel, deviceName, paramSummary } from '@/components/actions/describe'
import { EV_COLOR, EventLegend, RemoteSvg } from '@/components/remote/RemoteSvg'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { boundEvents, buttonForPath, commandFor, EVENT_ORDER, isSystem, releaseFor, triggerKey, type Catalog } from '@/lib/activity'
import { BUTTONS, buttonLabel } from '@/lib/buttons'
import { cn } from '@/lib/utils'

const EVENT_HELP: Record<EventKind, string> = {
  down: 'on press',
  up: 'on release',
  tap: 'quick press',
  hold: 'press and hold',
}

/**
 * Clickable remote plus the selected button's down/up/tap/hold bindings.
 * The UI edits one action per trigger; the wire format is always an array.
 */
export function BindingsEditor({
  bindings,
  onChange,
  catalog,
  errors,
  warnings,
  selected,
  onSelect,
  fallback,
}: {
  bindings: Bindings
  onChange: (b: Bindings) => void
  catalog: Catalog
  errors: ValidationIssue[]
  /** Advisory only: shown, but never mark a button as broken. */
  warnings: ValidationIssue[]
  selected: string | null
  onSelect: (id: string) => void
  /** Bindings that apply when this set leaves a trigger unbound (global bindings, shown greyed). */
  fallback?: Bindings
}) {
  const bound = useMemo(() => {
    const out: Record<string, EventKind[]> = {}
    for (const b of BUTTONS) {
      const ev = boundEvents(bindings, b.id)
      if (ev.length) out[b.id] = ev
    }
    return out
  }, [bindings])
  // Keyed on the joined ids so a re-validation with the same result keeps the Set (and RemoteSvg's memo) stable.
  const errorKey = [...new Set(errors.map((e) => buttonForPath(e.path)).filter(Boolean))].sort().join(' ')
  const errorButtons = useMemo(() => new Set(errorKey ? errorKey.split(' ') : []), [errorKey])

  const setBinding = (button: string, event: EventKind, action: Action | null) => {
    const next = { ...bindings }
    const key = triggerKey(button, event)
    if (action) next[key] = [action]
    else delete next[key]
    // Key-like convenience: a key pressed on down is released on up.
    // A hold (e.g. key down) is released on up, so the key never sticks.
    if (event === 'down' && action && commandFor(action, catalog)?.effect === 'hold') {
      const upKey = triggerKey(button, 'up')
      const up = next[upKey]?.[0]
      if (!up || up.device_id === action.device_id) next[upKey] = [releaseFor(action)]
    }
    onChange(next)
  }

  return (
    <div className="grid gap-4 md:grid-cols-[minmax(13rem,17rem)_minmax(0,1fr)] lg:gap-6">
      <div className="md:sticky md:top-4 md:self-start">
        <Card className="px-6 pt-5 pb-4">
          <RemoteSvg
            bound={bound}
            selected={selected}
            onSelect={onSelect}
            errors={errorButtons}
            className="mx-auto max-w-[19rem] md:max-h-[calc(100dvh-9rem)] md:max-w-[15rem]" // phones: full width, so keys are big enough to tap
            label="Remote"
          />
          <EventLegend className="mt-3 justify-center" />
        </Card>
      </div>
      <div className="grid content-start gap-4">
        {selected ? (
          <BindingPanel
            button={selected}
            bindings={bindings}
            fallback={fallback}
            catalog={catalog}
            errors={errors.filter((e) => buttonForPath(e.path) === selected)}
            warnings={warnings.filter((e) => buttonForPath(e.path) === selected)}
            onChange={(ev, a) => setBinding(selected, ev, a)}
          />
        ) : (
          <Card>
            <CardContent className="py-10 text-center text-sm text-muted-foreground">Select a button on the remote.</CardContent>
          </Card>
        )}
        <BoundList bound={bound} selected={selected} onSelect={onSelect} errorButtons={errorButtons} />
      </div>
    </div>
  )
}

function BindingPanel({ button, bindings, fallback, catalog, errors, warnings, onChange }: { button: string; bindings: Bindings; fallback?: Bindings; catalog: Catalog; errors: ValidationIssue[]; warnings: ValidationIssue[]; onChange: (e: EventKind, a: Action | null) => void }) {
  const [editing, setEditing] = useState<EventKind | null>(null)
  const [pending, setPending] = useState<Action | null>(null)
  useEffect(() => setEditing(null), [button])

  const begin = (ev: EventKind) => {
    setEditing(ev)
    setPending(bindings[triggerKey(button, ev)]?.[0] ?? null)
  }

  return (
    <Card aria-live="polite">
      <CardHeader>
        <div>
          <CardTitle className="text-lg">{buttonLabel(button)}</CardTitle>
        </div>
      </CardHeader>
      <CardContent className="grid gap-2">
        {errors.map((i) => (
          <Alert key={i.path + i.code} variant="error">
            {i.message}
          </Alert>
        ))}
        {warnings.map((i) => (
          <Alert key={i.path + i.code} variant="warning">
            {i.message}
          </Alert>
        ))}
        {EVENT_ORDER.map((ev) => {
          const key = triggerKey(button, ev)
          const action = bindings[key]?.[0] ?? null
          const inherited = !action ? fallback?.[key]?.[0] : undefined
          const isEditing = editing === ev
          return (
            <div key={ev} className={cn('rounded-lg border p-3 transition-colors', isEditing && 'border-primary/60 bg-accent/30', !action && !isEditing && 'border-dashed')}>
              <div className="flex items-center gap-3">
                <span className="size-3 shrink-0 rounded-full ring-2 ring-background" style={{ background: EV_COLOR[ev] }} aria-hidden />
                <div className="min-w-0 flex-1">
                  <p className="text-sm">
                    <span className="font-semibold capitalize">{ev}</span>
                    <span className="text-muted-foreground"> · {EVENT_HELP[ev]}</span>
                  </p>
                  {!isEditing && (
                    <p className={cn('truncate text-sm', !action && 'text-muted-foreground')}>
                      {action ? (
                        <ActionText action={action} catalog={catalog} />
                      ) : inherited ? (
                        <span className="italic">
                          Global: <ActionText action={inherited} catalog={catalog} />
                        </span>
                      ) : (
                        'Nothing'
                      )}
                    </p>
                  )}
                </div>
                {!isEditing && (
                  <div className="flex shrink-0 gap-1">
                    {action && (
                      <Button variant="ghost" size="icon" className="size-8 hover:text-destructive" aria-label={`Clear ${ev} binding`} onClick={() => onChange(ev, null)}>
                        <Eraser />
                      </Button>
                    )}
                    <Button variant={action ? 'ghost' : 'outline'} size="sm" onClick={() => begin(ev)} aria-label={`${action ? 'Edit' : 'Bind'} ${buttonLabel(button)} ${ev}`}>
                      {action ? <Pencil /> : <Plus />}
                      {action ? 'Edit' : 'Bind'}
                    </Button>
                  </div>
                )}
              </div>
              {isEditing && (
                <div className="mt-3 grid gap-3 border-t pt-3">
                  <ActionPicker value={pending} onChange={setPending} catalog={catalog} context="binding" allowHold={ev === 'down'} testable={false} />
                  <div className="flex flex-wrap items-center gap-2">
                    <Button
                      size="sm"
                      disabled={!!pending && !pending.command_id}
                      onClick={() => {
                        onChange(ev, pending)
                        setEditing(null)
                      }}
                    >
                      Apply
                    </Button>
                    <Button size="sm" variant="ghost" onClick={() => setEditing(null)}>
                      Cancel
                    </Button>
                    {pending?.device_id && pending.command_id && commandFor(pending, catalog)?.effect === 'momentary' && (
                      <div className="ml-auto">
                        <TestButton action={pending} />
                      </div>
                    )}
                  </div>
                  {pending && commandFor(pending, catalog)?.effect === 'hold' && <p className="text-xs text-muted-foreground">The release is added to Up.</p>}
                </div>
              )}
            </div>
          )
        })}
      </CardContent>
    </Card>
  )
}

function ActionText({ action, catalog }: { action: Action; catalog: Catalog }) {
  const summary = paramSummary(action, catalog)
  return (
    <>
      {!isSystem(action) && <span className="font-medium">{deviceName(action, catalog)} → </span>}
      {commandLabel(action, catalog)}
      {summary && <span className="text-muted-foreground"> ({summary})</span>}
    </>
  )
}

function BoundList({ bound, selected, onSelect, errorButtons }: { bound: Record<string, EventKind[]>; selected: string | null; onSelect: (id: string) => void; errorButtons: Set<string> }) {
  const ids = BUTTONS.filter((b) => bound[b.id]).map((b) => b.id)
  if (!ids.length) return <p className="text-sm text-muted-foreground">No buttons set up yet.</p>
  return (
    <div>
      <h3 className="mb-2 text-sm font-medium">Bound buttons ({ids.length})</h3>
      <ul className="flex flex-wrap gap-1.5">
        {ids.map((id) => (
          <li key={id}>
            <button
              onClick={() => onSelect(id)}
              aria-pressed={selected === id}
              className={cn(
                'inline-flex items-center gap-1.5 rounded-md border bg-card px-2 py-1 text-xs outline-none hover:bg-accent focus-visible:ring-2 focus-visible:ring-ring cursor-pointer',
                selected === id && 'border-primary bg-primary/10',
                errorButtons.has(id) && 'border-destructive text-destructive',
              )}
            >
              {errorButtons.has(id) && <AlertTriangle className="size-3" />}
              {buttonLabel(id)}
              <span className="flex gap-0.5" aria-label={bound[id]!.join(', ')}>
                {bound[id]!.map((e) => (
                  <span key={e} className="size-1.5 rounded-full" style={{ background: EV_COLOR[e] }} />
                ))}
              </span>
            </button>
          </li>
        ))}
      </ul>
    </div>
  )
}
