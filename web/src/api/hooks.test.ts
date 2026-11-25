// @vitest-environment jsdom
import { act, renderHook, waitFor } from '@testing-library/react'
import { afterEach, expect, test } from 'vitest'
import { clearCache, invalidate, useQuery } from './hooks'

afterEach(clearCache)

test('an invalidation during a fetch fetches again: the fetch in flight may predate the write', async () => {
  let saved = 2
  const pending: (() => void)[] = []
  // Each fetch reads the count when it starts, and answers when released.
  const fn = () => {
    const n = saved
    return new Promise<number>((resolve) => pending.push(() => resolve(n)))
  }
  const { result } = renderHook(() => useQuery('things', fn))
  await waitFor(() => expect(pending).toHaveLength(1))
  saved = 3
  invalidate('things') // the write landed after that fetch started
  await act(async () => pending.shift()!())
  expect(result.current.data).toBe(2)
  await waitFor(() => expect(pending).toHaveLength(1))
  await act(async () => pending.shift()!())
  expect(result.current.data).toBe(3)
})

test('staleMs Infinity: fetched once, even across remounts; a failure is fetched again', async () => {
  let calls = 0
  const fn = async () => {
    if (++calls === 1) throw new Error('offline')
    return calls
  }
  const opts = { staleMs: Infinity }
  const first = renderHook(() => useQuery('fixed', fn, opts))
  await waitFor(() => expect(first.result.current.error?.message).toBe('offline'))
  await act(() => first.result.current.refetch()) // Retry
  expect(first.result.current.data).toBe(2)
  first.unmount()
  const again = renderHook(() => useQuery('fixed', fn, opts))
  expect(again.result.current.data).toBe(2)
  await act(async () => {})
  expect(calls).toBe(2)
})
