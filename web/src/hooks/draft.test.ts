// @vitest-environment jsdom
import { renderHook } from '@testing-library/react'
import { describe, expect, it, vi } from 'vitest'
import type { Catalog } from '@/lib/activity'
import { useDraft, type Store } from './draft'

vi.mock('@/hooks/data', () => ({ useConfigLock: () => ({ locked: false }) }))

type D = { revision: number; slots?: object }
const store = (key: string, validate: Store<D>['validate']): Store<D> => ({ key, save: async (d) => d, latest: async () => ({ revision: 0 }), validate, diff: () => [] })
const catalog = { devices: new Map(), capabilities: new Map(), systemCommands: [], activities: [] } as Catalog

describe('useDraft', () => {
  it('never validates the last set’s draft against the next set’s rules while that one loads', () => {
    const global = store('global', () => ({ errors: [], warnings: [] }))
    const smart = store('smart', (d) => (d.slots ? { errors: [], warnings: [] } : (() => { throw new Error('not a smart doc') })()))
    const { result, rerender } = renderHook(({ s, server, cat }) => useDraft(s, server, cat), { initialProps: { s: global, server: { revision: 1 } as D | undefined, cat: catalog } })
    rerender({ s: smart, server: undefined, cat: { ...catalog } }) // a catalog refetch while the smart buttons load
    expect(result.current.pending).toBe(true)
    rerender({ s: smart, server: { revision: 2, slots: {} }, cat: catalog })
    expect(result.current.pending).toBe(false)
  })
})
