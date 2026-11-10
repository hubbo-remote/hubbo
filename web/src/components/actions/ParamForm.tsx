import { useId } from 'react'
import type { ActionParams, ParamSpec, ParamValue } from '@/api/types'
import { Field, Input, Select } from '@/components/ui/input'
import { Switch } from '@/components/ui/switch'
import { checkParam, paramLabel } from '@/lib/activity'

/** Form fields generated from a command's parameter schema (ParamSpec). */
export function ParamForm({ params, values, onChange, activities = [], compact }: { params: ParamSpec[]; values: ActionParams; onChange: (v: ActionParams) => void; activities?: { id: string; name: string }[]; compact?: boolean }) {
  const base = useId()
  if (params.length === 0) return null
  const set = (k: string, v: ParamValue | undefined) => {
    const next = { ...values }
    if (v === undefined || v === '') delete next[k]
    else next[k] = v
    onChange(next)
  }
  const ids = new Set(activities.map((a) => a.id))
  return (
    <div className={compact ? 'grid gap-3 sm:grid-cols-2' : 'grid gap-4'}>
      {params.map((p) => {
        const id = `${base}-${p.name}`
        const v = values[p.name]
        const error = checkParam(p, v, activities.length ? ids : undefined)
        const label = `${paramLabel(p)}${p.required ? '' : ' (optional)'}`
        switch (p.type) {
          case 'enum':
          case 'activity_ref': {
            const opts = p.type === 'enum' ? (p.options ?? []).map((o) => ({ value: o, label: o })) : activities.map((a) => ({ value: a.id, label: a.name }))
            // What's shown is always what's stored. Unset shows as "Choose…"/"Not set" (never as the first
            // option, which picking again wouldn't set); a stored value that isn't an option shows as
            // itself, so it can be seen and changed or cleared. Refs aren't judged before the list loads.
            const stored = v === undefined ? '' : String(v)
            const unknown = stored !== '' && !opts.some((o) => o.value === stored)
            const loading = p.type === 'activity_ref' && activities.length === 0
            return (
              <Field key={p.name} label={label} htmlFor={id} error={error}>
                <Select id={id} value={stored} onChange={(e) => set(p.name, e.target.value)} aria-invalid={!!error}>
                  {(!p.required || stored === '') && <option value="">{p.required ? 'Choose…' : 'Not set'}</option>}
                  {unknown && <option value={stored}>{loading ? stored : `${stored} (not available)`}</option>}
                  {opts.map((o) => (
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
                <label htmlFor={id} className="text-sm font-medium">
                  {label}
                </label>
                <Switch id={id} checked={v === true} onCheckedChange={(c) => set(p.name, c)} />
              </div>
            )
          case 'int':
          case 'number': {
            const ranged = p.minimum !== undefined && p.maximum !== undefined
            const step = p.step ?? (p.type === 'int' ? 1 : 0.01)
            const display = typeof v === 'number' ? v : ''
            const unit = p.unit ? ` ${p.unit}` : ''
            return (
              <Field key={p.name} label={label} htmlFor={id} error={error} hint={ranged ? `${p.minimum}–${p.maximum}${unit}` : undefined}>
                <div className="flex items-center gap-3">
                  {ranged && (
                    <input
                      type="range"
                      aria-label={`${paramLabel(p)} slider`}
                      min={p.minimum}
                      max={p.maximum}
                      step={step}
                      value={display === '' ? p.minimum : display}
                      onChange={(e) => set(p.name, Number(e.target.value))}
                      className="h-2 min-w-0 flex-1 cursor-pointer accent-[var(--primary)]"
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
                    className={ranged ? 'w-24' : undefined}
                    aria-invalid={!!error}
                  />
                </div>
              </Field>
            )
          }
        }
      })}
    </div>
  )
}
