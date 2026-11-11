import { Bluetooth, Loader2, Unlink, X } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { api } from '@/api/client'
import { getQueryData, invalidate, setQueryData, useQuery } from '@/api/hooks'
import type { BluetoothStatus, Device } from '@/api/types'
import { ErrorState, HealthDot } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { Dialog } from '@/components/ui/dialog'
import { Skeleton } from '@/components/ui/misc'
import { toast, toastError } from '@/components/ui/toast'
import { useConfigLock, useIntegrationConfig } from '@/hooks/data'
import { bluetoothDot, bluetoothLabel } from '@/lib/health'

/** Re-read the devices, which carry each one's status. */
const refreshDevices = () => invalidate('devices')

/**
 * A Bluetooth device's pairing: its status, a pairing window (polled every second while open),
 * cancel and forget. Pairing again replaces only this device's host; a window that ends with no new
 * host keeps the old one.
 */
export function BluetoothPairing({ device }: { device: Device }) {
  const id = device.id
  const key = `pair/${id}`
  const cfg = useIntegrationConfig(device.integration_id) // the name hosts see
  // Poll every second while the window is open (the cached status, or the device's before that loads).
  const open = (getQueryData<BluetoothStatus>(key) ?? device.bluetooth)?.state === 'pairing'
  const status = useQuery(key, () => api.pairStatus(id), { pollMs: open ? 1000 : undefined })
  const state = status.data?.state
  const [busy, setBusy] = useState(false)
  const [confirm, setConfirm] = useState<'pair' | 'forget' | null>(null)
  const [failed, setFailed] = useState(false) // the last window ended with nothing paired
  const watching = useRef(false) // a window we opened (or found open) and haven't seen end
  const lock = useConfigLock()

  // When a window ends on its own, say if nothing paired and re-read the devices.
  useEffect(() => {
    if (state === 'pairing') watching.current = true
    else if (watching.current && state) {
      watching.current = false
      if (state === 'not_paired') setFailed(true)
      refreshDevices()
    }
  }, [state])

  if (status.error) return <ErrorState error={status.error} retry={status.refetch} />
  const s = status.data
  const paired = state === 'connected' || state === 'not_connected'
  const hostName = cfg.data?.settings.type === 'ble_hid' ? cfg.data.settings.device_name : 'Hubbo'

  const run = async (failTitle: string, fn: () => Promise<void>) => {
    setConfirm(null)
    setBusy(true)
    try {
      await fn()
    } catch (e) {
      toastError(failTitle, e)
    } finally {
      setBusy(false)
    }
  }
  const pair = () =>
    run('Couldn’t start pairing', async () => {
      setFailed(false)
      setQueryData(key, await api.pairStart(id)) // starts the poll
    })
  const cancel = () =>
    run('Couldn’t cancel', async () => {
      watching.current = false // cancelled, not failed
      await api.pairCancel(id)
      refreshDevices()
      await status.refetch()
    })
  const forget = () =>
    run('Couldn’t forget the pairing', async () => {
      await api.forgetHost(id)
      setQueryData(key, { state: 'not_paired' })
      refreshDevices()
      toast('Pairing forgotten', { variant: 'success', description: `Also remove “${hostName}” from its Bluetooth settings.` })
    })

  return (
    <Card className="self-start">
      <CardHeader>
        <CardTitle>Bluetooth</CardTitle>
      </CardHeader>
      <CardContent className="grid gap-3 text-sm">
        {!s ? (
          <Skeleton className="h-16" />
        ) : (
          <>
            <p className="flex items-center gap-2" data-testid="bluetooth-state" aria-live="polite">
              <HealthDot state={bluetoothDot(s)} /> {bluetoothLabel(s)}
            </p>
            {state === 'pairing' && <p className="text-muted-foreground">On your device, open Bluetooth settings and choose “{hostName}”.</p>}
            {failed && state === 'not_paired' && <Alert variant="error" title="Nothing paired. Try again." />}
            <div className="flex flex-wrap gap-2">
              {state === 'pairing' ? (
                <Button variant="outline" size="sm" onClick={cancel} disabled={busy}>
                  <X /> Cancel
                </Button>
              ) : (
                <Button variant="outline" size="sm" onClick={() => (paired ? setConfirm('pair') : void pair())} disabled={busy || lock.locked} title={lock.title}>
                  {busy ? <Loader2 className="animate-spin" /> : <Bluetooth />} {paired ? 'Pair again' : 'Pair'}
                </Button>
              )}
              {paired && (
                <Button variant="ghost" size="sm" className="text-destructive hover:text-destructive" onClick={() => setConfirm('forget')} disabled={busy || lock.locked} title={lock.title}>
                  <Unlink /> Forget
                </Button>
              )}
            </div>
          </>
        )}
        <Dialog
          open={confirm !== null}
          onClose={() => setConfirm(null)}
          title={confirm === 'forget' ? 'Forget its pairing?' : 'Pair again?'}
          description={confirm === 'forget' ? `Also remove “${hostName}” from its Bluetooth settings.` : 'This replaces its current pairing.'}
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
                <Button onClick={pair}>Pair</Button>
              )}
            </>
          }
        />
      </CardContent>
    </Card>
  )
}
