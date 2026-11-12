import { AlertTriangle, Eraser, Pencil, Plus } from 'lucide-react'
import { useEffect, useMemo, useState, type ReactNode } from 'react'
import type { Action, Bindings, ButtonId, EventKind, ValidationIssue } from '@/api/types'
import { ActionPicker, TestButton } from '@/components/actions/ActionPicker'
import { CommandPicker } from '@/components/actions/CommandPicker'
import { commandLabel, deviceName, paramSummary } from '@/components/actions/describe'
import { EV_COLOR, EventLegend, RemoteSvg } from '@/components/remote/RemoteSvg'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { buttonForPath, commandFor, isSystem, releaseFor, triggerKey, type Catalog } from '@/lib/activity'
import { readButton, viewKinds, writeLongPress, writeMain, type Mode } from '@/lib/commands'
import { BUTTONS, buttonLabel } from '@/lib/buttons'
import { cn } from '@/lib/utils'

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
      const ev = viewKinds(readButton(bindings, b.id))
      if (ev.length) out[b.id] = ev
    }
    return out
  }, [bindings])
  // Keyed on the joined ids so a re-validation with the same result keeps the Set (and RemoteSvg's memo) stable.
  const errorKey = [...new Set(errors.map((e) => buttonForPath(e.path)).filter(Boolean))].sort().join(' ')
  const errorButtons = useMemo(() => new Set(errorKey ? errorKey.split(' ') : []), [errorKey])

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
            button={selected as ButtonId}
            bindings={bindings}
            fallback={fallback}
            catalog={catalog}
            errors={errors.filter((e) => buttonForPath(e.path) === selected)}
            warnings={warnings.filter((e) => buttonForPath(e.path) === selected)}
            onChange={onChange}
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

/** A row's look: its colour dot, title, and what it does now (or the global one it falls back to). */
function Row({ color, title, action, inherited, catalog, editing, children, actions }: { color: string; title: string; action: Action | Action[] | null; inherited?: Action | null; catalog: Catalog; editing: boolean; children?: ReactNode; actions: ReactNode }) {
  const list = Array.isArray(action) ? action : action ? [action] : []
  return (
    <div className={cn('rounded-lg border p-3 transition-colors', editing && 'border-primary/60 bg-accent/30', !list.length && !editing && 'border-dashed')}>
      <div className="flex items-center gap-3">
        <span className="size-3 shrink-0 rounded-full ring-2 ring-background" style={{ background: color }} aria-hidden />
        <div className="min-w-0 flex-1">
          <p className="text-sm font-semibold">{title}</p>
          {!editing && (
            <p className={cn('truncate text-sm', !list.length && 'text-muted-foreground')}>
              {list.length ? (
                list.map((a, i) => (
                  <span key={i}>
                    {i > 0 && ', then '}
                    <ActionText action={a} catalog={catalog} />
                  </span>
                ))
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
        {!editing && <div className="flex shrink-0 gap-1">{actions}</div>}
      </div>
      {editing && <div className="mt-3 grid gap-3 border-t pt-3">{children}</div>}
    </div>
  )
}

function RowButtons({ bound, label, onEdit, onClear }: { bound: boolean; label: string; onEdit: () => void; onClear: () => void }) {
  return (
    <>
      {bound && (
        <Button variant="ghost" size="icon" className="size-8 hover:text-destructive" aria-label={`Clear ${label}`} onClick={onClear}>
          <Eraser />
        </Button>
      )}
      <Button variant={bound ? 'ghost' : 'outline'} size="sm" onClick={onEdit} aria-label={`${bound ? 'Edit' : 'Bind'} ${label}`}>
        {bound ? <Pencil /> : <Plus />}
        {bound ? 'Edit' : 'Bind'}
      </Button>
    </>
  )
}

function EditButtons({ ok, onApply, onCancel, test }: { ok: boolean; onApply: () => void; onCancel: () => void; test?: Action | null }) {
  return (
    <div className="flex flex-wrap items-center gap-2">
      <Button size="sm" disabled={!ok} onClick={onApply}>
        Apply
      </Button>
      <Button size="sm" variant="ghost" onClick={onCancel}>
        Cancel
      </Button>
      {test && (
        <div className="ml-auto">
          <TestButton action={test} />
        </div>
      )}
    </div>
  )
}

type Editing = { row: 'main'; mode: Mode; action: Action | null } | { row: 'long'; action: Action | null } | { row: EventKind; action: Action | null }

/**
 * The selected button: what it does on a press (or while held), on a long press, and, for anything
 * that doesn't fit those, its raw down/up/tap triggers ("Advanced").
 */
function BindingPanel({ button, bindings, fallback, catalog, errors, warnings, onChange }: { button: ButtonId; bindings: Bindings; fallback?: Bindings; catalog: Catalog; errors: ValidationIssue[]; warnings: ValidationIssue[]; onChange: (b: Bindings) => void }) {
  const [editing, setEditing] = useState<Editing | null>(null)
  const [advanced, setAdvanced] = useState(false)
  useEffect(() => (setEditing(null), setAdvanced(false)), [button])

  const view = readButton(bindings, button)
  const inherited = fallback ? readButton(fallback, button) : null
  const forced = view.raw.some((e) => e !== 'hold') // doesn't fit Press/Hold: only the raw rows can show it
  const showRaw = forced || advanced
  const rawRows: EventKind[] = [...(showRaw ? (['down', 'up', 'tap'] as const) : []), ...(view.raw.includes('hold') ? (['hold'] as const) : [])]
  const close = () => setEditing(null)
  const testable = (a: Action | null) => (a?.device_id && a.command_id && commandFor(a, catalog)?.effect === 'momentary' ? a : null)

  // A raw trigger edits its first action and keeps the rest; a hold on down gets its release on up.
  const setRaw = (ev: EventKind, action: Action | null) => {
    const key = triggerKey(button, ev)
    const next = { ...bindings }
    if (action) next[key] = [action, ...(bindings[key] ?? []).slice(1)]
    else delete next[key]
    if (ev === 'down' && action && commandFor(action, catalog)?.effect === 'hold') {
      const upKey = triggerKey(button, 'up')
      const up = next[upKey]?.[0]
      if (!up || up.device_id === action.device_id) next[upKey] = [releaseFor(action)]
    }
    onChange(next)
  }

  return (
    <Card aria-live="polite">
      <CardHeader>
        <div className="flex items-center justify-between gap-2">
          <CardTitle className="text-lg">{buttonLabel(button)}</CardTitle>
          {!forced && (
            <Button variant="link" size="sm" className="h-auto p-0 text-muted-foreground" onClick={() => (close(), setAdvanced(!advanced))} aria-pressed={advanced}>
              {advanced ? 'Simple' : 'Advanced'}
            </Button>
          )}
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
        {!showRaw && (
          <Row
            color={EV_COLOR[view.main?.mode === 'hold' ? 'down' : 'tap']}
            title={view.main?.mode === 'hold' ? 'Hold' : 'Press'}
            action={view.main?.action ?? null}
            inherited={inherited?.main?.action}
            catalog={catalog}
            editing={editing?.row === 'main'}
            actions={<RowButtons bound={!!view.main} label={`${buttonLabel(button)} press`} onEdit={() => setEditing({ row: 'main', mode: view.main?.mode ?? 'press', action: view.main?.action ?? null })} onClear={() => onChange(writeMain(bindings, button, null))} />}
          >
            {editing?.row === 'main' && (
              <>
                <CommandPicker value={editing.action} onChange={(action) => setEditing({ ...editing, action })} catalog={catalog} context="binding" testable={false} />
                {editing.action?.device_id && (
                  <div role="radiogroup" aria-label="When" className="flex gap-1.5">
                    {(['press', 'hold'] as const).map((m) => (
                      <label key={m} className="flex cursor-pointer items-center gap-2 rounded-md border px-3 py-1.5 text-sm has-[:checked]:border-primary has-[:checked]:bg-primary/5">
                        <input type="radio" name={`mode-${button}`} className="accent-[var(--color-primary)]" checked={editing.mode === m} onChange={() => setEditing({ ...editing, mode: m })} />
                        {m === 'press' ? 'Press' : 'Hold'}
                      </label>
                    ))}
                  </div>
                )}
                <EditButtons
                  ok={!editing.action || !!editing.action.command_id}
                  onApply={() => (onChange(writeMain(bindings, button, editing.action && { mode: editing.action.device_id ? editing.mode : 'press', action: editing.action })), close())}
                  onCancel={close}
                  test={testable(editing.action)}
                />
              </>
            )}
          </Row>
        )}
        {!view.raw.includes('hold') && (
          <Row
            color={EV_COLOR.hold}
            title="Long press"
            action={view.longPress}
            inherited={inherited?.longPress}
            catalog={catalog}
            editing={editing?.row === 'long'}
            actions={<RowButtons bound={!!view.longPress} label={`${buttonLabel(button)} long press`} onEdit={() => setEditing({ row: 'long', action: view.longPress })} onClear={() => onChange(writeLongPress(bindings, button, null))} />}
          >
            {editing?.row === 'long' && (
              <>
                <CommandPicker value={editing.action} onChange={(action) => setEditing({ ...editing, action })} catalog={catalog} context="binding" testable={false} />
                <EditButtons ok={!editing.action || !!editing.action.command_id} onApply={() => (onChange(writeLongPress(bindings, button, editing.action)), close())} onCancel={close} test={testable(editing.action)} />
              </>
            )}
          </Row>
        )}
        {rawRows.length > 0 && <p className="mt-2 text-xs font-medium text-muted-foreground">Advanced</p>}
        {rawRows.map((ev) => {
          const list = bindings[triggerKey(button, ev)] ?? []
          const isEditing = editing?.row === ev
          return (
            <Row
              key={ev}
              color={EV_COLOR[ev]}
              title={RAW_TITLE[ev]}
              action={list}
              inherited={fallback?.[triggerKey(button, ev)]?.[0]}
              catalog={catalog}
              editing={isEditing}
              actions={<RowButtons bound={list.length > 0} label={`${buttonLabel(button)} ${ev}`} onEdit={() => setEditing({ row: ev, action: list[0] ?? null })} onClear={() => setRaw(ev, null)} />}
            >
              {editing && isEditing && (
                <>
                  <ActionPicker value={editing.action} onChange={(action) => setEditing({ ...editing, action })} catalog={catalog} context="binding" allowHold={ev === 'down'} testable={false} />
                  <EditButtons ok={!editing.action || !!editing.action.command_id} onApply={() => (setRaw(ev, editing.action), close())} onCancel={close} test={testable(editing.action)} />
                  {editing.action && commandFor(editing.action, catalog)?.effect === 'hold' && <p className="text-xs text-muted-foreground">The release is added to Up.</p>}
                </>
              )}
            </Row>
          )
        })}
      </CardContent>
    </Card>
  )
}

const RAW_TITLE: Record<EventKind, string> = { down: 'Down', up: 'Up', tap: 'Tap', hold: 'Long press' }

function ActionText({ action, catalog }: { action: Action; catalog: Catalog }) {
  const summary = paramSummary(action, catalog)
  return (
    <>
      {!isSystem(action) && <span className="font-medium">{deviceName(action, catalog)} → </span>}
      {commandLabel(action, catalog)}
      {summary && <span className="text-muted-foreground"> {summary}</span>}
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
