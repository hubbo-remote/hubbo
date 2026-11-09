import { Bluetooth, Loader2, Unlink } from 'lucide-react'
import { useEffect, useState } from 'react'
import { api } from '@/api/client'
import { invalidate } from '@/api/hooks'
import { ErrorState } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Skeleton } from '@/components/ui/misc'
import { toast, toastError } from '@/components/ui/toast'
import { useIntegrationConfig } from '@/hooks/data'

/** Bluetooth HID: bonded host, pairing window, forget. */
export function BlePanel({ id }: { id: string }) {
  const cfg = useIntegrationConfig(id)
  const { refetch } = cfg
  const [windowEnds, setWindowEnds] = useState<number | null>(null)
  const [now, setNow] = useState(Date.now())
  const [busy, setBusy] = useState(false)

  useEffect(() => {
    if (!windowEnds) return
    const t = setInterval(() => {
      if (document.visibilityState !== 'visible') return
      setNow(Date.now())
      void refetch()
      if (Date.now() > windowEnds) setWindowEnds(null)
    }, 2000)
    return () => clearInterval(t)
  }, [windowEnds, refetch])

  const s = cfg.data?.settings
  if (cfg.error) return <ErrorState error={cfg.error} retry={cfg.refetch} what="Bluetooth settings" />
  if (!s || s.type !== 'ble_hid') return <Skeleton className="h-24" />

  const pair = async () => {
    setBusy(true)
    try {
      const r = await api.blePairStart(id)
      setWindowEnds(Date.now() + (r.window_s ?? 120) * 1000)
      setNow(Date.now())
    } catch (e) {
      toastError('Could not open pairing', e)
    } finally {
      setBusy(false)
    }
  }
  const forget = async () => {
    try {
      await api.bleForget(id)
      invalidate('integrations', 'integration-config', 'status')
      toast('Bond removed', { variant: 'success' })
    } catch (e) {
      toastError('Could not forget host', e)
    }
  }

  return (
    <div className="grid gap-3 text-sm">
      <p className="text-muted-foreground">
        Hubbo appears as a Bluetooth keyboard named <b className="text-foreground">{s.device_name}</b>.{' '}
        {s.bonded ? (
          <>
            Bonded with <b className="text-foreground">{s.peer_name ?? 'a host'}</b>.
          </>
        ) : (
          'Nothing is paired.'
        )}
      </p>
      {windowEnds && (
        <Alert variant="info" title={`Pairing open · ${Math.max(0, Math.round((windowEnds - now) / 1000))} s left`}>
          On Apple TV: Settings → Remotes and Devices → Bluetooth, then pick “{s.device_name}”. A new bond replaces the old one.
        </Alert>
      )}
      <div className="flex flex-wrap gap-2">
        <Button variant="outline" size="sm" onClick={pair} disabled={busy || !!windowEnds}>
          {busy ? <Loader2 className="animate-spin" /> : <Bluetooth />} {s.bonded ? 'Pair a different host' : 'Pair a host'}
        </Button>
        {s.bonded && (
          <Button variant="ghost" size="sm" className="text-destructive hover:text-destructive" onClick={forget}>
            <Unlink /> Forget host
          </Button>
        )}
      </div>
      <p className="text-xs text-muted-foreground">Held remote buttons send key down, release sends key up. Keys are released on disconnect, activity switch and timeout.</p>
    </div>
  )
}
