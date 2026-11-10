// A deliberately tiny query cache (instead of a data library): keyed fetches,
// shared subscribers, stale-while-revalidate, one poll timer per key, eviction
// of unwatched keys, and invalidation by key path ("devices" also covers
// "devices/dev_x", but not "devices-foo").
import { useCallback, useEffect, useState, useSyncExternalStore } from 'react'
import { toApiError, type ApiError } from './client'

export interface QueryState<T> {
  data: T | undefined
  error: ApiError | undefined
  loading: boolean
}

interface Entry {
  data?: unknown
  error?: ApiError
  inflight?: Promise<void>
  fetchedAt: number
  fn?: () => Promise<unknown>
  listeners: Set<() => void>
  snapshot: QueryState<unknown>
  /** Poll interval requested by each mounted subscriber. */
  polls: Map<symbol, number>
  timer?: ReturnType<typeof setInterval>
  evict?: ReturnType<typeof setTimeout>
}

const STALE_MS = 4000
const EVICT_MS = 60_000
const cache = new Map<string, Entry>()

function entry(key: string): Entry {
  let e = cache.get(key)
  if (!e) {
    e = { fetchedAt: 0, listeners: new Set(), polls: new Map(), snapshot: { data: undefined, error: undefined, loading: true } }
    cache.set(key, e)
  }
  return e
}

function publish(e: Entry) {
  e.snapshot = { data: e.data, error: e.error, loading: !!e.inflight && e.data === undefined }
  e.listeners.forEach((l) => l())
}

function run(key: string): Promise<void> {
  const e = entry(key)
  if (e.inflight || !e.fn) return e.inflight ?? Promise.resolve()
  e.inflight = e
    .fn()
    .then((d) => {
      e.data = d
      e.error = undefined
    })
    .catch((err: unknown) => {
      e.error = toApiError(err)
    })
    .finally(() => {
      e.inflight = undefined
      e.fetchedAt = Date.now()
      publish(e)
    })
  publish(e)
  return e.inflight
}

/** One timer per key, at the shortest interval any subscriber asked for. */
function schedule(key: string) {
  const e = entry(key)
  clearInterval(e.timer)
  e.timer = undefined
  if (!e.polls.size) return
  const ms = Math.min(...e.polls.values())
  e.timer = setInterval(() => {
    if (document.visibilityState === 'visible' && Date.now() - e.fetchedAt >= ms * 0.8) void run(key)
  }, ms)
}

/**
 * `keepPrevious`: while a new key (e.g. a search) loads, return the last key's data instead of
 * nothing, so the UI around it (the search box) stays put.
 */
export function useQuery<T>(key: string | null, fn: () => Promise<T>, opts: { pollMs?: number; keepPrevious?: boolean } = {}) {
  const k = key ?? '__none__'
  if (key) entry(k).fn = fn
  const subscribe = useCallback(
    (l: () => void) => {
      const e = entry(k)
      clearTimeout(e.evict)
      e.listeners.add(l)
      return () => {
        e.listeners.delete(l)
        if (!e.listeners.size) e.evict = setTimeout(() => !e.listeners.size && cache.delete(k), EVICT_MS)
      }
    },
    [k],
  )
  const state = useSyncExternalStore(subscribe, () => entry(k).snapshot) as QueryState<T>

  const { pollMs } = opts
  useEffect(() => {
    if (!key) return
    if (Date.now() - entry(key).fetchedAt > STALE_MS) void run(key)
    if (!pollMs) return
    const id = Symbol()
    entry(key).polls.set(id, pollMs)
    schedule(key)
    return () => {
      entry(key).polls.delete(id)
      schedule(key)
    }
  }, [key, pollMs])

  const [previous, setPrevious] = useState<T | undefined>(undefined)
  if (opts.keepPrevious && key && state.data !== undefined && state.data !== previous) setPrevious(state.data)

  const refetch = useCallback(() => (key ? run(key) : Promise.resolve()), [key])
  if (!key) return { data: undefined, error: undefined, loading: false, refetch }
  return { ...state, data: state.data ?? (opts.keepPrevious ? previous : undefined), refetch }
}

const matches = (key: string, p: string) => key === p || key.startsWith(p + '/')

/** Refetch watched queries under these key paths; forget unwatched ones. */
export function invalidate(...paths: string[]) {
  for (const [k, e] of cache) {
    if (!paths.some((p) => matches(k, p))) continue
    e.fetchedAt = 0
    if (e.listeners.size) void run(k)
  }
}

export function invalidateAll() {
  invalidate(...cache.keys())
}

export function setQueryData<T>(key: string, data: T) {
  const e = entry(key)
  e.data = data
  e.error = undefined
  e.fetchedAt = Date.now()
  publish(e)
}

export function getQueryData<T>(key: string): T | undefined {
  return cache.get(key)?.data as T | undefined
}

export function clearCache() {
  for (const e of cache.values()) clearInterval(e.timer)
  cache.clear()
}
