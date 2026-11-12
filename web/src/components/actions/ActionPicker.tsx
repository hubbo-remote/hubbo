import { Loader2, Play } from 'lucide-react'
import { useId, useState } from 'react'
import { api, errorMessage } from '@/api/client'
import type { Action, Command, ExecuteResult } from '@/api/types'
import { Button } from '@/components/ui/button'
import { Field, Select } from '@/components/ui/input'
import { useCapabilities } from '@/hooks/data'
import { commandFor, defaultParams, hubboCommands, isSystem, type Catalog } from '@/lib/activity'
import { commandTitle } from '@/lib/commands'
import { commandGroups } from '@/lib/keys'
import { cn } from '@/lib/utils'
import { ParamForm } from './ParamForm'
import { DeviceSelect } from './DeviceSelect'
import { SYSTEM_VALUE, targetOf } from './start'

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
  const systemCommands = hubboCommands(catalog, context)
  // Holds only where a release can follow (a binding's down); waits (wait_connected) only in sequences.
  const offer = (c: Command) => (allowHold || c.effect !== 'hold') && (context === 'sequence' || c.effect !== 'wait')
  const commands = (system ? systemCommands : ((known ?? caps.data)?.commands ?? [])).filter(offer)
  const cmd = value ? commandFor(value, catalog) ?? commands.find((c) => c.command_id === value.command_id) : undefined
  const [showCustom, setShowCustom] = useState(false)
  const customPicked = cmd?.group === 'custom'
  const groups = commandGroups(commands, showCustom || customPicked)
  const hasCustom = commands.some((c) => c.group === 'custom')

  const pickDevice = (d: string) => {
    const first = d === SYSTEM_VALUE ? systemCommands[0] : catalog.capabilities.get(d)?.commands.find(offer)
    onChange({ ...targetOf(catalog, d), command_id: first?.command_id ?? '', params: first ? defaultParams(first.params, first.command_id) : {} })
  }

  const pickCommand = (c: string) => {
    if (!value) return
    const def = commands.find((x) => x.command_id === c)
    onChange({ ...value, command_id: c, params: def ? defaultParams(def.params, c) : {} })
  }

  return (
    <div className="grid gap-3">
      <div className="grid gap-3 sm:grid-cols-2">
        <Field label="Device" htmlFor={`${id}-dev`}>
          <DeviceSelect id={`${id}-dev`} value={deviceId} onChange={pickDevice} catalog={catalog} hubbo={systemCommands.length > 0} />
        </Field>
        {value && (!system || systemCommands.length > 1) && (
          <Field label="Command" htmlFor={`${id}-cmd`}>
            <Select id={`${id}-cmd`} value={value.command_id} onChange={(e) => pickCommand(e.target.value)} disabled={caps.loading && !commands.length}>
              {!commands.some((c) => c.command_id === value.command_id) && <option value={value.command_id}>{value.command_id || 'Choose…'}</option>}
              {groups.map(([label, list]) => {
                const options = list.map((c) => (
                  <option key={c.command_id} value={c.command_id}>
                    {commandTitle(c)}
                  </option>
                ))
                return label ? (
                  <optgroup key={label} label={label}>
                    {options}
                  </optgroup>
                ) : (
                  options
                )
              })}
            </Select>
          </Field>
        )}
      </div>
      {hasCustom && !customPicked && (
        <label className="flex items-center gap-2 text-xs text-muted-foreground">
          <input type="checkbox" checked={showCustom} onChange={(e) => setShowCustom(e.target.checked)} /> Show custom codes
        </label>
      )}
      {cmd?.description && <p className="text-xs text-muted-foreground">{cmd.description}</p>}
      {value && cmd && <ParamForm key={value.command_id} commandId={value.command_id} name={commandTitle(cmd)} params={cmd.params} values={value.params} onChange={(params) => onChange({ ...value, params })} activities={catalog.activities} compact />}
      {testable && value && cmd && !system && cmd.effect === 'momentary' && <TestButton action={value} />}
    </div>
  )
}

export type TestResult = ExecuteResult | { status: 'error'; message: string }

/** `onResult`: the caller shows the result itself (a device page row, under the name), not beside the button. */
export function TestButton({ action, size = 'sm', onRun, onResult }: { action: Action; size?: 'sm' | 'default'; onRun?: () => void; onResult?: (r: TestResult | null) => void }) {
  const [busy, setBusy] = useState(false)
  const [res, setResState] = useState<TestResult | null>(null)
  const setRes = (r: TestResult | null) => {
    setResState(r)
    onResult?.(r)
  }
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
  const button = (
    <Button variant="outline" size={size} onClick={run} disabled={busy}>
      {busy ? <Loader2 className="animate-spin" /> : <Play />}
      Test
    </Button>
  )
  if (onResult) return button
  return (
    <div className="flex flex-wrap items-center gap-3">
      {button}
      <span aria-live="polite" className="text-sm">
        {res && <TestResultText res={res} />}
      </span>
    </div>
  )
}

export function TestResultText({ res }: { res: TestResult }) {
  return (
    <span className={cn(res.status === 'accepted' ? 'text-success' : res.status === 'timeout' ? 'text-[color-mix(in_oklch,var(--warning),black_35%)] dark:text-warning' : 'text-destructive')}>
      {res.status === 'accepted' ? 'Sent' : res.status === 'timeout' ? 'Timed out' : res.status === 'failed' ? 'Failed' : 'Not sent'}
      {res.message && <span className="text-muted-foreground"> · {res.message}</span>}
    </span>
  )
}
