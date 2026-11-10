import { Bluetooth, Loader2, Unlink } from 'lucide-react'
import { useEffect, useState } from 'react'
import { api } from '@/api/client'
import { invalidate, setQueryData, useQuery } from '@/api/hooks'
import { ErrorState } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { Skeleton } from '@/components/ui/misc'
import { toast, toastError } from '@/components/ui/toast'
import { useIntegrationConfig } from '@/hooks/data'

/** Re-read everything that shows the bond. */
const refreshBond = () => invalidate('integrations', 'integration-config', 'status')

/**
 * Bluetooth HID: a pairing window (polled about every second while it's open) and forget; the card
 * header says who's paired. A new bond replaces the old one; `failed` (the window closed with no
 * bond) stays until the next pair or forget.
 */
export function BlePanel({ id }: { id: string }) {
  const cfg = useIntegrationConfig(id)
  const key = `ble-pair/${id}`
  const [pairing, setPairing] = useState(false)
  const status = useQuery(key, () => api.blePairStatus(id), { pollMs: pairing ? 1000 : undefined })
  const state = status.data?.state
  const [busy, setBusy] = useState(false)
  const [confirm, setConfirm] = useState<'pair' | 'forget' | null>(null)

  // Poll while the window is open; once it closes, re-read the bond (paired, or not).
  useEffect(() => {
    if (state === 'pairing') setPairing(true)
    else if (pairing) {
      setPairing(false)
      refreshBond()
    }
  }, [state, pairing])

  const s = cfg.data?.settings
  if (cfg.error) return <ErrorState error={cfg.error} retry={cfg.refetch} />
  if (!s || s.type !== 'ble_hid') return <Skeleton className="h-24" />
  // The pair status is the live word on the bond (the hub may not keep the peer's name in the settings).
  const bonded = state === undefined ? s.bonded : state === 'paired'
  const peer = (status.data?.state === 'paired' ? status.data.peer_name : undefined) ?? s.peer_name ?? null

  const pair = async () => {
    setConfirm(null)
    setBusy(true)
    try {
      setQueryData(key, await api.blePairStart(id))
      refreshBond() // the old bond is gone
    } catch (e) {
      toastError('Couldn’t start pairing', e)
    } finally {
      setBusy(false)
    }
  }
  const forget = async () => {
    setConfirm(null)
    setBusy(true)
    try {
      await api.bleForget(id)
      setQueryData(key, { state: 'idle' })
      refreshBond()
      toast(`Forgot ${peer ?? 'the device'}`, { variant: 'success', description: `Also remove “${s.device_name}” from its Bluetooth settings.` })
    } catch (e) {
      toastError('Couldn’t forget the device', e)
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="grid gap-3 text-sm">
      <div aria-live="polite" className="grid gap-3 empty:hidden">
        {state === 'pairing' && (
          <Alert variant="info" title={`On your device, open Bluetooth settings and choose “${s.device_name}”.`}>
            {status.data?.window_s ?? 0} s left
          </Alert>
        )}
        {state === 'failed' && <Alert variant="error" title="Nothing paired. Try again." />}
      </div>
      <div className="flex flex-wrap gap-2">
        <Button variant="outline" size="sm" onClick={() => (bonded ? setConfirm('pair') : void pair())} disabled={busy || state === 'pairing'}>
          {busy ? <Loader2 className="animate-spin" /> : <Bluetooth />} {bonded ? 'Pair another device' : 'Pair a device'}
        </Button>
        {bonded && (
          <Button variant="ghost" size="sm" className="text-destructive hover:text-destructive" onClick={() => setConfirm('forget')} disabled={busy}>
            <Unlink /> Forget
          </Button>
        )}
      </div>

      <Dialog
        open={confirm !== null}
        onClose={() => setConfirm(null)}
        title={confirm === 'forget' ? `Forget ${peer ?? 'this device'}?` : 'Pair another device?'}
        description={confirm === 'forget' ? `Also remove “${s.device_name}” from its Bluetooth settings.` : `This replaces ${peer ?? 'the paired device'}.`}
        footer={
          <>
            <Button variant="ghost" onClick={() => setConfirm(null)}>
              Cancel
            </Button>
            {confirm === 'forget' ? (
              <Button variant="destructive" onClick={forget}>
                Forget
              </Button>
            ) : (
              <Button onClick={pair}>Start pairing</Button>
            )}
          </>
        }
      />
    </div>
  )
}
