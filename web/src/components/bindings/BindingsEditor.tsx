import { AlertTriangle, Eraser, ListOrdered, Loader2, Pencil, Play, Plus, SlidersHorizontal } from 'lucide-react'
import { useCallback, useEffect, useId, useMemo, useState, type ReactNode } from 'react'
import type { Action, Bindings, ButtonId, EventKind, SlotId, SmartPress, SmartSlotActions, ValidationIssue } from '@/api/types'
import { ActionPicker, TestButton } from '@/components/actions/ActionPicker'
import { CommandPicker } from '@/components/actions/CommandPicker'
import { actionParts, actionText, plainActions } from '@/components/actions/describe'
import { EV_COLOR, EV_LABEL, EventLegend, RemoteSvg, SourceLegend, type BindingSource } from '@/components/remote/RemoteSvg'
import { Alert } from '@/components/ui/alert'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { Dialog } from '@/components/ui/dialog'
import { Field, Select } from '@/components/ui/input'
import { InfoTip } from '@/components/ui/info-tip'
import { bindsButton, buttonForPath, commandFor, isTimed, releaseFor, triggerKey, type Catalog } from '@/lib/activity'
import { callReady } from '@/lib/haCall'
import { readButton, viewKinds, writeLongPress, writeMain, type ButtonSet } from '@/lib/commands'
import { BUTTONS, buttonLabel } from '@/lib/buttons'
import { isSlot, SLOT_KINDS, SLOTS, type SmartSet } from '@/lib/smartButtons'
import { cn, plural, same } from '@/lib/utils'
import { Skeleton } from '@/components/ui/misc'
import { SequenceEditor } from '@/components/actions/SequenceEditor'
import { Sheet, SheetSpacer } from '@/components/ui/sheet'
import { useWide } from '@/hooks/media'
import { runMacro } from '@/hooks/runMacro'

/**
 * Clickable remote plus the selected button's down/up/tap/hold bindings.
 * The UI edits one action per trigger; the wire format is always an array.
 */
/**
 * Whose buttons these are. The global set is the base layer; an activity's set always shows the globals behind
 * the buttons it leaves unbound, so both hosts (the activity editor and the Remote page) render it the same.
 */
export type BindingSet = { source: 'global'; globals?: undefined; activityId?: undefined } | { source: 'activity'; globals: ButtonSet; activityId: string }

const pressKinds = (a: SmartPress): EventKind[] => [...(a.tap.length ? (['tap'] as const) : []), ...(a.hold.length ? (['hold'] as const) : [])]
const isPlusMinus = (b: string): b is 'plus' | 'minus' => b === 'plus' || b === 'minus'
const anyDevice = (smart: SmartSet) => SLOTS.some((s) => smart.slots[s])

/**
 * What the smart buttons give a button (undefined when they don't take it): a slot's own button once it has a
 * device, and + / − once any slot has one. `kinds` may be empty (a switch gives + and − nothing).
 */
function smartFor(smart: SmartSet | undefined, b: string): { kinds: EventKind[]; presses: SmartPress[] } | undefined {
  if (!smart) return undefined
  if (isSlot(b)) {
    if (!smart.slots[b]) return undefined
    const p = smart.actions(b)?.button
    return { kinds: p ? pressKinds(p) : [], presses: p ? [p] : [] }
  }
  if (!isPlusMinus(b) || !anyDevice(smart)) return undefined
  const presses = SLOTS.flatMap((s) => (smart.slots[s] && smart.actions(s)?.[b]) || [])
  return { kinds: [...new Set(presses.flatMap(pressKinds))], presses }
}

/** The button shown before any pick. */
const FIRST_BUTTON: Record<BindingSource, ButtonId> = { activity: 'select', global: 'tv' }

export function BindingsEditor({
  bindings,
  macros,
  onChange,
  catalog,
  errors,
  warnings,
  selected: picked,
  onSelect,
  set,
  smart,
  onPickSlot,
}: {
  bindings: Bindings
  /** Which of `bindings`' triggers are macros. */
  macros: string[]
  onChange: (b: ButtonSet) => void
  catalog: Catalog
  errors: ValidationIssue[]
  /** Advisory only: shown, but never mark a button as broken. */
  warnings: ValidationIssue[]
  /** The picked button; null until the first pick (the set's first button shows). */
  selected: string | null
  onSelect: (id: string) => void
  set: BindingSet
  /**
   * The smart buttons (Light 1, Light 2, Switch 1 and Switch 2's devices). On Global, a slot with a device does what
   * the hub derives (shown read-only), and `onPickSlot` sets it; in an activity they show as inherited.
   */
  smart?: SmartSet
  onPickSlot?: (slot: SlotId, device: string | null) => void
}) {
  const { source, globals: fallback } = set
  const own = useMemo(() => ({ bindings, macros }), [bindings, macros])
  const selected = picked ?? FIRST_BUTTON[source]
  const bound = useMemo(() => {
    const out: Record<string, EventKind[]> = {}
    for (const b of BUTTONS) {
      const ev = viewKinds(readButton(own, b.id))
      if (ev.length) out[b.id] = ev
    }
    // On Global, what the smart buttons take is theirs (an activity shows it as inherited instead).
    if (source !== 'global') return out
    for (const b of [...SLOTS, 'plus', 'minus']) {
      const sm = smartFor(smart, b)
      if (!sm) continue
      const ev = new Set([...(isSlot(b) ? [] : (out[b] ?? [])), ...sm.kinds])
      if (ev.size) out[b] = [...ev]
      else delete out[b]
    }
    return out
  }, [own, smart, source])
  // Press / Long press macros, marked on the remote: "<button>:tap" / "<button>:hold".
  const macroMarks = useMemo(() => {
    const out = new Set<string>()
    for (const b of BUTTONS) {
      const v = readButton(own, b.id)
      if (v.pressMacro) out.add(`${b.id}:tap`)
      if (v.longMacro) out.add(`${b.id}:hold`)
    }
    return out
  }, [own])
  // Buttons this set leaves to a global binding (it binds nothing on them), with what the global does.
  const inherited = useMemo(() => {
    if (!fallback) return undefined
    const out: Record<string, { kinds: EventKind[]; text: string }> = {}
    for (const b of BUTTONS) {
      if (bindsButton(bindings, b.id)) continue
      // The smart buttons come before Global (an activity's own bindings before both).
      const sm = smartFor(smart, b.id)
      if (sm) {
        if (sm.kinds.length) out[b.id] = { kinds: sm.kinds, text: `Smart button: ${sm.presses.map((p) => pressText(p, catalog)).filter(Boolean).join(' / ')}` }
        continue
      }
      if (!bindsButton(fallback.bindings, b.id)) continue
      const v = readButton(fallback, b.id)
      const parts = [
        v.press.length && `${EV_LABEL.tap} → ${v.press.map((a) => actionText(a, catalog)).join(', then ')}`,
        v.longPress.length && `${EV_LABEL.hold} → ${v.longPress.map((a) => actionText(a, catalog)).join(', then ')}`,
        ...v.raw.map((e) => `${EV_LABEL[e]} → ${fallback.bindings[triggerKey(b.id, e)]!.map((a) => actionText(a, catalog)).join(', ')}`),
      ]
      out[b.id] = { kinds: viewKinds(v), text: parts.filter(Boolean).join(' · ') }
    }
    return out
  }, [fallback, bindings, catalog, smart])
  // Keyed on the joined ids so a re-validation with the same result keeps the Set (and RemoteSvg's memo) stable.
  const errorKey = [...new Set(errors.map((e) => buttonForPath(e.path, macros)).filter(Boolean))].sort().join(' ')
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
      set={own}
      fallback={fallback}
      activityId={set.activityId ?? null}
      smart={smart}
      onPickSlot={onPickSlot}
      catalog={catalog}
      errors={errors.filter((e) => buttonForPath(e.path, macros) === selected)}
      warnings={warnings.filter((e) => buttonForPath(e.path, macros) === selected)}
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
            macros={macroMarks}
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
function Row({ color, title, note, action, inherited, macro, catalog, editing, children, actions }: { color: string; title: string; note?: string; action: Action | Action[] | null; inherited?: Action | Action[] | null; /** What it shows is a macro (an inherited one). */ macro?: boolean; catalog: Catalog; editing: boolean; children?: ReactNode; actions: ReactNode }) {
  const list = Array.isArray(action) ? action : action ? [action] : []
  const from = Array.isArray(inherited) ? inherited : inherited ? [inherited] : []
  return (
    <div className={cn('rounded-lg border p-3 transition-colors', editing && 'border-[var(--bind)]/60 bg-accent/30', !list.length && !inherited && !editing && 'border-dashed')}>
      <div className="flex items-center gap-3">
        <span className="size-3 shrink-0 rounded-full ring-2 ring-background" style={{ background: color }} aria-hidden />
        <div className="min-w-0 flex-1">
          <p className="flex items-center gap-2 text-sm font-semibold">
            {title}
            {!list.length && from.length > 0 && <SourceTag>Global</SourceTag>}
            {macro && <MacroTag />}
            {note && <span className="text-xs font-normal text-muted-foreground">{note}</span>}
          </p>
          {!editing && (
            <p className={cn('truncate text-sm', !list.length && 'text-muted-foreground')}>
              {list.length || from.length ? (
                (list.length ? list : from).map((a, i) => (
                  <span key={i}>
                    {i > 0 && ', then '}
                    <ActionText action={a} catalog={catalog} />
                  </span>
                ))
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

const TAG = 'px-1.5 py-0 text-[11px] leading-4'

/** Marks a Press or Long press that runs a list of commands. */
function MacroTag() {
  return (
    <Badge variant="outline" className={TAG}>
      Macro
    </Badge>
  )
}

/**
 * A Press or Long press macro: its commands as a short list, edited in place like a startup (add a command or a
 * wait, drag to reorder, remove). Clear asks first once it has any (a built macro is work to lose).
 */
function MacroRow({ color, title, note, list, label, name, catalog, issues, path, startAdding, onChange, onClear, onRun }: { color: string; title: string; note?: string; list: Action[]; label: string; /** "Guide", "Guide long press": what the confirm calls it. */ name: string; catalog: Catalog; /** The button's issues; those under `path` (its binding) mark their step. */ issues: ValidationIssue[]; path: string; startAdding: boolean; onChange: (list: Action[]) => void; onClear: () => void; /** Runs it as shown; resolves when done. */ onRun: () => Promise<void> }) {
  const [confirm, setConfirm] = useState(false)
  const [running, setRunning] = useState(false)
  const run = () => (setRunning(true), void onRun().finally(() => setRunning(false)))
  return (
    <div className="grid gap-3 rounded-lg border border-[var(--bind)]/40 p-3">
      <div className="flex items-center gap-3">
        <span className="size-3 shrink-0 rounded-full ring-2 ring-background" style={{ background: color }} aria-hidden />
        <p className="flex min-w-0 flex-1 flex-wrap items-center gap-2 text-sm font-semibold">
          {title}
          <MacroTag />
          {note && <span className="text-xs font-normal text-muted-foreground">{note}</span>}
        </p>
        {list.length > 0 && (
          <Button variant="ghost" size="icon" className="size-8" aria-label={`Run ${label}`} title="Run this macro" disabled={running} onClick={run}>
            {running ? <Loader2 className="animate-spin" /> : <Play />}
          </Button>
        )}
        <Button variant="ghost" size="icon" className="size-8 hover:text-destructive" aria-label={`Clear ${label}`} onClick={() => (list.length ? setConfirm(true) : onClear())}>
          <Eraser />
        </Button>
      </div>
      <SequenceEditor label={title} actions={list} onChange={onChange} catalog={catalog} issues={issues} pathPrefix={path} noun="command" startAdding={startAdding} />
      <Dialog
        open={confirm}
        onClose={() => setConfirm(false)}
        title={`Clear the ${name} macro?`}
        description={`Its ${plural(list.length, 'step')} will be removed.`}
        footer={
          <>
            <Button variant="ghost" onClick={() => setConfirm(false)}>
              Cancel
            </Button>
            <Button variant="destructive" onClick={() => (setConfirm(false), onClear())}>
              Clear
            </Button>
          </>
        }
      />
    </div>
  )
}

/** Marks what a row gets from elsewhere ("Global", "Smart button"): amber, like the remote's key. */
function SourceTag({ children }: { children: ReactNode }) {
  return (
    <Badge variant="outline" className={cn(TAG, 'border-[var(--global)] text-[var(--global)]')}>
      {children}
    </Badge>
  )
}

/** Edit when bound; Change when a global applies here (it opens on the global's action); else Bind. */
function RowButtons({ bound, label, onEdit, onClear }: { bound: boolean; label: string; onEdit: () => void; onClear: () => void }) {
  const verb = bound ? 'Edit' : 'Bind'
  return (
    <>
      {bound && (
        <Button variant="ghost" size="icon" className="size-8 hover:text-destructive" aria-label={`Clear ${label}`} onClick={onClear}>
          <Eraser />
        </Button>
      )}
      <Button variant={bound ? 'ghost' : 'outline'} size="sm" onClick={onEdit} aria-label={`${verb} ${label}`}>
        {bound ? <Pencil /> : <Plus />}
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
 * that doesn't fit those, its raw down/up/tap triggers ("Advanced triggers").
 */
function BindingPanel({ button, set, fallback, activityId, smart, onPickSlot, catalog, errors, warnings, onChange }: { button: ButtonId; set: ButtonSet; fallback?: ButtonSet; /** Whose macros run (null: Global's). */ activityId: string | null; smart?: SmartSet; onPickSlot?: (slot: SlotId, device: string | null) => void; catalog: Catalog; errors: ValidationIssue[]; warnings: ValidationIssue[]; onChange: (b: ButtonSet) => void }) {
  const { bindings } = set
  const [editing, setEditing] = useState<Editing | null>(null)
  const [advanced, setAdvanced] = useState(false)
  // An empty macro row (Macro just chosen, or its last command removed): the bindings have nothing to say yet.
  // `adding`: its add dialog opens.
  const [emptyMacro, setEmptyMacro] = useState<Partial<Record<'main' | 'long', { adding: boolean }>>>({})
  const markEmpty = (row: 'main' | 'long', m: { adding: boolean } | undefined) => setEmptyMacro((x) => ({ ...x, [row]: m }))
  useEffect(() => (setEditing(null), setAdvanced(false), setEmptyMacro({})), [button])

  const view = readButton(set, button)
  // A button the set binds at all masks every global for it (the hub's rule), so only an untouched button inherits.
  const owns = bindsButton(bindings, button)
  const global = !fallback
  // In an activity, the smart buttons stand in for Global on what they take (shown above the rows to override).
  const smartHere = !global && !!smartFor(smart, button)
  const inherited = fallback && !owns && !smartHere ? readButton(fallback, button) : null
  const timed = isTimed(catalog)
  const forced = view.raw.length > 0 // doesn't fit Press: only the raw rows can show it
  // Advanced (raw down/up/tap) is for single commands: not offered on a button with a macro, own or inherited.
  const hasMacro = view.pressMacro || view.longMacro || !!inherited?.pressMacro || !!inherited?.longMacro || !!(emptyMacro.main || emptyMacro.long)
  const showRaw = forced || (advanced && !hasMacro)
  const rawRows: EventKind[] = showRaw ? ['down', 'up', 'tap'] : []
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
    onChange({ bindings: next, macros: set.macros.filter((k) => next[k]?.length) })
  }

  /**
   * Press or Long press: a Command (one, passed through as today) or a Macro (a list, like a startup, at any
   * length). Unbound, the row offers both; to change which, clear it and choose again.
   */
  const pressRow = (row: 'main' | 'long') => {
    const main = row === 'main'
    const own = main ? view.press : view.longPress
    const ownMacro = main ? view.pressMacro : view.longMacro
    const from = (main ? inherited?.press : inherited?.longPress) ?? []
    const fromMacro = !!(main ? inherited?.pressMacro : inherited?.longMacro)
    const write = (list: Action[], macro: boolean) => onChange(main ? writeMain(set, button, list, { macro, timed }) : writeLongPress(set, button, list, { macro, timed }))
    const color = EV_COLOR[main ? 'tap' : 'hold']
    const title = main ? 'Press' : 'Long press'
    const label = `${buttonLabel(button)} ${main ? 'press' : 'long press'}`
    const note = main && view.press.length && view.longPress.length ? 'Sends on release' : undefined
    // The trigger the macro is on (none yet while it's empty).
    const macroKey = (main ? view.pressKey : triggerKey(button, 'hold')) ?? ''
    if (ownMacro || emptyMacro[row])
      return (
        <MacroRow
          key={row}
          color={color}
          title={title}
          note={note}
          list={own}
          label={label}
          name={main ? buttonLabel(button) : `${buttonLabel(button)} long press`}
          issues={errors}
          path={`/bindings/${macroKey}`}
          onRun={() => runMacro(own, macroKey, activityId)}
          catalog={catalog}
          startAdding={!!emptyMacro[row]?.adding}
          onChange={(list) => {
            // Its last command removed, it stays an empty macro here (the bindings can't hold an empty one).
            markEmpty(row, list.length ? undefined : { adding: false })
            write(list, true)
          }}
          onClear={() => (markEmpty(row, undefined), write([], false))}
        />
      )
    return (
      <Row
        key={row}
        color={color}
        title={title}
        note={note}
        action={own[0] ?? null}
        inherited={from.length ? from : null}
        macro={!own.length && fromMacro}
        catalog={catalog}
        editing={editing?.row === row}
        actions={
          own.length ? (
            <RowButtons bound label={label} onEdit={() => edit(row, own[0]!, undefined)} onClear={() => write([], false)} />
          ) : (
            // Unbound (or only inherited): a command or a macro. An inherited one opens on what Global does.
            <>
              <Button variant="outline" size="sm" onClick={() => edit(row, null, fromMacro ? null : from[0])}>
                <Plus /> Command<span className="sr-only"> for {label}</span>
              </Button>
              {!showRaw && (
                <Button
                  variant="outline"
                  size="sm"
                  onClick={() => {
                    close()
                    if (fromMacro) write(from, true)
                    else markEmpty(row, { adding: true })
                  }}
                >
                  <ListOrdered /> Macro<span className="sr-only"> for {label}</span>
                </Button>
              )}
            </>
          )
        }
      >
        {editing?.row === row && (
          <>
            <CommandPicker value={editing.action} onChange={(action) => setEditing({ ...editing, action })} catalog={catalog} context="binding" testable={false} />
            <EditButtons
              ok={!!editing.action?.command_id && callReady(editing.action)}
              onApply={() => (unchanged(editing) || write(editing.action ? [editing.action] : [], false), close())}
              onCancel={close}
              test={testable(editing.action)}
            />
          </>
        )}
      </Row>
    )
  }

  // On Global, a slot's button picks its device; with one, it (and + / −) only explain what they do.
  const slot = global && smart && onPickSlot && isSlot(button) ? button : null
  const picker = slot && <SlotDevice slot={slot} device={smart!.slots[slot]} onPick={onPickSlot!} catalog={catalog} />
  const explained = (slot && smart!.slots[slot]) || (global && smart && isPlusMinus(button) && anyDevice(smart))
  const title = (
    <CardHeader>
      <div className="flex flex-wrap items-center justify-between gap-2">
        <CardTitle className="text-lg">{buttonLabel(button)}</CardTitle>
        {!explained && (
          // Pressed and locked when the bindings only fit the raw triggers, so their rows showing isn't a mystery.
          <div className="flex items-center gap-1.5">
            <Button
              variant={showRaw ? 'secondary' : 'outline'}
              size="sm"
              className="border"
              aria-pressed={showRaw}
              disabled={forced || hasMacro}
              onClick={() => (close(), setAdvanced(!advanced))}
            >
              <SlidersHorizontal aria-hidden />
              Advanced triggers
            </Button>
            <InfoTip
              text={
                forced
                  ? 'On while this button has down, up or tap actions. Clear them to turn it off.'
                  : hasMacro
                    ? 'Not available for a button with a macro.'
                    : 'Set what the button does on down, up and tap separately.'
              }
              label="Advanced triggers"
            />
          </div>
        )}
      </div>
    </CardHeader>
  )
  if (explained)
    return (
      <Card aria-live="polite">
        {title}
        <CardContent className="grid gap-2">
          {picker}
          {slot ? <SlotActions slot={slot} actions={smart!.actions(slot)} catalog={catalog} /> : <Follows button={button as 'plus' | 'minus'} smart={smart!} catalog={catalog} />}
        </CardContent>
      </Card>
    )

  return (
    <Card aria-live="polite">
      {title}
      <CardContent className="grid gap-2">
        {picker}
        {smartHere && !owns && (isPlusMinus(button) ? <Follows button={button} smart={smart!} catalog={catalog} inherited /> : <SmartInherited slot={button as SlotId} smart={smart!} catalog={catalog} />)}
        {smartHere && owns && <p className="text-sm text-muted-foreground">This activity’s own, in place of the smart button{isPlusMinus(button) ? 's' : ''}.</p>}
        {smartHere && !owns && <p className="mt-2 text-xs font-medium text-muted-foreground">Bind it to use something else in this activity</p>}
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
        {!showRaw && pressRow('main')}
        {pressRow('long')}
        {rawRows.length > 0 && <p className="mt-2 text-xs font-medium text-muted-foreground">Advanced triggers</p>}
        {rawRows.map((ev) => {
          const list = bindings[triggerKey(button, ev)] ?? []
          const isEditing = editing?.row === ev
          return (
            <Row
              key={ev}
              color={EV_COLOR[ev]}
              title={RAW_TITLE[ev]}
              action={list}
              inherited={owns ? undefined : fallback?.bindings[triggerKey(button, ev)]?.[0]}
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

/** A smart button's device: lights, fans, covers and switches (and whatever it has now), or none. */
function SlotDevice({ slot, device, onPick, catalog }: { slot: SlotId; device: string | null; onPick: (slot: SlotId, device: string | null) => void; catalog: Catalog }) {
  const id = useId()
  const offered = [...catalog.devices.values()].filter((d) => SLOT_KINDS.has(d.kind) || d.id === device)
  return (
    <div className="mb-2">
      <Field label="Device" htmlFor={id} hint={device ? 'The button and + / − control it.' : 'None: bind it like any other button.'}>
        <Select id={id} value={device ?? ''} onChange={(e) => onPick(slot, e.target.value || null)}>
          <option value="">None</option>
          {offered.map((d) => (
            <option key={d.id} value={d.id}>
              {d.name}
            </option>
          ))}
        </Select>
      </Field>
    </div>
  )
}

/** + or −, once a smart button has a device: what it does for each one. In an activity it's inherited (tagged). */
function Follows({ button, smart, catalog, inherited }: { button: 'plus' | 'minus'; smart: SmartSet; catalog: Catalog; inherited?: boolean }) {
  return (
    <div className="grid gap-3">
      <p className="flex flex-wrap items-center gap-x-2 text-sm">
        {inherited && <SourceTag>Smart button</SourceTag>}
        <span className="font-semibold">Controls your smart buttons.</span> <span className="text-muted-foreground">+ and − adjust whichever one you pressed last.</span>
      </p>
      <SmartCards slots={SLOTS.filter((s) => smart.slots[s])} smart={smart} press={(a) => a[button]} nothing="+ and − do nothing" catalog={catalog} />
      {!inherited && <p className="text-xs text-muted-foreground">Activities can use + and − for something else, like volume.</p>}
    </div>
  )
}

/** An activity's slot button that its smart button handles: what it does there. */
function SmartInherited({ slot, smart, catalog }: { slot: SlotId; smart: SmartSet; catalog: Catalog }) {
  return (
    <div className="grid gap-2">
      <p className="flex items-center gap-2 text-sm">
        <SourceTag>Smart button</SourceTag>
        <span className="text-muted-foreground">Set on the Global remote.</span>
      </p>
      <SmartCards slots={[slot]} smart={smart} press={(a) => a.button} nothing="Does nothing" catalog={catalog} />
    </div>
  )
}

/** A press's actions in plain words: "Turn on; Hold: Turn off". */
const pressText = (p: SmartPress, cat: Catalog) => [p.tap.length && plainActions(p.tap, cat), p.hold.length && `Hold: ${plainActions(p.hold, cat)}`].filter(Boolean).join('; ')

/** One card per smart button: its badge and device on the left, what `press` picks out of its actions on the right. */
function SmartCards({ slots, smart, press, nothing, catalog }: { slots: SlotId[]; smart: SmartSet; press: (a: SmartSlotActions) => SmartPress; nothing: string; catalog: Catalog }) {
  return (
    <ul className="grid gap-2" aria-label="Smart buttons">
      {slots.map((slot) => {
        const device = smart.slots[slot]!
        const name = catalog.devices.get(device)?.name ?? device
        const actions = smart.actions(slot)
        const p = actions && press(actions)
        if (!p?.tap.length && !p?.hold.length)
          return (
            <li key={slot} className="px-1 text-sm text-muted-foreground">
              {buttonLabel(slot)} · {name}{p ? `: ${nothing}` : '…'}
            </li>
          )
        return (
          <li key={slot} className="flex items-center gap-3 rounded-lg border p-3">
            <div className="min-w-0 flex-1">
              <Badge variant="outline" className={TAG}>
                {buttonLabel(slot)}
              </Badge>
              <p className="truncate text-sm font-medium">{name}</p>
            </div>
            <div className="shrink-0 text-right text-sm">
              {p.tap.length > 0 && <p className="font-medium">{plainActions(p.tap, catalog)}</p>}
              {p.hold.length > 0 && <p className="text-xs text-muted-foreground">Hold: {plainActions(p.hold, catalog)}</p>}
            </div>
          </li>
        )
      })}
    </ul>
  )
}

/** What a smart button with a device does: the hub works it out, so it's read-only (a long press only if it has one). */
function SlotActions({ slot, actions, catalog }: { slot: SlotId; actions: SmartSlotActions | null; catalog: Catalog }) {
  if (!actions) return <Skeleton className="h-24" />
  const stops = (p: SmartPress) => (p.release.length ? 'stops on release' : undefined)
  const rows: { title: string; color: string; list: Action[]; note?: string; optional?: boolean }[] = [
    { title: 'Press', color: EV_COLOR.tap, list: actions.button.tap },
    { title: 'Long press', color: EV_COLOR.hold, list: actions.button.hold },
    { title: '+', color: EV_COLOR.tap, list: actions.plus.tap },
    { title: '+ long press', color: EV_COLOR.hold, list: actions.plus.hold, note: stops(actions.plus), optional: true },
    { title: '−', color: EV_COLOR.tap, list: actions.minus.tap },
    { title: '− long press', color: EV_COLOR.hold, list: actions.minus.hold, note: stops(actions.minus), optional: true },
  ]
  return (
    <section className="grid gap-2" aria-label={`What ${buttonLabel(slot)} does`}>
      {rows
        .filter((r) => r.list.length || !r.optional)
        .map((r) => (
          <Row key={r.title} color={r.color} title={r.title} note={r.note} action={r.list} catalog={catalog} editing={false} actions={null} />
        ))}
    </section>
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
                'inline-flex items-center gap-1.5 rounded-md border bg-card px-2 py-1 text-xs outline-none hover:bg-accent focus-visible:ring-2 focus-visible:ring-ring',
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
