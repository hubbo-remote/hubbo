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
