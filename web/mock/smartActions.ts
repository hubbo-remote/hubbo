// The hub's smart button table (docs/design/smart-buttons.md), played by the mock: GET /smart-buttons derives each
// slot's actions from its device with it. The web never derives them; it shows what the hub sends.
import type { Action, Command, Device, SmartPress, SmartSlotActions } from '../src/api/types.ts'
import { SLOT_KINDS } from '../src/lib/smartButtons.ts'

const press = (tap: Action[] = [], hold: Action[] = [], release: Action[] = []): SmartPress => ({ tap, hold, release })
export const noActions = (): SmartSlotActions => ({ button: press(), plus: press(), minus: press() })

/**
 * What a slot does with this device, as the hub derives it (the design doc's table): by the device's kind, each
 * action only if the device offers that command. Not Home Assistant, or another kind: nothing.
 */
export function slotActions(device: Device, commands: readonly Command[]): SmartSlotActions {
  const domain = device.provider.type === 'home_assistant' ? device.provider.entity_id.split('.')[0]! : ''
  const offered = new Set(commands.map((c) => c.command_id))
  const act = (service: string, params: Record<string, number> = {}): Action[] =>
    offered.has(`${domain}.${service}`) ? [{ integration_id: device.integration_id, device_id: device.id, command_id: `${domain}.${service}`, params }] : []
  const out = noActions()
  const kind = domain ? device.kind : null
  if (!kind || !SLOT_KINDS.has(kind)) return out
  out.button = kind === 'cover' ? press(act('open_cover'), act('close_cover')) : press(act('turn_on'), act('turn_off'))
  if (kind === 'light') {
    out.plus = press(act('brightness_step', { step: 26 }))
    out.minus = press(act('brightness_step', { step: -26 }))
  } else if (kind === 'fan') {
    out.plus = press(act('increase_speed'))
    out.minus = press(act('decrease_speed'))
  } else if (kind === 'cover') {
    const stepped = act('position_step').length > 0
    out.plus = stepped ? press(act('position_step', { step: 10 }), act('open_cover'), act('stop_cover')) : press(act('open_cover'))
    out.minus = stepped ? press(act('position_step', { step: -10 }), act('close_cover'), act('stop_cover')) : press(act('close_cover'))
  }
  return out
}
