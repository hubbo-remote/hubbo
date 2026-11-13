import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import type { Session } from '@/api/types'

const session = vi.fn<() => Promise<Session>>()
vi.mock('@/api/client', () => ({ api: { session: () => session() } }))
const { backFromRestart, followRestart, RESTART_SLOW_MS } = await import('./restart')

const signedIn: Session = { authenticated: true, role: 'admin', setup_required: false, auth_state: 'ok', auth_mode: 'enforced' }
const signedOut: Session = { ...signedIn, authenticated: false, role: null }
const down = () => Promise.reject(new Error('not answering'))

describe('backFromRestart', () => {
  it('is back once the hub went down and answers, or the session is gone', () => {
    expect(backFromRestart(signedIn, false)).toBe(false) // still the old hub, before it went down
    expect(backFromRestart(signedIn, true)).toBe(true)
    expect(backFromRestart(signedOut, false)).toBe(true)
    expect(backFromRestart({ ...signedIn, auth_mode: 'simulator_no_auth' }, false)).toBe(true)
  })
})

describe('followRestart', () => {
  const reload = vi.fn()
  beforeEach(() => {
    vi.useFakeTimers()
    vi.stubGlobal('window', { location: { hash: '#/settings/hub', reload } })
  })
  afterEach(() => {
    vi.useRealTimers()
    vi.unstubAllGlobals()
    vi.clearAllMocks()
  })

  it('polls through the restart, then opens the app afresh', async () => {
    session.mockResolvedValueOnce(signedIn).mockImplementationOnce(down).mockResolvedValueOnce(signedIn)
    followRestart(backFromRestart)
    await vi.advanceTimersByTimeAsync(2000)
    expect(reload).not.toHaveBeenCalled()
    await vi.advanceTimersByTimeAsync(1000)
    expect(session).toHaveBeenCalledTimes(3)
    expect(window.location.hash).toBe('#/')
    expect(reload).toHaveBeenCalledOnce()
  })

  it('says when it’s slow, and stops when told', async () => {
    session.mockImplementation(down)
    const onSlow = vi.fn()
    const stop = followRestart(backFromRestart, onSlow)
    await vi.advanceTimersByTimeAsync(RESTART_SLOW_MS)
    expect(onSlow).toHaveBeenCalledOnce()
    stop()
    const calls = session.mock.calls.length
    await vi.advanceTimersByTimeAsync(5000)
    expect(session.mock.calls.length).toBeLessThanOrEqual(calls + 1)
    expect(reload).not.toHaveBeenCalled()
  })
})
