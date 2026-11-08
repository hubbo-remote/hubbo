import { CheckCircle2, Loader2, Radio } from 'lucide-react'
import { useEffect, useState } from 'react'
import { api, errorMessage } from '@/api/client'
import { invalidate } from '@/api/hooks'
import type { RemotePairStatus } from '@/api/types'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'

/** RF pairing by learning an existing Hub's address while its pair button is pressed. */
export function PairRemote({ paired, channel, onPaired }: { paired?: boolean; channel?: number; onPaired?: () => void }) {
  const [st, setSt] = useState<RemotePairStatus | null>(null)
  const [error, setError] = useState<string | null>(null)
  const scanning = st?.state === 'scanning'

  useEffect(() => {
    if (!scanning) return
    const t = setInterval(async () => {
      if (document.visibilityState !== 'visible') return
      try {
        const s = await api.remotePairStatus()
        setSt(s)
        if (s.state === 'found') {
          invalidate('status')
          onPaired?.()
        }
      } catch (e) {
        setError(errorMessage(e))
      }
    }, 1000)
    return () => clearInterval(t)
  }, [scanning, onPaired])

  const start = async () => {
    setError(null)
    try {
      setSt(await api.remotePairStart())
    } catch (e) {
      setError(errorMessage(e))
    }
  }
  const cancel = async () => {
    await api.remotePairCancel().catch(() => undefined)
    setSt(null)
  }

  return (
    <div className="grid gap-4">
      <ol className="grid list-decimal gap-1.5 pl-5 text-sm text-muted-foreground marker:text-foreground">
        <li>Put your existing Hub (the one the remote already works with) near Hubbo and power it on.</li>
        <li>
          Press <b className="text-foreground">Start scan</b>, then press the pair button on the back of the Hub.
        </li>
        <li>Press a few buttons on the remote. Hubbo learns the Hub’s address and listens on it.</li>
      </ol>
      <p className="text-xs text-muted-foreground">You only need the old Hub once per remote. Pairing a remote without a Hub isn’t supported yet.</p>
      {error && <Alert variant="error">{error}</Alert>}
      {st?.state === 'found' ? (
        <Alert variant="success" title="Remote paired">
          Listening on channel {st.channel}. {st.message}
        </Alert>
      ) : st?.state === 'failed' || st?.state === 'timeout' ? (
        <Alert variant="error" title={st.state === 'timeout' ? 'No Hub found' : 'Pairing failed'}>
          {st.message ?? 'Make sure the Hub is powered and press its pair button right after starting the scan.'}
        </Alert>
      ) : null}
      <div className="flex flex-wrap items-center gap-3">
        {scanning ? (
          <>
            <Button variant="outline" onClick={cancel}>
              Cancel
            </Button>
            <span className="inline-flex items-center gap-2 text-sm" aria-live="polite">
              <Loader2 className="size-4 animate-spin text-primary" /> {st.message ?? 'Scanning…'} {st.remaining_s !== undefined && `· ${st.remaining_s}s left`}
            </span>
          </>
        ) : (
          <Button onClick={start}>
            <Radio /> {paired || st?.state === 'found' ? 'Re-pair' : 'Start scan'}
          </Button>
        )}
        {paired && !scanning && st?.state !== 'found' && (
          <span className="inline-flex items-center gap-1.5 text-sm text-muted-foreground">
            <CheckCircle2 className="size-4 text-success" /> Paired{channel !== undefined && ` · channel ${channel}`}
          </span>
        )}
      </div>
    </div>
  )
}
