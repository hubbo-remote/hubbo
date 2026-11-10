import { CheckCircle2, Loader2, Radio } from 'lucide-react'
import { useEffect, useState } from 'react'
import { api } from '@/api/client'
import { invalidate } from '@/api/hooks'
import { REMOTE_SCAN_TIMEOUT_S } from '@/api/constants'
import type { RemotePairStatus } from '@/api/types'
import { ErrorState } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { PressCheck } from './PressCheck'

/** RF pairing by learning an existing Hub's address while its pair button is pressed. */
export function PairRemote({ paired }: { paired?: boolean }) {
  const [st, setSt] = useState<RemotePairStatus | null>(null)
  const [error, setError] = useState<unknown>(null)
  const scanning = st?.state === 'scanning'

  // A scan started elsewhere (another tab, a reload) keeps going on the hub: pick it up.
  useEffect(() => {
    let live = true
    api.remotePairStatus().then((s) => live && s.state === 'scanning' && setSt(s), () => undefined)
    return () => {
      live = false
    }
  }, [])

  useEffect(() => {
    if (!scanning) return
    const t = setInterval(async () => {
      if (document.visibilityState !== 'visible') return
      try {
        const s = await api.remotePairStatus()
        setSt(s)
        if (s.state !== 'scanning') invalidate('status', 'remote')
      } catch (e) {
        setError(e)
        setSt(null) // stop polling; the error explains why
      }
    }, 1000)
    return () => clearInterval(t)
  }, [scanning])

  const start = async () => {
    setError(null)
    try {
      setSt(await api.remotePairStart())
    } catch (e) {
      setError(e)
    }
  }
  const cancel = async () => {
    try {
      await api.remotePairCancel()
      setSt(await api.remotePairStatus())
    } catch (e) {
      setError(e)
      setSt(null)
    }
    invalidate('status', 'remote')
  }
  const remaining = st?.remaining_s ?? REMOTE_SCAN_TIMEOUT_S
  const elapsed = Math.min(REMOTE_SCAN_TIMEOUT_S, Math.max(0, REMOTE_SCAN_TIMEOUT_S - remaining))

  return (
    <div className="grid gap-4">
      <ol className="grid list-decimal gap-1.5 pl-5 text-sm text-muted-foreground marker:text-foreground">
        <li>Press Scan, then the pair button on your old Hub.</li>
        <li>Press a few buttons on the remote.</li>
      </ol>
      {error != null && <ErrorState error={error} />}
      {st?.state === 'found' ? (
        <>
          <Alert variant="success" title="Remote paired" />
          <PressCheck />
        </>
      ) : st?.state === 'timeout' ? (
        <Alert variant="error" title="No Hub found">
          Press the Hub’s pair button right after you start the scan.
        </Alert>
      ) : st?.state === 'stopped' ? (
        <Alert variant="info" title="Scan stopped" />
      ) : null}
      <div className="flex flex-wrap items-center gap-3">
        {scanning ? (
          <>
            <Button variant="outline" onClick={cancel}>
              Cancel
            </Button>
            <span className="inline-flex items-center gap-2 text-sm" aria-live="polite">
              <Loader2 className="size-4 animate-spin text-primary" /> Scanning… {st.remaining_s !== undefined && `${st.remaining_s} s`}
            </span>
            <div
              role="progressbar"
              aria-label="Scan progress"
              aria-valuemin={0}
              aria-valuemax={REMOTE_SCAN_TIMEOUT_S}
              aria-valuenow={elapsed}
              aria-valuetext={`${remaining} seconds left`}
              className="h-1.5 w-full overflow-hidden rounded-full bg-muted"
            >
              <div className="h-full bg-primary transition-[width] duration-1000 ease-linear" style={{ width: `${(elapsed / REMOTE_SCAN_TIMEOUT_S) * 100}%` }} />
            </div>
          </>
        ) : (
          <Button onClick={start}>
            <Radio /> {paired || st?.state === 'found' ? 'Pair again' : 'Scan'}
          </Button>
        )}
        {paired && !scanning && st?.state !== 'found' && (
          <span className="inline-flex items-center gap-1.5 text-sm text-muted-foreground">
            <CheckCircle2 className="size-4 text-success" /> Paired
          </span>
        )}
      </div>
    </div>
  )
}
