import { useEffect, useSyncExternalStore } from 'react'

export type ThemePref = 'light' | 'dark' | 'system'
const KEY = 'hubbo.theme' // display preference only, never secrets
const listeners = new Set<() => void>()

function read(): ThemePref {
  try {
    const v = localStorage.getItem(KEY)
    return v === 'light' || v === 'dark' ? v : 'system'
  } catch {
    return 'system'
  }
}

let current: ThemePref = read()

function apply() {
  const dark = current === 'dark' || (current === 'system' && matchMedia('(prefers-color-scheme: dark)').matches)
  document.documentElement.classList.toggle('dark', dark)
}

export function setTheme(t: ThemePref) {
  current = t
  try {
    localStorage.setItem(KEY, t)
  } catch {
    /* private mode */
  }
  apply()
  listeners.forEach((l) => l())
}

export function useTheme(): [ThemePref, (t: ThemePref) => void] {
  const t = useSyncExternalStore(
    (l) => (listeners.add(l), () => listeners.delete(l)),
    () => current,
  )
  useEffect(() => {
    const mq = matchMedia('(prefers-color-scheme: dark)')
    mq.addEventListener('change', apply)
    return () => mq.removeEventListener('change', apply)
  }, [])
  return [t, setTheme]
}
