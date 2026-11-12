import { useId } from 'react'
import type { Action } from '@/api/types'
import { Field, Select } from '@/components/ui/input'
import { useCapabilities } from '@/hooks/data'
import { isSystem, SYSTEM, systemCommandAllowed, type Catalog } from '@/lib/activity'
import { actionFor, choiceName, choiceOf, choiceParams, choices, domainOf, isFriendly } from '@/lib/commands'
import { ActionPicker, TestButton } from './ActionPicker'
import { ParamForm } from './ParamForm'

const SYSTEM_VALUE = '__system__'

/**
 * Device → one of its commands by name (Home, Menu, Volume up...) → any params it asks for.
 * Values are tap-form actions; Press or Hold is the caller's. An action this can't show as it is
 * (a raw down or up) opens in the full catalog picker instead, so nothing is rewritten unasked.
 */
export function CommandPicker({ value, onChange, catalog, context, testable = true }: { value: Action | null; onChange: (a: Action | null) => void; catalog: Catalog; context: 'sequence' | 'binding'; testable?: boolean }) {
  const id = useId()
  const system = value ? isSystem(value) : false
  const deviceId = !value ? '' : system ? SYSTEM_VALUE : (value.device_id ?? '')
  const known = catalog.capabilities.get(deviceId)
  const caps = useCapabilities(deviceId && deviceId !== SYSTEM_VALUE && !known ? deviceId : null)
  const systemCommands = catalog.systemCommands.filter((c) => systemCommandAllowed(c.command_id, context === 'sequence' ? 'sequence' : 'binding-last'))
  const commands = system ? systemCommands : ((known ?? caps.data)?.commands ?? [])
  if (value && !isFriendly(value, commands)) return <ActionPicker value={value} onChange={onChange} catalog={catalog} context={context} allowHold={context === 'binding'} testable={testable} />

  const list = choices(commands, context, domainOf(catalog.devices.get(deviceId)))
  const choice = value ? choiceOf(value, commands) : ''
  const params = value ? choiceParams(choice, commands, context) : []
  const picked = list.find((c) => c.id === choice)
  const own = list.filter((c) => !c.more)
  const more = list.filter((c) => c.more)

  const pickDevice = (d: string) => {
    if (!d) return onChange(null)
    const target = d === SYSTEM_VALUE ? { integration_id: SYSTEM, device_id: null } : { integration_id: catalog.devices.get(d)!.integration_id, device_id: d }
    const cmds = d === SYSTEM_VALUE ? systemCommands : (catalog.capabilities.get(d)?.commands ?? [])
    const first = choices(cmds, context, domainOf(catalog.devices.get(d)))[0]
    onChange(first ? actionFor(first.id, target, cmds) : { ...target, command_id: '', params: {} })
  }
  const pickCommand = (c: string) => value && onChange(actionFor(c, value, commands))

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
            <Select id={`${id}-cmd`} value={choice} onChange={(e) => pickCommand(e.target.value)} disabled={caps.loading && !commands.length}>
              {!list.some((c) => c.id === choice) && <option value={choice}>{value.command_id || 'Choose…'}</option>}
              {own.map((c) => (
                <option key={c.id} value={c.id}>
                  {c.label}
                </option>
              ))}
              {more.length > 0 && (
                <optgroup label="More">
                  {more.map((c) => (
                    <option key={c.id} value={c.id}>
                      {c.label}
                    </option>
                  ))}
                </optgroup>
              )}
            </Select>
          </Field>
        )}
      </div>
      {value && <ParamForm key={choice} commandId={value.command_id} name={picked ? choiceName(picked) : value.command_id} params={params} values={value.params} onChange={(p) => onChange({ ...value, params: p })} activities={catalog.activities} compact />}
      {testable && value?.device_id && value.command_id && commands.find((c) => c.command_id === value.command_id)?.effect === 'momentary' && <TestButton action={value} />}
    </div>
  )
}
