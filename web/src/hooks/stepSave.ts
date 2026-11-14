import { createContext, useContext, useEffect, useRef, type RefObject } from 'react'

/** Saves a form's unsaved edits; resolves false when it couldn't (the form shows why). */
export type PendingSave = () => Promise<boolean>

/** Set by the setup wizard: its Continue saves the step's unsaved edits first. Settings has none. */
export const StepSaveContext = createContext<RefObject<PendingSave | null> | null>(null)

/** Hand the wizard this form's save while it has unsaved edits (null when there's nothing to save). */
export function usePendingSave(save: PendingSave | null) {
  const slot = useContext(StepSaveContext)
  useEffect(() => {
    if (!slot) return
    slot.current = save
    return () => void (slot.current = null)
  })
}

/** usePendingSave for a form: its fields' own checks run first. Put the returned ref on the form. */
export function usePendingFormSave(dirty: boolean, save: () => Promise<boolean>) {
  const form = useRef<HTMLFormElement>(null)
  usePendingSave(dirty ? () => (form.current?.reportValidity() ? save() : Promise.resolve(false)) : null)
  return form
}
