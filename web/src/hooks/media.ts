import { useSyncExternalStore } from 'react'

/** Tailwind's `md` and up: the side-by-side editors (follows changes, e.g. a phone turned sideways). */
const wide = typeof matchMedia === 'function' ? matchMedia('(min-width: 768px)') : null
const subscribe = (cb: () => void) => {
  wide?.addEventListener('change', cb)
  return () => wide?.removeEventListener('change', cb)
}
const isWide = () => wide?.matches ?? true

export const useWide = (): boolean => useSyncExternalStore(subscribe, isWide)
