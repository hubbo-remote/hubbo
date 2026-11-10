import { Loader2, Play } from 'lucide-react'
import { useId, useState } from 'react'
import { api, errorMessage } from '@/api/client'
import type { Action, ExecuteResult } from '@/api/types'
import { Button } from '@/components/ui/button'
import { Field, Select } from '@/components/ui/input'
import { useCapabilities } from '@/hooks/data'
import { commandFor, defaultParams, isSystem, SYSTEM, systemCommandAllowed, type Catalog } from '@/lib/activity'
import { cn } from '@/lib/utils'
import { ParamForm } from './ParamForm'

const SYSTEM_VALUE = '__system__'


/**
 * Device → command → typed params. Only registered devices and their
 * discovered commands are offered, plus Hubbo's own system commands.
 */
export function ActionPicker({ value, onChange, catalog, context, allowHold = false, testable = true }: { value: Action | null; onChange: (a: Action | null) => void; catalog: Catalog; context: 'sequence' | 'binding'; /** Offer hold commands (only a binding's down event can release them on up). */ allowHold?: boolean; testable?: boolean }) {
  const id = useId()
  const system = value ? isSystem(value) : false
  const deviceId = !value ? '' : system ? SYSTEM_VALUE : (value.device_id ?? '')
  const known = catalog.capabilities.get(deviceId)
  const caps = useCapabilities(deviceId && deviceId !== SYSTEM_VALUE && !known ? deviceId : null)
  // The binding UI edits one action per trigger, so it is always the last one.
  const systemCommands = catalog.systemCommands.filter((c) => systemCommandAllowed(c.command_id, context === 'sequence' ? 'sequence' : 'binding-last'))
  const commands = (system ? systemCommands : ((known ?? caps.data)?.commands ?? [])).filter((c) => allowHold || c.effect !== 'hold')
  const cmd = value ? commandFor(value, catalog) ?? commands.find((c) => c.command_id === value.command_id) : undefined

  const pickDevice = (d: string) => {
    if (!d) return onChange(null)
    if (d === SYSTEM_VALUE) {
      const first = systemCommands[0]
      return onChange({ integration_id: SYSTEM, device_id: null, command_id: first?.command_id ?? '', params: first ? defaultParams(first.params) : {} })
    }
    const dev = catalog.devices.get(d)!
    const first = catalog.capabilities.get(d)?.commands.find((c) => allowHold || c.effect !== 'hold')
    onChange({ integration_id: dev.integration_id, device_id: d, command_id: first?.command_id ?? '', params: first ? defaultParams(first.params) : {} })
  }

  const pickCommand = (c: string) => {
    if (!value) return
    const def = commands.find((x) => x.command_id === c)
    onChange({ ...value, command_id: c, params: def ? defaultParams(def.params) : {} })
  }

  return (
    <div className="grid gap-3">
      <div className="grid gap-3 sm:grid-cols-2">
        <Field label="Device" htmlFor={`${id}-dev`}>
          <Select id={`${id}-dev`} value={deviceId} onChange={(e) => pickDevice(e.target.value)}>
            <option value="">None</option>
            {[...catalog.devices.values()].map((d) => (
              <option key={d.id} value={d.id}>
                {d.name}
              </option>
            ))}
            {systemCommands.length > 0 && <option value={SYSTEM_VALUE}>{context === 'sequence' ? 'Wait' : 'Hubbo'}</option>}
          </Select>
        </Field>
        {value && (!system || systemCommands.length > 1) && (
          <Field label="Command" htmlFor={`${id}-cmd`}>
            <Select id={`${id}-cmd`} value={value.command_id} onChange={(e) => pickCommand(e.target.value)} disabled={caps.loading && !commands.length}>
              {!commands.some((c) => c.command_id === value.command_id) && <option value={value.command_id}>{value.command_id || 'Choose…'}</option>}
              {commands.map((c) => (
                <option key={c.command_id} value={c.command_id}>
                  {c.label}
                </option>
              ))}
            </Select>
          </Field>
        )}
      </div>
      {cmd?.description && <p className="text-xs text-muted-foreground">{cmd.description}</p>}
      {value && cmd && <ParamForm params={cmd.params} values={value.params} onChange={(params) => onChange({ ...value, params })} activities={catalog.activities} compact />}
      {testable && value && cmd && !system && cmd.effect === 'momentary' && <TestButton action={value} />}
    </div>
  )
}

export function TestButton({ action, size = 'sm', onRun }: { action: Action; size?: 'sm' | 'default'; onRun?: () => void }) {
  const [busy, setBusy] = useState(false)
  const [res, setRes] = useState<ExecuteResult | { status: 'error'; message: string } | null>(null)
  const run = async () => {
    onRun?.()
    if (!action.device_id) return
    setBusy(true)
    setRes(null)
    try {
      setRes(await api.execute(action.device_id, action.command_id, action.params))
    } catch (e) {
      setRes({ status: 'error', message: errorMessage(e) })
    } finally {
      setBusy(false)
    }
  }
  return (
    <div className="flex flex-wrap items-center gap-3">
      <Button variant="outline" size={size} onClick={run} disabled={busy}>
        {busy ? <Loader2 className="animate-spin" /> : <Play />}
        Test
      </Button>
      <span aria-live="polite" className="text-sm">
        {res && (
          <span className={cn(res.status === 'accepted' ? 'text-success' : res.status === 'timeout' ? 'text-[color-mix(in_oklch,var(--warning),black_35%)] dark:text-warning' : 'text-destructive')}>
            {res.status === 'accepted' ? 'Sent' : res.status === 'timeout' ? 'Timed out' : res.status === 'failed' ? 'Failed' : 'Not sent'}
            {res.message && <span className="text-muted-foreground"> · {res.message}</span>}
          </span>
        )}
      </span>
    </div>
  )
}
