import { Loader2, type LucideIcon } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { Link } from 'wouter'
import { api, errorMessage, toApiError } from '@/api/client'
import { invalidate } from '@/api/hooks'
import type { Device, DeviceKind, HomeAssistantProvider, ProfileSummary } from '@/api/types'
import { EntityPreview, HaCandidates } from '@/components/admin/HaCandidates'
import { EntityId } from '@/components/common'
import { haDeviceError, loadEntityDetails, type EntityDetails } from '@/lib/ha'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { Field, Input, Select } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { useDevices, useIntegrations, useKinds, useProfiles } from '@/hooks/data'
import { INTEGRATION_ICON } from '@/lib/health'

/** What's being added: a built-in profile, or a Home Assistant entity (with its details, which registering needs). */
type Pick =
  | { profile: ProfileSummary }
  | { provider: HomeAssistantProvider; label: string; details?: { state: 'loading' } | { state: 'done'; d: EntityDetails } | { state: 'failed'; error: string } }

const matches = (p: ProfileSummary, q: string) => `${p.manufacturer} ${p.model}`.toLowerCase().includes(q.trim().toLowerCase())

/**
 * One list: the built-in profiles (search by manufacturer and model) and "From Home Assistant".
 * A profile → its name; Home Assistant → an entity → its name. It stays open to add the next one;
 * a Bluetooth device goes to its page instead (`onPair`), where it pairs.
 */
export function AddDeviceDialog({ open, onClose, onPair }: { open: boolean; onClose: () => void; onPair?: (d: Device) => void }) {
  const integrations = useIntegrations()
  const enabled = (type: string) => (integrations.data ?? []).filter((i) => i.type === type && i.enabled)
  const ha = enabled('home_assistant')
  // Built-in lists: fetched once the dialog opens.
  const profiles = useProfiles(open)
  const kinds = useKinds(open).data ?? []
  // Profiles need their platform's integration on (Bluetooth for now).
  const offered = (profiles.data ?? []).filter((p) => enabled(p.platform).length > 0)
  const [query, setQuery] = useState('')
  const [integration, setIntegration] = useState<string | null>(null)
  const [pick, setPick] = useState<Pick | null>(null)
  const haPick = pick && 'provider' in pick ? pick : null
  const loaded = haPick?.details?.state === 'done' ? haPick.details.d : null
  const devices = useDevices(!!loaded?.registered_device_id).data // only to name it
  const [name, setName] = useState('')
  const [kind, setKind] = useState<DeviceKind>('other')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [added, setAdded] = useState<string | null>(null) // the last one added, while the dialog stays open
  const [haQuery, setHaQuery] = useState('') // the entity search, kept while one is picked and added
  // Bumped per details load, so a load for an entity the user moved on from is dropped.
  const loadId = useRef(0)
  const cancelDetails = () => void loadId.current++

  /** Load the picked entity's details: its actions show (and Register enables) when they're in. */
  const loadDetails = async (entityId: string) => {
    const id = ++loadId.current
    const stale = () => id !== loadId.current
    const set = (details: Extract<Pick, { provider: unknown }>['details']) => setPick((p) => p && 'provider' in p ? { ...p, details } : p)
    set({ state: 'loading' })
    try {
      const d = await loadEntityDetails(integration!, entityId, stale)
      if (d) set({ state: 'done', d })
    } catch (e) {
      if (!stale()) set({ state: 'failed', error: errorMessage(e) })
    }
  }

  useEffect(() => {
    if (!open) {
      cancelDetails()
      setIntegration(null)
      setPick(null)
      setQuery('')
      setHaQuery('')
      setName('')
      setError(null)
      setAdded(null)
    }
  }, [open])

  const chooseProfile = (p: ProfileSummary) => {
    setPick({ profile: p })
    setName(p.model)
    setKind(p.kind)
  }
  const submit = async () => {
    if (!pick) return
    setBusy(true)
    setError(null)
    try {
      const d = await api.registerDevice('profile' in pick ? { profile_id: pick.profile.id, name: name.trim(), kind } : { integration_id: integration!, provider: pick.provider, name: name.trim(), kind })
      invalidate('devices', 'discover', 'caps-all', 'integrations')
      if (d.provider.type === 'ble_hid' && onPair) {
        onPair(d)
        return onClose()
      }
      // Ready for the next one: HA back to its entities (the added one drops out), a profile to the start.
      setAdded(d.name)
      setPick(null)
    } catch (e) {
      // Already a device (e.g. the entity was renamed in HA): reload the details, which name it.
      if ('provider' in pick && toApiError(e).details.some((d) => d.code === 'duplicate_id')) return void loadDetails(pick.provider.entity_id)
      setError('provider' in pick ? await haDeviceError(e) : errorMessage(e, { details: true }))
    } finally {
      setBusy(false)
    }
  }

  const step = pick ? 3 : integration ? 2 : 1
  // Back from a picked entity goes to the entity list; anything else goes to the start.
  const back = () => {
    setPick(null)
    if (haPick) cancelDetails()
    else {
      setIntegration(null)
      setHaQuery('')
    }
  }
  const shown = offered.filter((p) => matches(p, query))
  return (
    <Dialog
      open={open}
      onClose={onClose}
      title="Add a device"
      description={step === 1 ? 'What are you adding?' : undefined}
      footer={
        <>
          {step > 1 && (
            <Button variant="ghost" className="mr-auto" onClick={back}>
              Back
            </Button>
          )}
          <Button variant="ghost" onClick={onClose}>
            {added ? 'Done' : 'Cancel'}
          </Button>
          {step === 3 && (
            <Button onClick={submit} disabled={busy || !name.trim() || (!!haPick?.details && (!loaded || !loaded.supported || !!loaded.registered_device_id))}>
              {busy && <Loader2 className="animate-spin" />}
              Add device
            </Button>
          )}
        </>
      }
    >
      {added && <Alert variant="success" title={`Added ${added}`} className="mb-4" />}
      {step === 1 && (
        <div className="grid gap-2">
          {(integrations.loading || profiles.loading) && <Skeleton className="h-24" />}
          {offered.length > 0 && (
            <Input type="search" aria-label="Search devices" placeholder="Search devices" value={query} onChange={(e) => setQuery(e.target.value)} autoFocus />
          )}
          {shown.map((p) => (
            <Choice key={p.id} icon={INTEGRATION_ICON[p.platform]} title={p.model} subtitle={[p.manufacturer !== 'Other' && p.manufacturer, enabled(p.platform)[0]?.name].filter(Boolean).join(' · ')} onClick={() => chooseProfile(p)} />
          ))}
          {query && offered.length > 0 && shown.length === 0 && <p className="px-1 text-sm text-muted-foreground">No matches.</p>}
          {ha.map((i) => (
            <Choice key={i.id} icon={INTEGRATION_ICON.home_assistant} title={ha.length > 1 ? `From ${i.name}` : 'From Home Assistant'} subtitle="Media players, lights, covers, scenes and more" onClick={() => setIntegration(i.id)} />
          ))}
          {integrations.data && profiles.data && offered.length === 0 && ha.length === 0 && <p className="text-sm text-muted-foreground">Turn on an integration first.</p>}
        </div>
      )}
      {step === 2 && (
        <HaCandidates
          integration={integration!}
          search={[haQuery, setHaQuery]}
          onPick={(c) => {
            setPick({ provider: c.provider, label: c.name })
            setName(c.name.slice(0, 64))
            setKind(c.suggested_kind)
            void loadDetails(c.provider.entity_id)
          }}
        />
      )}
      {step === 3 && pick && (
        <div className="grid gap-4">
          <div className="rounded-lg bg-muted px-3 py-2 text-sm">
            {'profile' in pick ? (
              <>
                <p className="font-medium">{pick.profile.model}</p>
                <p className="text-xs text-muted-foreground">{pick.profile.manufacturer}</p>
              </>
            ) : (
              <>
                <p className="font-medium">{pick.label}</p>
                <EntityId provider={pick.provider} />
              </>
            )}
          </div>
          {haPick?.details?.state === 'loading' && (
            <p className="flex items-center gap-2 rounded-lg border px-3 py-3 text-sm" role="status">
              <Loader2 className="size-4 animate-spin text-primary" /> Loading commands…
            </p>
          )}
          {loaded?.registered_device_id ? (
            <AlreadyAdded id={loaded.registered_device_id} name={devices?.find((x) => x.id === loaded.registered_device_id)?.name} onOpen={onClose} />
          ) : (
            loaded && <EntityPreview c={loaded} />
          )}
          {haPick?.details?.state === 'failed' && (
            <Alert variant="error" title="Couldn’t load its commands" action={<Button size="sm" variant="outline" onClick={() => void loadDetails(haPick.provider.entity_id)}>Retry</Button>}>
              {haPick.details.error}
            </Alert>
          )}
          {error && <Alert variant="error"><p className="whitespace-pre-line">{error}</p></Alert>}
          <Field label="Name" htmlFor="dev-name">
            <Input id="dev-name" value={name} onChange={(e) => setName(e.target.value)} maxLength={64} autoFocus />
          </Field>
          <Field label="Type" htmlFor="dev-kind">
            <Select id="dev-kind" value={kind} onChange={(e) => setKind(e.target.value as DeviceKind)}>
              {!kinds.some((k) => k.id === kind) && <option value={kind}>{kind}</option>}
              {kinds.map((k) => (
                <option key={k.id} value={k.id}>
                  {k.label}
                </option>
              ))}
            </Select>
          </Field>
        </div>
      )}
    </Dialog>
  )
}

function Choice({ icon: Icon, title, subtitle, onClick }: { icon: LucideIcon; title: string; subtitle: string; onClick: () => void }) {
  return (
    <button onClick={onClick} className="flex cursor-pointer items-center gap-3 rounded-lg border p-3 text-left outline-none hover:bg-accent focus-visible:ring-[3px] focus-visible:ring-ring/50">
      <Icon className="size-5 shrink-0 text-muted-foreground" />
      <div>
        <p className="font-medium">{title}</p>
        <p className="text-xs text-muted-foreground">{subtitle}</p>
      </div>
    </button>
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
