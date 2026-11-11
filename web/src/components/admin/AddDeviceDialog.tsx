import { Loader2 } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { Link } from 'wouter'
import { api, errorMessage, toApiError } from '@/api/client'
import { invalidate } from '@/api/hooks'
import type { Device, DeviceKind, DiscoverCandidate, Provider } from '@/api/types'
import { EntityPreview, HaCandidates } from '@/components/admin/HaCandidates'
import { haDeviceError, loadEntityDetails, type EntityDetails } from '@/lib/ha'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { Field, Input, Select } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { useDevices, useIntegrations } from '@/hooks/data'
import { INTEGRATION_ICON, isUserIntegration, KIND_LABEL, providerRef } from '@/lib/health'

const KINDS: DeviceKind[] = ['tv', 'receiver', 'speaker', 'streamer', 'console', 'light', 'switch', 'other']
const guessKind = (c: DiscoverCandidate): DeviceKind => (c.domain === 'light' ? 'light' : c.domain === 'switch' ? 'switch' : /tv|oled|bravia|webos/i.test(c.name) ? 'tv' : /denon|marantz|yamaha|onkyo|receiver|avr/i.test(c.name) ? 'receiver' : /sonos|speaker/i.test(c.name) ? 'speaker' : 'other')

interface Pick {
  provider: Provider
  /** HA only: the entity's name, shown above the name field with its entity id. */
  label?: string
  /** HA only: the entity's details, loaded once it's picked (registering needs them). */
  details?: { state: 'loading' } | { state: 'done'; d: EntityDetails } | { state: 'failed'; error: string }
}

/** Integration → discovered target → name (Bluetooth: integration → name; pairing is on the device's page). */
export function AddDeviceDialog({ open, onClose, onAdded }: { open: boolean; onClose: () => void; onAdded?: (d: Device) => void }) {
  const integrations = useIntegrations()
  const options = (integrations.data ?? []).filter((i) => isUserIntegration(i) && i.enabled)
  const [integration, setIntegration] = useState<string | null>(null)
  const [pick, setPick] = useState<Pick | null>(null)
  const loaded = pick?.details?.state === 'done' ? pick.details.d : null
  const devices = useDevices(!!loaded?.registered_device_id).data // only to name it
  const [name, setName] = useState('')
  const [kind, setKind] = useState<DeviceKind>('other')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  // Bumped per details load, so a load for an entity the user moved on from is dropped.
  const loadId = useRef(0)
  const cancelDetails = () => void loadId.current++

  /** Load the picked entity's details: its actions show (and Register enables) when they're in. */
  const loadDetails = async (entityId: string) => {
    const id = ++loadId.current
    const stale = () => id !== loadId.current
    setPick((p) => p && { ...p, details: { state: 'loading' } })
    try {
      const d = await loadEntityDetails(integration!, entityId, stale)
      if (d) setPick((p) => p && { ...p, details: { state: 'done', d } })
    } catch (e) {
      if (!stale()) setPick((p) => p && { ...p, details: { state: 'failed', error: errorMessage(e) } })
    }
  }

  useEffect(() => {
    if (!open) {
      cancelDetails()
      setIntegration(null)
      setPick(null)
      setName('')
      setError(null)
    }
  }, [open])

  const type = options.find((i) => i.id === integration)?.type
  const choose = (id: string, t: string) => {
    setIntegration(id)
    // Bluetooth has nothing to discover: straight to the name.
    if (t === 'ble_hid') {
      setPick({ provider: { type: 'ble_hid' } })
      setName('')
      setKind('streamer')
    }
  }
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
      // Already a device (e.g. the entity was renamed in HA): reload the details, which name it.
      if (pick.provider.type === 'home_assistant' && toApiError(e).details.some((d) => d.code === 'duplicate_id')) return void loadDetails(pick.provider.entity_id)
      setError(pick.provider.type === 'home_assistant' ? await haDeviceError(e) : errorMessage(e, { details: true }))
    } finally {
      setBusy(false)
    }
  }

  const step = !integration ? 1 : !pick ? 2 : 3
  // Back from a picked entity goes to the list; Bluetooth has no list, so it goes to the start.
  const back = () => {
    setPick(null)
    if (step === 3 && type === 'home_assistant') cancelDetails()
    else setIntegration(null)
  }
  return (
    <Dialog
      open={open}
      onClose={onClose}
      title="Add a device"
      description={step === 1 ? 'How is it connected?' : undefined}
      footer={
        <>
          {step > 1 && (
            <Button variant="ghost" className="mr-auto" onClick={back}>
              Back
            </Button>
          )}
          <Button variant="ghost" onClick={onClose}>
            Cancel
          </Button>
          {step === 3 && (
            <Button onClick={submit} disabled={busy || !name.trim() || (!!pick?.details && (!loaded || !loaded.supported || !!loaded.registered_device_id))}>
              {busy && <Loader2 className="animate-spin" />}
              Add device
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
              <button key={i.id} onClick={() => choose(i.id, i.type)} className="flex items-center gap-3 rounded-lg border p-3 text-left outline-none hover:bg-accent focus-visible:ring-[3px] focus-visible:ring-ring/50 cursor-pointer">
                <Icon className="size-5 text-muted-foreground" />
                <div>
                  <p className="font-medium">{i.name}</p>
                  <p className="text-xs text-muted-foreground">{i.type === 'home_assistant' ? 'Media players, lights and switches' : 'TV boxes and streamers'}</p>
                </div>
              </button>
            )
          })}
          {integrations.data && options.length === 0 && <p className="text-sm text-muted-foreground">Turn on an integration first.</p>}
        </div>
      )}
      {step === 2 && type === 'home_assistant' && (
        <HaCandidates
          integration={integration!}
          onPick={(c) => {
            setPick({ provider: c.provider, label: c.name })
            setName(c.name.slice(0, 64))
            setKind(guessKind(c))
            void loadDetails(c.provider.entity_id)
          }}
        />
      )}
      {step === 3 && pick && (
        <div className="grid gap-4">
          {pick.label && (
            <div className="rounded-lg bg-muted px-3 py-2 text-sm">
              <p className="font-medium">{pick.label}</p>
              <p className="font-mono text-xs text-muted-foreground">{providerRef(pick.provider)}</p>
            </div>
          )}
          {pick.details?.state === 'loading' && (
            <p className="flex items-center gap-2 rounded-lg border px-3 py-3 text-sm" role="status">
              <Loader2 className="size-4 animate-spin text-primary" /> Loading commands…
            </p>
          )}
          {loaded?.registered_device_id ? (
            <AlreadyAdded id={loaded.registered_device_id} name={devices?.find((x) => x.id === loaded.registered_device_id)?.name} onOpen={onClose} />
          ) : (
            loaded && <EntityPreview c={loaded} />
          )}
          {pick.details?.state === 'failed' && (
            <Alert variant="error" title="Couldn’t load its commands" action={<Button size="sm" variant="outline" onClick={() => void loadDetails(providerRef(pick.provider))}>Retry</Button>}>
              {pick.details.error}
            </Alert>
          )}
          {error && <Alert variant="error"><p className="whitespace-pre-line">{error}</p></Alert>}
          <Field label="Name" htmlFor="dev-name">
            <Input id="dev-name" value={name} onChange={(e) => setName(e.target.value)} maxLength={64} autoFocus />
          </Field>
          <Field label="Type" htmlFor="dev-kind">
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

/** The entity already has a device (the hub matches renames too): re-link that one instead of adding another. */
function AlreadyAdded({ id, name, onOpen }: { id: string; name?: string; onOpen: () => void }) {
  return (
    <Alert variant="info" title={`Already added as ${name ?? id}`}>
      <Link href={`/devices/${id}`} onClick={onOpen} className="font-medium text-primary underline underline-offset-2">
        Open it
      </Link>
    </Alert>
  )
}
