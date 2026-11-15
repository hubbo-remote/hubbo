// Smart buttons (docs/design/smart-buttons.md): Light 1, Light 2, Switch 1 and Switch 2 each take a device, and
// + / − follow the last one pressed. Pure: imported by the UI and the mock, so relative imports only.
import type { Bindings, SlotId, SmartButtons, SmartButtonsInput, SmartSlotActions, ValidationIssue } from '../api/types.ts'
import { parseTrigger, type Catalog } from './activity.ts'

export const SLOTS: readonly SlotId[] = ['light_1', 'light_2', 'switch_1', 'switch_2']
/** The kinds a slot's device picker offers (and the hub has actions for). */
export const SLOT_KINDS: ReadonlySet<string> = new Set(['light', 'fan', 'cover', 'switch'])

export const isSlot = (button: string): button is SlotId => (SLOTS as readonly string[]).includes(button)

export const emptySmartButtons = (revision: number): SmartButtonsInput => ({ revision, slots: { light_1: null, light_2: null, switch_1: null, switch_2: null } })

/** The client's checks (the hub's: a slot's device must exist). */
export function validateSmartButtons(doc: Pick<SmartButtonsInput, 'slots'>, cat: Catalog): { errors: ValidationIssue[]; warnings: ValidationIssue[] } {
  const errors: ValidationIssue[] = []
  for (const slot of SLOTS) {
    const d = doc.slots[slot]
    if (d && !cat.devices.has(d)) errors.push({ path: `/slots/${slot}`, code: 'unknown_device', message: 'No device with that id' })
  }
  return { errors, warnings: [] }
}

/** The smart buttons as the bindings editor shows them: each slot's device and what the hub says it does. */
export type SmartSet = {
  slots: Record<SlotId, string | null>
  /** The hub's actions for the slot's device; null while a just-picked device's are loading. */
  actions: (slot: SlotId) => SmartSlotActions | null
}

/**
 * The hub's smart buttons, or (on Global) a draft of their slots over them, with `picked`: the hub's actions for
 * devices picked but not saved yet (GET /smart-buttons/actions). Undefined until they load.
 */
export const smartOf = (sb: SmartButtons | undefined, slots = sb?.slots, picked?: ReadonlyMap<string, SmartSlotActions>): SmartSet | undefined =>
  sb && slots && { slots, actions: (slot) => (slots[slot] === sb.slots[slot] ? sb.actions[slot] : (picked?.get(slots[slot] ?? '') ?? null)) }

/** The devices in `slots` that the saved copy doesn't have: their actions come from GET /smart-buttons/actions. */
export const pickedDevices = (sb: SmartButtons | undefined, slots: SmartButtons['slots'] | undefined): string[] =>
  sb && slots ? [...new Set(SLOTS.flatMap((s) => (slots[s] && slots[s] !== sb.slots[s] ? [slots[s]] : [])))].sort() : []

/**
 * Global's bindings as saved with these smart buttons: a slot that gets a device (none saved, one in the draft)
 * takes its button, and the first device anywhere takes + and − too. Their bindings go, so None later starts empty.
 */
export function withoutSmartButtons(bindings: Bindings, saved: SmartButtons['slots'], draft: SmartButtons['slots']): Bindings {
  const taken = new Set<string>(SLOTS.filter((s) => !saved[s] && draft[s]))
  if (!SLOTS.some((s) => saved[s]) && SLOTS.some((s) => draft[s])) taken.add('plus').add('minus')
  if (!taken.size) return bindings
  return Object.fromEntries(Object.entries(bindings).filter(([k]) => !taken.has(parseTrigger(k)?.button ?? '')))
}
