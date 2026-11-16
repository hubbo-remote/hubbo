import { ChevronDown } from 'lucide-react'
import { useId, useState, type ReactNode } from 'react'
import type { ActionParams, ParamSpec, ParamValue } from '@/api/types'
import { Field, Input, Select } from '@/components/ui/input'
import { InfoTip } from '@/components/ui/info-tip'
import { Switch } from '@/components/ui/switch'
import { checkParam, drawerParams, fieldLabel, numberChoices, paramLabel, withUnit } from '@/lib/activity'
import { groupKeys, paramValueLabel } from '@/lib/keys'
import { cn } from '@/lib/utils'

/**
 * Form fields generated from a command's parameter schema (ParamSpec). The primary params (required
 * ones, or a command's sole optional one with no default) show in place; any other optional ones sit
 * in a drawer below, behind a chevron that's always there for such a command (a dot when one is set).
 * `inline`: the device page row, label-less controls between `before` and `after`, no sliders.
 * `lead`: a picker's own fields (Device, Command), in one row with the primary params, which fit their content.
 */
export function ParamForm({ commandId, name, params, values, onChange, activities = [], compact, inline, before, after, lead }: { commandId: string; name: string; params: ParamSpec[]; values: ActionParams; onChange: (v: ActionParams) => void; activities?: { id: string; name: string }[]; compact?: boolean; inline?: boolean; before?: ReactNode; after?: ReactNode; lead?: ReactNode }) {
  const base = useId()
  const drawer = drawerParams(params)
  const primary = params.filter((p) => !drawer.includes(p))
  const changed = drawer.some((p) => values[p.name] !== undefined && values[p.name] !== p.default)
  // Open for this command only (a picker keeps its form while the command changes); a stored action with settings opens on them.
  const sig = `${commandId} ${name}`
  const [openFor, setOpenFor] = useState(changed ? sig : '')
  const open = openFor === sig
  const setOpen = (o: boolean) => setOpenFor(o ? sig : '')
  const set = (k: string, v: ParamValue | undefined) => {
    const next = { ...values }
    if (v === undefined || v === '') delete next[k]
    else next[k] = v
    onChange(next)
  }
  const ids = new Set(activities.map((a) => a.id))
  // `row`: a device page row (screen-reader labels, fixed widths); `fit`: a picker's row (labels above,
  // sized to content, no slider); `stack`: one under another.
  const field = (p: ParamSpec, mode: 'row' | 'fit' | 'stack') => {
    const inRow = mode === 'row'
    const fit = mode === 'fit'
    const id = `${base}-${p.name}`
    // An unset optional param shows its default (what runs), without storing it.
    const v = values[p.name] ?? (p.required ? undefined : p.default)
    const error = checkParam(p, values[p.name], activities.length ? ids : undefined)
    const label = fieldLabel(p, commandId)
    const inlineAs = inRow || undefined
    switch (p.type) {
      case 'enum':
      case 'activity_ref': {
        const opts = p.type === 'enum' ? (p.options ?? []).map((o) => ({ value: o, label: paramValueLabel(p.name, o) })) : activities.map((a) => ({ value: a.id, label: a.name }))
        // What's shown is always what's stored. Unset shows as "Choose…"/"Not set" (never as the first
        // option, which picking again wouldn't set); a stored value that isn't an option shows as
        // itself, so it can be seen and changed or cleared. Refs aren't judged before the list loads.
        const stored = v === undefined ? '' : String(v)
        const unknown = stored !== '' && !opts.some((o) => o.value === stored)
        const loading = p.type === 'activity_ref' && activities.length === 0
        return (
          <Field key={p.name} label={label} htmlFor={id} error={error} info={p.description} inline={inlineAs}>
            <Select id={id} value={stored} onChange={(e) => set(p.name, e.target.value)} aria-invalid={!!error} className={inRow ? 'w-48' : fit ? 'w-auto max-w-full' : undefined}>
              {((!p.required && p.default === undefined) || stored === '') && <option value="">{p.required ? 'Choose…' : 'Not set'}</option>}
              {unknown && <option value={stored}>{loading ? stored : `${stored} (not available)`}</option>}
              {p.name === 'key'
                ? groupKeys(opts.map((o) => o.value)).map(([group, keys]) => (
                    <optgroup key={group} label={group}>
                      {keys.map((k) => (
                        <option key={k} value={k}>
                          {paramValueLabel(p.name, k)}
                        </option>
                      ))}
                    </optgroup>
                  ))
                : opts.map((o) => (
                    <option key={o.value} value={o.value}>
                      {o.label}
                    </option>
                  ))}
            </Select>
          </Field>
        )
      }
      case 'bool':
        return (
          <div key={p.name} className="flex items-center justify-between gap-3 rounded-md border px-3 py-2">
            <span className="flex items-center gap-1.5">
              <label htmlFor={id} className="text-sm font-medium">
                {label}
              </label>
              {p.description && <InfoTip text={p.description} label={label} />}
            </span>
            <Switch id={id} checked={v === true} onCheckedChange={(c) => set(p.name, c)} />
          </div>
        )
      case 'int':
      case 'number': {
        const choices = numberChoices(commandId, p)
        if (choices) {
          const current = typeof v === 'number' ? v : undefined
          return (
            <Field key={p.name} label={label} htmlFor={id} error={error} info={p.description} inline={inlineAs}>
              <Select id={id} value={current === undefined ? '' : String(current)} onChange={(e) => set(p.name, e.target.value === '' ? undefined : Number(e.target.value))} aria-invalid={!!error} className={inRow ? (choices.kind === 'speed' ? 'w-36' : 'w-28') : fit ? 'w-auto' : undefined}>
                {current === undefined && <option value="">{p.required ? 'Choose…' : 'Not set'}</option>}
                {choices.options(current).map((o) => (
                  <option key={o} value={String(o)}>
                    {choices.label(o)}
                  </option>
                ))}
              </Select>
            </Field>
          )
        }
        const ranged = p.minimum !== undefined && p.maximum !== undefined
        const step = p.step ?? (p.type === 'int' ? 1 : 0.01)
        const display = typeof v === 'number' ? v : ''
        return (
          <Field key={p.name} label={label} htmlFor={id} error={error} info={p.description} hint={ranged ? withUnit(`${p.minimum}–${p.maximum}`, p.unit) : undefined} inline={inRow && 'beside'}>
            <div className="flex items-center gap-3">
              {ranged && !inRow && !fit && (
                <input
                  type="range"
                  aria-label={`${paramLabel(p)} slider`}
                  min={p.minimum}
                  max={p.maximum}
                  step={step}
                  value={display === '' ? p.minimum : display}
                  onChange={(e) => set(p.name, Number(e.target.value))}
                  className="h-2 min-w-0 flex-1 accent-[var(--primary)]"
                />
              )}
              <Input
                id={id}
                type="number"
                inputMode="decimal"
                min={p.minimum}
                max={p.maximum}
                step={step}
                value={display}
                onChange={(e) => set(p.name, e.target.value === '' ? undefined : Number(e.target.value))}
                className={ranged || inRow || fit ? 'w-24' : undefined}
                aria-invalid={!!error}
              />
            </div>
          </Field>
        )
      }
    }
  }

  // The drawer's toggle: a bare chevron at a device page row's end; in a picker, "More settings" under the fields.
  const toggle = (labelled: boolean) => (
    <button
      type="button"
      onClick={() => setOpen(!open)}
      aria-expanded={open}
      aria-controls={`${base}-drawer`}
      aria-label={`More settings for ${name}`}
      title={labelled ? undefined : 'More settings'}
      className={cn(
        'relative inline-flex items-center rounded-md text-muted-foreground outline-none hover:text-foreground focus-visible:ring-2 focus-visible:ring-ring',
        labelled ? 'gap-1.5 justify-self-start text-xs' : 'size-9 shrink-0 justify-center hover:bg-accent',
      )}
    >
      {labelled && 'More settings'}
      <span className="relative inline-flex">
        <ChevronDown className={cn('transition-transform', labelled ? 'size-3.5' : 'size-4', open && 'rotate-180')} aria-hidden />
        {changed && <span className="absolute -top-0.5 -right-1 size-1.5 rounded-full bg-primary" aria-hidden />}
      </span>
    </button>
  )
  const drawerBody = open && (
    <div id={`${base}-drawer`} className={cn('grid gap-3 bg-muted/40 py-3', inline ? '-mx-4 -mb-2 rounded-b-lg border-t px-4' : 'rounded-md border px-3')}>
      {drawer.map((p) => (
        <div key={p.name} className="grid gap-1">
          {field(p, 'stack')}
          {values[p.name] !== undefined && (
            <button type="button" onClick={() => set(p.name, undefined)} className="justify-self-start text-xs text-muted-foreground underline-offset-2 hover:text-foreground hover:underline">
              Reset{p.default === undefined ? '' : ' to default'}
            </button>
          )}
        </div>
      ))}
    </div>
  )
  if (inline)
    return (
      <div className="grid">
        {/* Name, then a right-aligned trailing group: controls, Test, a chevron slot always reserved, so every
            row's Test and chevron line up in columns (on a phone the group wraps under the name, still right-aligned). */}
        <div className="flex flex-wrap items-center gap-x-3 gap-y-2">
          {before}
          <div className="ml-auto flex flex-wrap items-center justify-end gap-2">
            {primary.length > 0 && <div className="flex flex-wrap items-start justify-end gap-2">{primary.map((p) => field(p, 'row'))}</div>}
            {after}
            {drawer.length > 0 ? toggle(false) : <span className="size-9 shrink-0" aria-hidden />}
          </div>
        </div>
        {drawerBody && <div className="mt-2">{drawerBody}</div>}
      </div>
    )
  if (params.length === 0 && !lead) return null
  return (
    <div className="grid gap-2">
      {lead ? (
        <div className="flex flex-wrap items-start gap-3">
          {lead}
          {primary.map((p) => field(p, 'fit'))}
        </div>
      ) : (
        primary.length > 0 && <div className={compact ? 'grid gap-3 sm:grid-cols-2' : 'grid gap-4'}>{primary.map((p) => field(p, 'stack'))}</div>
      )}
      {drawer.length > 0 && (
        <div className="grid">
          {toggle(true)}
          {drawerBody && <div className="mt-2">{drawerBody}</div>}
        </div>
      )}
    </div>
  )
}
