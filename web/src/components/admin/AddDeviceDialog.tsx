import { Loader2 } from 'lucide-react'
import { useEffect, useState } from 'react'
import { api, errorMessage } from '@/api/client'
import { invalidate } from '@/api/hooks'
import type { Device, DeviceKind, DiscoverCandidate, Provider } from '@/api/types'
import { CandidatePreview, HaCandidates } from '@/components/admin/HaCandidates'
import { haDeviceError } from '@/lib/ha'
import { ErrorState } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { Field, Input, Select } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { useIntegrationConfig, useIntegrations } from '@/hooks/data'
import { INTEGRATION_ICON } from '@/lib/health'

const KINDS: DeviceKind[] = ['tv', 'receiver', 'speaker', 'streamer', 'console', 'light', 'switch', 'other']
const KIND_LABEL: Record<DeviceKind, string> = { tv: 'TV', receiver: 'Receiver', speaker: 'Speaker', streamer: 'Streamer', console: 'Game console', light: 'Light', switch: 'Switch', other: 'Other' }
const guessKind = (c: DiscoverCandidate): DeviceKind => (c.domain === 'light' ? 'light' : c.domain === 'switch' ? 'switch' : /tv|oled|bravia|webos/i.test(c.name) ? 'tv' : /denon|marantz|yamaha|onkyo|receiver|avr/i.test(c.name) ? 'receiver' : /sonos|speaker/i.test(c.name) ? 'speaker' : 'other')

interface Pick {
  provider: Provider
  label: string
  ref: string
  /** HA only: what discovery said about it (for the preview). */
  candidate?: DiscoverCandidate
}

/** Integration → discovered target → name. Only discovered targets can be registered. */
export function AddDeviceDialog({ open, onClose, onAdded }: { open: boolean; onClose: () => void; onAdded?: (d: Device) => void }) {
  const integrations = useIntegrations()
  const options = (integrations.data ?? []).filter((i) => i.type !== 'system' && i.enabled)
  const [integration, setIntegration] = useState<string | null>(null)
  const [pick, setPick] = useState<Pick | null>(null)
  const [name, setName] = useState('')
  const [kind, setKind] = useState<DeviceKind>('other')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    if (!open) {
      setIntegration(null)
      setPick(null)
      setName('')
      setError(null)
    }
  }, [open])

  const type = options.find((i) => i.id === integration)?.type
  const submit = async () => {
    if (!integration || !pick) return
    setBusy(true)
    setError(null)
    try {
      const d = await api.registerDevice({ integration_id: integration, provider: pick.provider, name: name.trim(), kind })
      invalidate('devices', 'discover', 'caps-all', 'integrations')
      onAdded?.(d)
      onClose()
    } catch (e) {
      setError(pick.provider.type === 'home_assistant' ? haDeviceError(e) : errorMessage(e, { details: true }))
    } finally {
      setBusy(false)
    }
  }

  const step = !integration ? 1 : !pick ? 2 : 3
  return (
    <Dialog
      open={open}
      onClose={onClose}
      title="Add a device"
      description={step === 1 ? 'Where does it live?' : step === 2 ? 'Pick what to register. Only registered devices can be used in activities.' : pick?.provider.type === 'home_assistant' ? 'Check what it can do, then give it a name you’ll recognise.' : 'Give it a name you’ll recognise.'}
      footer={
        <>
          {step > 1 && (
            <Button variant="ghost" className="mr-auto" onClick={() => (step === 3 ? setPick(null) : setIntegration(null))}>
              Back
            </Button>
          )}
          <Button variant="ghost" onClick={onClose}>
            Cancel
          </Button>
          {step === 3 && (
            <Button onClick={submit} disabled={busy || !name.trim()}>
              {busy && <Loader2 className="animate-spin" />}
              Register device
            </Button>
          )}
        </>
      }
    >
      {step === 1 && (
        <div className="grid gap-2">
          {integrations.loading && <Skeleton className="h-24" />}
          {options.map((i) => {
            const Icon = INTEGRATION_ICON[i.type]
            return (
              <button key={i.id} onClick={() => setIntegration(i.id)} className="flex items-center gap-3 rounded-lg border p-3 text-left outline-none hover:bg-accent focus-visible:ring-[3px] focus-visible:ring-ring/50 cursor-pointer">
                <Icon className="size-5 text-muted-foreground" />
                <div>
                  <p className="font-medium">{i.name}</p>
                  <p className="text-xs text-muted-foreground">{i.type === 'home_assistant' ? 'Media players, lights and switches' : 'The host paired over Bluetooth (e.g. Apple TV)'}</p>
                </div>
              </button>
            )
          })}
          {integrations.data && options.length === 0 && <p className="text-sm text-muted-foreground">No integrations are enabled. Set one up under Integrations first.</p>}
        </div>
      )}
      {step === 2 && type === 'home_assistant' && (
        <HaCandidates
          integration={integration!}
          onPick={(c) => {
            setPick({ provider: c.provider, label: c.name, ref: c.provider.entity_id, candidate: c })
            setName(c.name.slice(0, 64))
            setKind(guessKind(c))
          }}
        />
      )}
      {step === 2 && type === 'ble_hid' && (
        <BleCandidate
          integration={integration!}
          onPick={(peer) => {
            setPick({ provider: { type: 'ble_hid', ...(peer ? { peer_name: peer } : {}) }, label: peer ?? 'Bluetooth host', ref: 'Bonded host' })
            setName(peer ?? 'Apple TV')
            setKind('streamer')
          }}
        />
      )}
      {step === 3 && pick && (
        <div className="grid gap-4">
          <div className="rounded-lg bg-muted px-3 py-2 text-sm">
            <p className="font-medium">{pick.label}</p>
            <p className="font-mono text-xs text-muted-foreground">{pick.ref}</p>
          </div>
          {pick.candidate && <CandidatePreview c={pick.candidate} />}
          {error && <Alert variant="error"><p className="whitespace-pre-line">{error}</p></Alert>}
          <Field label="Name" htmlFor="dev-name">
            <Input id="dev-name" value={name} onChange={(e) => setName(e.target.value)} maxLength={64} autoFocus />
          </Field>
          <Field label="Kind" htmlFor="dev-kind" hint="Only used for icons and grouping.">
            <Select id="dev-kind" value={kind} onChange={(e) => setKind(e.target.value as DeviceKind)}>
              {KINDS.map((k) => (
                <option key={k} value={k}>
                  {KIND_LABEL[k]}
                </option>
              ))}
            </Select>
          </Field>
        </div>
      )}
    </Dialog>
  )
}

function BleCandidate({ integration, onPick }: { integration: string; onPick: (peer: string | undefined) => void }) {
  const cfg = useIntegrationConfig(integration)
  const s = cfg.data?.settings
  if (cfg.error) return <ErrorState error={cfg.error} retry={cfg.refetch} what="Bluetooth settings" />
  if (!s) return <Skeleton className="h-16" />
  if (s.type !== 'ble_hid' || !s.bonded) {
    return <Alert variant="info" title="Nothing paired yet">Pair a host from Integrations → Bluetooth keyboard first, then come back.</Alert>
  }
  return (
    <button onClick={() => onPick(s.peer_name)} className="flex w-full items-center gap-3 rounded-lg border p-3 text-left outline-none hover:bg-accent focus-visible:ring-[3px] focus-visible:ring-ring/50 cursor-pointer">
      <div className="min-w-0 flex-1">
        <p className="font-medium">{s.peer_name ?? 'Bonded host'}</p>
        <p className="text-xs text-muted-foreground">Bonded with “{s.device_name}”</p>
      </div>
    </button>
  )
}
