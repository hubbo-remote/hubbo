import { api } from '@/api/client'
import type { Session } from '@/api/types'
import { sleep } from '@/lib/utils'

const POLL_MS = 1000
/** A restart takes seconds (a firmware write under a minute); after this long without the hub, say so. */
export const RESTART_SLOW_MS = 3 * 60_000
export const NOT_BACK = 'Hubbo hasn’t come back. Check that it’s on and connected.'

/**
 * A plain restart is over: the hub stopped answering and answers again, or the session it had is
 * gone. The simulator without sign-in never goes down or drops anything, so any answer will do.
 */
export const backFromRestart = (s: Session, wasDown: boolean) => wasDown || !s.authenticated || s.auth_mode === 'simulator_no_auth'

/**
 * Follows a hub restart: polls the public session once a second (not /status: every session is gone
 * after a restart) until `back` says the hub is back, then opens the app afresh, on the sign-in or
 * setup when the hub asks for them. `wasDown`: the hub stopped answering at some point. `onSlow`
 * runs once if that takes over RESTART_SLOW_MS. Returns a stop function (for unmounting).
 */
export function followRestart(back: (s: Session, wasDown: boolean) => boolean, onSlow?: () => void): () => void {
  let live = true
  const slow = setTimeout(() => onSlow?.(), RESTART_SLOW_MS)
  void (async () => {
    let wasDown = false
    while (live) {
      await sleep(POLL_MS)
      const s = await api.session().catch(() => null) // not answering: still restarting
      if (!live) return
      if (!s) wasDown = true
      else if (back(s, wasDown)) {
        window.location.hash = '#/'
        return window.location.reload()
      }
    }
  })()
  return () => {
    live = false
    clearTimeout(slow)
  }
}
