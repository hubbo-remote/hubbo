import { CheckCircle2, Loader2 } from 'lucide-react'
import { useEffect, useState } from 'react'
import { api } from '@/api/client'
import { useQuery } from '@/api/hooks'
import type { RemoteStatus } from '@/api/types'
import { buttonLabel } from '@/lib/buttons'
import { EVENT_VERB, isNewEvent, type RemoteEvent } from '@/lib/remote'

const POLL_MS = 500
/** No press by then: probably the wrong endpoint, so say what to check. */
const PRESS_HINT_MS = 30_000

/**
 * Live proof the remote reaches the hub: after an endpoint is saved or paired, polls
 * /remote/status while the page is visible and shows each press as it arrives. Mount it with a
 * new `key` per save so it starts waiting again.
 */
export function PressCheck() {
  // Shares the cache's poll (one timer per key, paused while the page is hidden).
  const data = useQuery('remote-status', () => api.remoteStatus(), { pollMs: POLL_MS }).data
  const [seen, setSeen] = useState<RemoteStatus>() // the last read compared; the first is the baseline
  const [got, setGot] = useState<RemoteEvent | null>(null)
  const [late, setLate] = useState(false)
  if (data && data !== seen) {
    setSeen(data)
    if (seen && isNewEvent(seen.last_event, data.last_event)) setGot(data.last_event!)
  }

  useEffect(() => {
    const hint = setTimeout(() => setLate(true), PRESS_HINT_MS)
    return () => clearTimeout(hint)
  }, [])

  return (
    <div role="status" aria-live="polite" data-testid="press-check" className="grid gap-1 rounded-lg border px-4 py-3 text-sm">
      {got ? (
        <span className="inline-flex items-center gap-2 font-medium">
          <CheckCircle2 className="size-4 text-success" /> {buttonLabel(got.button)} {EVENT_VERB[got.kind] ?? got.kind}
        </span>
      ) : (
        <>
          <span className="inline-flex items-center gap-2">
            <Loader2 className="size-4 animate-spin text-primary" /> Press any button on the remote.
          </span>
          {late && <span className="text-muted-foreground">Nothing yet. Check the channel and address, or scan instead.</span>}
        </>
      )}
    </div>
  )
}
