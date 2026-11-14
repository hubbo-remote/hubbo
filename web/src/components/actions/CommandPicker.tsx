import { useId } from 'react'
import type { Action } from '@/api/types'
import { Field, Select } from '@/components/ui/input'
import { useCapabilities } from '@/hooks/data'
import { hubboCommands, isSystem, type Catalog } from '@/lib/activity'
import { actionFor, choiceName, choiceOf, choiceParams, choices, domainOf, isFriendly } from '@/lib/commands'
import { customServices, startCall } from '@/lib/haCall'
import { ActionPicker, TestButton } from './ActionPicker'
import { DeviceSelect } from './DeviceSelect'
import { HaActionFields } from './HaAction'
import { ParamForm } from './ParamForm'
import { startAction, SYSTEM_VALUE } from './start'

/**
 * Device → one of its commands by name (Home, Menu, Volume up...) → any params it asks for.
 * Values are tap-form actions; how a button sends one is the caller's. An action this can't show as it is
 * (a raw down or up) opens in the full catalog picker instead, so nothing is rewritten unasked.
 */
export function CommandPicker({ value, onChange, catalog, context, testable = true }: { value: Action | null; onChange: (a: Action | null) => void; catalog: Catalog; context: 'sequence' | 'binding'; testable?: boolean }) {
  const id = useId()
  const system = value ? isSystem(value) : false
  const deviceId = !value ? '' : system ? SYSTEM_VALUE : (value.device_id ?? '')
  const known = catalog.capabilities.get(deviceId)
  const caps = useCapabilities(deviceId && deviceId !== SYSTEM_VALUE && !known ? deviceId : null)
  const systemCommands = hubboCommands(catalog, context)
  const commands = system ? systemCommands : ((known ?? caps.data)?.commands ?? [])
  if (value && !isFriendly(value, commands)) return <ActionPicker value={value} onChange={onChange} catalog={catalog} context={context} allowHold={context === 'binding'} testable={testable} />

  const list = choices(commands, context, domainOf(catalog.devices.get(deviceId)))
  const services = customServices(commands)
  const choice = value ? choiceOf(value, commands) : ''
  const params = value ? choiceParams(choice, commands, context) : []
  const picked = list.find((c) => c.id === choice)
  const own = list.filter((c) => !c.more)
  const more = list.filter((c) => c.more)

  const pickDevice = (d: string) => onChange(startAction(catalog, context, d))
  const pickCommand = (c: string) => value && onChange(c === 'call' ? startCall(actionFor(c, value, commands), services) : actionFor(c, value, commands))

  // One row: Device | Command | what the command needs (a key, a code, a source); Device on its own
  // line on a phone. The drawer (other settings) sits below.
  const lead = (
    <>
      <div className="basis-full sm:basis-auto">
        <Field label="Device" htmlFor={`${id}-dev`}>
          <DeviceSelect id={`${id}-dev`} value={deviceId} onChange={pickDevice} catalog={catalog} hubbo={systemCommands.length > 0} className="w-auto max-w-full" />
        </Field>
      </div>
      {value && (!system || systemCommands.length > 1) && (
        <Field label="Command" htmlFor={`${id}-cmd`}>
          <Select id={`${id}-cmd`} value={choice} onChange={(e) => pickCommand(e.target.value)} disabled={caps.loading && !commands.length} className="w-auto max-w-full">
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
    </>
  )
  return (
    <div className="grid gap-3">
      {/* Always through ParamForm, so the Device and Command selects keep their place (and focus) once a value is set. */}
      <ParamForm commandId={value?.command_id ?? ''} name={picked ? choiceName(picked) : (value?.command_id ?? '')} params={params} values={value?.params ?? {}} onChange={(p) => value && onChange({ ...value, params: p })} activities={catalog.activities} lead={lead} />
      {value && choice === 'call' && <HaActionFields key={deviceId} id={id} value={value} services={services} onChange={onChange} />}
      {testable && value?.device_id && value.command_id && commands.find((c) => c.command_id === value.command_id)?.effect === 'momentary' && <TestButton action={value} />}
    </div>
  )
}
