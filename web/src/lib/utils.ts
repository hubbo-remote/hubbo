import { clsx, type ClassValue } from 'clsx'
import { twMerge } from 'tailwind-merge'

export function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs))
}

/** A typed word that confirms a destructive action (case and surrounding spaces don't matter). */
export const typedMatches = (typed: string, word: string) => typed.trim().toLowerCase() === word

export const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))

/** Structural equality for plain JSON data (drafts vs saved copies). */
export const same = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b)

/** An option for display: `_` → space, each word capitalized unless it already has a capital or a digit ("HDMI 1", "eARC"). */
export const titleCase = (s: string) =>
  s
    .replace(/_/g, ' ')
    .split(' ')
    .map((w) => (/[A-Z0-9]/.test(w) ? w : w.charAt(0).toUpperCase() + w.slice(1)))
    .join(' ')

/** `arrow_up` → "Arrow up": first letter capitalised, underscores to spaces. */
export const humanize = (s: string) => s.charAt(0).toUpperCase() + s.slice(1).replace(/_/g, ' ')

/**
 * "Sleep Time" → "Sleep time": a label in Title Case (every word with letters Capitalized) goes to
 * sentence case. Anything else is left as written ("Join Sonos group", "Set HEV Cycle State").
 */
export function sentenceCase(s: string): string {
  const words = s.split(' ')
  const title = words.every((w) => !/[A-Za-z]/.test(w) || /^[A-Z][a-z]*$/.test(w.replace(/[^A-Za-z]/g, '')))
  return title ? words.map((w, i) => (i > 0 && /^[A-Z][a-z]+$/.test(w) ? w.toLowerCase() : w)).join(' ') : s
}

/** `3 bindings`, `1 binding`. */
export const plural = (n: number, word: string, many = `${word}s`) => `${n} ${n === 1 ? word : many}`

/** 500 ms, 1 s, 1.5 s, 2.25 s: milliseconds under a second, else seconds without trailing zeros. */
export const duration = (ms: number) => (ms >= 1000 ? `${Number((ms / 1000).toFixed(3))} s` : `${ms} ms`)

export function formatDuration(s: number): string {
  const d = Math.floor(s / 86400)
  const h = Math.floor((s % 86400) / 3600)
  const m = Math.floor((s % 3600) / 60)
  if (d) return `${d}d ${h}h`
  if (h) return `${h}h ${m}m`
  return `${m}m ${Math.floor(s % 60)}s`
}

export function timeAgo(iso: string | null, now = Date.now()): string {
  return iso ? agoMs(now - Date.parse(iso)) : 'never'
}

/** Age in milliseconds as `just now`, `12s ago`, `3m ago`… */
export function agoMs(ms: number | null | undefined): string {
  if (ms == null) return 'never'
  const s = Math.max(0, Math.round(ms / 1000))
  if (s < 5) return 'just now'
  if (s < 60) return `${s}s ago`
  if (s < 3600) return `${Math.floor(s / 60)}m ago`
  if (s < 86400) return `${Math.floor(s / 3600)}h ago`
  return `${Math.floor(s / 86400)}d ago`
}

export const ms = (us: number) => (us / 1000).toFixed(us < 10_000 ? 1 : 0)

export function bytes(n: number): string {
  if (n >= 1 << 20) return `${(n / (1 << 20)).toFixed(1)} MB`
  return `${Math.round(n / 1024)} KB`
}
