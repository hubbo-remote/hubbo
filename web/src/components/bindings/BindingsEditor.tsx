import { AlertTriangle, Eraser, Pencil, Plus } from 'lucide-react'
import { useCallback, useEffect, useMemo, useState, type ReactNode } from 'react'
import type { Action, Bindings, ButtonId, EventKind, ValidationIssue } from '@/api/types'
import { ActionPicker, TestButton } from '@/components/actions/ActionPicker'
import { CommandPicker } from '@/components/actions/CommandPicker'
import { actionParts, actionText } from '@/components/actions/describe'
import { EV_COLOR, EV_LABEL, EventLegend, RemoteSvg, SourceLegend, type BindingSource } from '@/components/remote/RemoteSvg'
import { Alert } from '@/components/ui/alert'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { bindsButton, buttonForPath, commandFor, isTimed, releaseFor, triggerKey, type Catalog } from '@/lib/activity'
import { readButton, viewKinds, writeLongPress, writeMain } from '@/lib/commands'
import { BUTTONS, buttonLabel } from '@/lib/buttons'
import { cn, same } from '@/lib/utils'
import { Sheet, SheetSpacer } from '@/components/ui/sheet'
import { useWide } from '@/hooks/media'

/**
 * Clickable remote plus the selected button's down/up/tap/hold bindings.
 * The UI edits one action per trigger; the wire format is always an array.
 */
/**
 * Whose buttons these are. The global set is the base layer; an activity's set always shows the globals behind
 * the buttons it leaves unbound, so both hosts (the activity editor and the Remote page) render it the same.
 */
export type BindingSet = { source: 'global'; globals?: undefined } | { source: 'activity'; globals: Bindings }

/** The button shown before any pick. */
const FIRST_BUTTON: Record<BindingSource, ButtonId> = { activity: 'select', global: 'tv' }

export function BindingsEditor({
  bindings,
  onChange,
  catalog,
  errors,
  warnings,
  selected: picked,
  onSelect,
  set,
}: {
  bindings: Bindings
  onChange: (b: Bindings) => void
  catalog: Catalog
  errors: ValidationIssue[]
  /** Advisory only: shown, but never mark a button as broken. */
  warnings: ValidationIssue[]
  /** The picked button; null until the first pick (the set's first button shows). */
  selected: string | null
  onSelect: (id: string) => void
  set: BindingSet
}) {
  const { source, globals: fallback } = set
  const selected = picked ?? FIRST_BUTTON[source]
  const bound = useMemo(() => {
    const out: Record<string, EventKind[]> = {}
    for (const b of BUTTONS) {
      const ev = viewKinds(readButton(bindings, b.id))
      if (ev.length) out[b.id] = ev
    }
    return out
  }, [bindings])
  // Buttons this set leaves to a global binding (it binds nothing on them), with what the global does.
  const inherited = useMemo(() => {
    if (!fallback) return undefined
    const out: Record<string, { kinds: EventKind[]; text: string }> = {}
    for (const b of BUTTONS) {
      if (bindsButton(bindings, b.id) || !bindsButton(fallback, b.id)) continue
      const v = readButton(fallback, b.id)
      const parts = [
        v.main && `${EV_LABEL.tap} → ${actionText(v.main, catalog)}`,
        v.longPress && `${EV_LABEL.hold} → ${actionText(v.longPress, catalog)}`,
        ...v.raw.map((e) => `${EV_LABEL[e]} → ${fallback[triggerKey(b.id, e)]!.map((a) => actionText(a, catalog)).join(', ')}`),
      ]
      out[b.id] = { kinds: viewKinds(v), text: parts.filter(Boolean).join(' · ') }
    }
    return out
  }, [fallback, bindings, catalog])
  // Keyed on the joined ids so a re-validation with the same result keeps the Set (and RemoteSvg's memo) stable.
  const errorKey = [...new Set(errors.map((e) => buttonForPath(e.path)).filter(Boolean))].sort().join(' ')
  const errorButtons = useMemo(() => new Set(errorKey ? errorKey.split(' ') : []), [errorKey])

  // Phones: the remote fills the page and a tapped button's editor opens in a bottom sheet over it.
  const wide = useWide()
  const [sheet, setSheet] = useState(false)
  // A pick from the page (e.g. jumping to an error) opens the sheet too, not just a tap on a key.
  const [lastPicked, setLastPicked] = useState(picked)
  if (picked !== lastPicked) {
    setLastPicked(picked)
    if (picked) setSheet(true)
  }
  const pick = useCallback(
    (id: string) => {
      onSelect(id)
      setSheet(true)
    },
    [onSelect],
  )
  const panel = (
    <BindingPanel
      button={selected as ButtonId}
      bindings={bindings}
      fallback={fallback}
      catalog={catalog}
      errors={errors.filter((e) => buttonForPath(e.path) === selected)}
      warnings={warnings.filter((e) => buttonForPath(e.path) === selected)}
      onChange={onChange}
    />
  )

  return (
    // `--bind`: this set's accent (selected bound-list chip, the row being edited): blue, or amber for globals.
    <div className={cn('grid gap-4 md:grid-cols-[minmax(13rem,17rem)_minmax(0,1fr)] lg:gap-6', source === 'global' ? '[--bind:var(--global)]' : '[--bind:var(--primary)]')}>
      <div className="md:sticky md:top-4 md:self-start">
        <Card className="px-6 pt-5 pb-4">
          <RemoteSvg
            bound={bound}
            selected={selected}
            onSelect={pick}
            errors={errorButtons}
            inherited={inherited}
            source={source}
            className="mx-auto max-w-[19rem] md:max-h-[calc(100dvh-9rem)] md:max-w-[15rem]" // phones: full width, so keys are big enough to tap
            label="Remote"
          />
          <EventLegend className="mt-3 justify-center" />
          {fallback && <SourceLegend className="mt-1.5 justify-center" />}
        </Card>
      </div>
      <div className="grid content-start gap-4">
        {wide && panel}
        <BoundList bound={bound} selected={selected} onSelect={pick} errorButtons={errorButtons} />
      </div>
      {!wide && sheet && (
        <>
          <SheetSpacer />
          <Sheet label={`${buttonLabel(selected as ButtonId)} button`} anchor={selected} find={keyEl} onClose={() => setSheet(false)}>
            {panel}
          </Sheet>
        </>
      )}
    </div>
  )
}

/** A remote key's element, for the sheet to keep in view and return focus to. */
const keyEl = (id: string) => document.querySelector<SVGGElement>(`[data-key="${id}"]`)

/** A row's look: its colour dot, title, and what it does now (or the global one it falls back to). */
function Row({ color, title, note, action, inherited, catalog, editing, children, actions }: { color: string; title: string; note?: string; action: Action | Action[] | null; inherited?: Action | null; catalog: Catalog; editing: boolean; children?: ReactNode; actions: ReactNode }) {
  const list = Array.isArray(action) ? action : action ? [action] : []
  return (
    <div className={cn('rounded-lg border p-3 transition-colors', editing && 'border-[var(--bind)]/60 bg-accent/30', !list.length && !inherited && !editing && 'border-dashed')}>
      <div className="flex items-center gap-3">
        <span className="size-3 shrink-0 rounded-full ring-2 ring-background" style={{ background: color }} aria-hidden />
        <div className="min-w-0 flex-1">
          <p className="flex items-center gap-2 text-sm font-semibold">
            {title}
            {!list.length && inherited && <GlobalTag />}
            {note && <span className="text-xs font-normal text-muted-foreground">{note}</span>}
          </p>
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
                <ActionText action={inherited} catalog={catalog} />
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

/** Marks a row whose binding comes from the global buttons: amber, like the remote's key. */
function GlobalTag() {
  return (
    <Badge variant="outline" className="border-[var(--global)] px-1.5 py-0 text-[11px] leading-4 text-[var(--global)]">
      Global
    </Badge>
  )
}

/** Edit when bound; Change when a global applies here (it opens on the global's action); else Bind. */
function RowButtons({ bound, inherited, label, onEdit, onClear }: { bound: boolean; inherited?: boolean; label: string; onEdit: () => void; onClear: () => void }) {
  const verb = bound ? 'Edit' : inherited ? 'Change' : 'Bind'
  return (
    <>
      {bound && (
        <Button variant="ghost" size="icon" className="size-8 hover:text-destructive" aria-label={`Clear ${label}`} onClick={onClear}>
          <Eraser />
        </Button>
      )}
      <Button variant={bound || inherited ? 'ghost' : 'outline'} size="sm" onClick={onEdit} aria-label={`${verb} ${label}`}>
        {bound || inherited ? <Pencil /> : <Plus />}
        {verb}
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

/** `from`: the global action an inherited row opened on; applying it unchanged binds nothing. */
type Editing = { row: 'main' | 'long' | EventKind; action: Action | null; from?: Action }

/**
 * The selected button: what it does on a press (passed straight through), on a long press, and, for anything
 * that doesn't fit those, its raw down/up/tap triggers ("Advanced").
 */
function BindingPanel({ button, bindings, fallback, catalog, errors, warnings, onChange }: { button: ButtonId; bindings: Bindings; fallback?: Bindings; catalog: Catalog; errors: ValidationIssue[]; warnings: ValidationIssue[]; onChange: (b: Bindings) => void }) {
  const [editing, setEditing] = useState<Editing | null>(null)
  const [advanced, setAdvanced] = useState(false)
  useEffect(() => (setEditing(null), setAdvanced(false)), [button])

  const view = readButton(bindings, button)
  // A button the set binds at all masks every global for it (the hub's rule), so only an untouched button inherits.
  const owns = bindsButton(bindings, button)
  const inherited = fallback && !owns ? readButton(fallback, button) : null
  const timed = isTimed(catalog)
  const forced = view.raw.some((e) => e !== 'hold') // doesn't fit Press: only the raw rows can show it
  const showRaw = forced || advanced
  const rawRows: EventKind[] = [...(showRaw ? (['down', 'up', 'tap'] as const) : []), ...(view.raw.includes('hold') ? (['hold'] as const) : [])]
  const close = () => setEditing(null)
  const unchanged = (e: Editing) => !!e.from && same(e.action, e.from)
  const edit = (row: 'main' | 'long', own: Action | null, global: Action | null | undefined) =>
    setEditing(own ? { row, action: own } : global ? { row, action: global, from: global } : { row, action: null }) // unbound: nothing guessed
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
            color={EV_COLOR.tap}
            title="Press"
            note={view.main && view.longPress ? 'Sends on release' : undefined}
            action={view.main}
            inherited={inherited?.main}
            catalog={catalog}
            editing={editing?.row === 'main'}
            actions={<RowButtons bound={!!view.main} inherited={!!inherited?.main} label={`${buttonLabel(button)} press`} onEdit={() => edit('main', view.main, inherited?.main)} onClear={() => onChange(writeMain(bindings, button, null, timed))} />}
          >
            {editing?.row === 'main' && (
              <>
                <CommandPicker value={editing.action} onChange={(action) => setEditing({ ...editing, action })} catalog={catalog} context="binding" testable={false} />
                <EditButtons
                  ok={!!editing.action?.command_id}
                  onApply={() => (unchanged(editing) || onChange(writeMain(bindings, button, editing.action, timed)), close())}
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
            actions={<RowButtons bound={!!view.longPress} inherited={!!inherited?.longPress} label={`${buttonLabel(button)} long press`} onEdit={() => edit('long', view.longPress, inherited?.longPress)} onClear={() => onChange(writeLongPress(bindings, button, null, timed))} />}
          >
            {editing?.row === 'long' && (
              <>
                <CommandPicker value={editing.action} onChange={(action) => setEditing({ ...editing, action })} catalog={catalog} context="binding" testable={false} />
                <EditButtons ok={!!editing.action?.command_id} onApply={() => (unchanged(editing) || onChange(writeLongPress(bindings, button, editing.action, timed)), close())} onCancel={close} test={testable(editing.action)} />
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
              inherited={owns ? undefined : fallback?.[triggerKey(button, ev)]?.[0]}
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

/** `actionText`, styled: the device stands out, the settings are muted. */
function ActionText({ action, catalog }: { action: Action; catalog: Catalog }) {
  const p = actionParts(action, catalog)
  return (
    <>
      {p.device && <span className="font-medium">{p.device} → </span>}
      {p.command}
      {p.summary && <span className="text-muted-foreground"> {p.summary}</span>}
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
                selected === id && 'border-[var(--bind)] bg-[var(--bind)]/10',
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
