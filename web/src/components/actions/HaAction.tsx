import { useState } from 'react'
import type { Action, CustomService } from '@/api/types'
import { Field, Select, ShowErrors, Textarea } from '@/components/ui/input'
import { callIssues, dataText, prefill, readCallData } from '@/lib/haCall'

/**
 * A Home Assistant action's fields: the service, picked from the device's own (never typed), and its
 * data as JSON, checked as you type and started from HA's example. Remount it per device.
 */
export function HaActionFields({ id, value, services, onChange }: { id: string; value: Action; services: CustomService[]; onChange: (a: Action) => void }) {
  const call = value.call ?? { service: '', data: {} }
  const [text, setText] = useState(() => dataText(call.data))
  const picked = services.find((s) => s.service === call.service)
  const dataError = readCallData(text).error
  const serviceError = callIssues(call, '').find((x) => x.path === '/service')?.message

  const pick = (service: string) => {
    const data = prefill(services.find((s) => s.service === service))
    setText(dataText(data))
    onChange({ ...value, call: { service, data } })
  }
  const type = (t: string) => {
    setText(t)
    onChange({ ...value, call: { service: call.service, data: readCallData(t).data as Record<string, unknown> } })
  }

  return (
    <div className="grid gap-3">
      <Field label="Service" htmlFor={`${id}-svc`} error={serviceError}>
        <Select id={`${id}-svc`} value={call.service} onChange={(e) => pick(e.target.value)} aria-invalid={!!serviceError}>
          {!call.service && <option value="">Choose…</option>}
          {call.service && !picked && <option value={call.service}>{call.service} (not available)</option>}
          {services.map((s) => (
            <option key={s.service} value={s.service}>
              {s.label ? `${s.service} (${s.label})` : s.service}
            </option>
          ))}
        </Select>
      </Field>
      {/* Checked as you type, whatever the editor around it shows. */}
      <ShowErrors.Provider value>
        <Field label="Data" htmlFor={`${id}-data`} error={dataError} hint={dataError ? undefined : 'JSON, sent to the device as it is.'}>
          <Textarea id={`${id}-data`} value={text} onChange={(e) => type(e.target.value)} rows={Math.min(8, Math.max(2, text.split('\n').length))} aria-invalid={!!dataError} />
        </Field>
      </ShowErrors.Provider>
      {picked && picked.fields.length > 0 && (
        <ul className="grid gap-1 text-xs text-muted-foreground" aria-label={`${picked.service} fields`}>
          {picked.fields.map((f) => (
            <li key={f.name}>
              <code className="font-mono text-foreground">{f.name}</code>
              {f.required && ' (required)'}
              {f.description && ` · ${f.description}`}
            </li>
          ))}
        </ul>
      )}
    </div>
  )
}
