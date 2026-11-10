import { CheckCircle2, Loader2 } from 'lucide-react'
import { useEffect, useState } from 'react'
import { api } from '@/api/client'
import { useQuery } from '@/api/hooks'
import type { EventKind, RemoteStatus } from '@/api/types'
import { buttonLabel } from '@/lib/buttons'
import { isNewEvent, type RemoteEvent } from '@/lib/remote'

const POLL_MS = 500
/** No press by then: probably the wrong endpoint, so say what to check. */
const PRESS_HINT_MS = 30_000
const VERB: Record<EventKind, string> = { down: 'pressed', tap: 'pressed', hold: 'held', up: 'released' }

/**
 * Live proof the remote reaches the hub: after an endpoint is saved or paired, polls
 * /remote/status while the page is visible and shows each press as it arrives. Mount it with a
 * new `key` per save so it starts waiting again.
 */
export function PressCheck() {
  // Shares the cache's poll (one timer per key, paused while the page is hidden).
  const data = useQuery('remote-status', () => api.remoteStatus(), { pollMs: POLL_MS }).data
  const [seen, setSeen] = useState<RemoteStatus>() // the last read compared; the first is the baseline
  const [got, setGot] = useState<{ ev: RemoteEvent; n: number } | null>(null)
  const [late, setLate] = useState(false)
  if (data && data !== seen) {
    setSeen(data)
    if (seen && isNewEvent(seen.last_event, data.last_event)) setGot((g) => ({ ev: data.last_event!, n: (g?.n ?? 0) + 1 }))
  }

  useEffect(() => {
    const hint = setTimeout(() => setLate(true), PRESS_HINT_MS)
    return () => clearTimeout(hint)
  }, [])

  return (
    <div role="status" aria-live="polite" data-testid="press-check" className="grid gap-1 rounded-lg border px-4 py-3 text-sm">
      {got ? (
        <span className="inline-flex items-center gap-2 font-medium">
          <CheckCircle2 className="size-4 text-success" /> Got it: {buttonLabel(got.ev.button)} {VERB[got.ev.kind] ?? got.ev.kind}
          {got.n > 1 && <span className="font-normal text-muted-foreground">{` (${got.n} so far)`}</span>}
        </span>
      ) : (
        <>
          <span className="inline-flex items-center gap-2">
            <Loader2 className="size-4 animate-spin text-primary" /> Waiting for a button press… Press any button on the remote.
          </span>
          {late && <span className="text-muted-foreground">Nothing yet. Check that the channel and address match your Hub, or pair by scanning from the Hub instead.</span>}
        </>
      )}
    </div>
  )
}
