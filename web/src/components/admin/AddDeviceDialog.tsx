import { FileUp, Loader2 } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { Link } from 'wouter'
import { api, errorMessage, toApiError } from '@/api/client'
import { invalidate } from '@/api/hooks'
import type { Device, DeviceKind, HomeAssistantProvider } from '@/api/types'
import { CatalogBrands, CatalogModels } from '@/components/admin/CatalogPicker'
import { Choice } from '@/components/admin/Choice'
import { EntityPreview, HaCandidates } from '@/components/admin/HaCandidates'
import { EntityId } from '@/components/common'
import { haDeviceError, loadEntityDetails, type EntityDetails } from '@/lib/ha'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { Field, Input, Select } from '@/components/ui/input'
import { useDevices, useIntegrations, useKinds } from '@/hooks/data'
import { profileText, type Brand, type CatalogModel } from '@/lib/catalog'
import { INTEGRATION_ICON } from '@/lib/health'
import { profileFileError, readProfileFile, type ProfileFile } from '@/lib/profileFile'

/** Something the dialog loads for a pick before it can be added. */
type Loaded<T> = { state: 'loading' } | { state: 'done'; d: T } | { state: 'failed'; error: string }

/** A profile to install: what the dialog shows of it, and its text (a file's at once; the catalog's once fetched). */
type ProfilePick = Omit<ProfileFile, 'text'> & { text: Loaded<string>; fromCatalog?: CatalogModel }

/** What's being added: a profile (the catalog's or a file's), or a Home Assistant entity (with its details, which registering needs). */
type Pick = { profile: ProfilePick } | { provider: HomeAssistantProvider; label: string; details?: Loaded<EntityDetails> }

/**
 * Search the catalog (a manufacturer, then a model), "Import a profile file" or "From Home Assistant".
 * A profile → its name; Home Assistant → an entity → its name. It stays open to add the next one;
 * a Bluetooth device goes to its page instead (`onPair`), where it pairs.
 */
export function AddDeviceDialog({ open, onClose, onPair }: { open: boolean; onClose: () => void; onPair?: (d: Device) => void }) {
  const integrations = useIntegrations()
  const enabled = (type: string) => (integrations.data ?? []).filter((i) => i.type === type && i.enabled)
  const ha = enabled('home_assistant')
  const kinds = useKinds(open).data ?? []
  const [brandQuery, setBrandQuery] = useState('')
  const [brand, setBrand] = useState<Brand | null>(null)
  const [modelQuery, setModelQuery] = useState('')
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
  const [fileError, setFileError] = useState<string | null>(null) // a picked file that isn't a profile
  const fileInput = useRef<HTMLInputElement>(null)
  const [haQuery, setHaQuery] = useState('') // the entity search, kept while one is picked and added
  // Bumped per load, so a load for a pick the user moved on from is dropped.
  const loadId = useRef(0)
  const cancelLoad = () => void loadId.current++

  /** Load what the pick needs (Add enables when it's in); `set` puts each state on the pick. */
  const load = async <T,>(fetch: (stale: () => boolean) => Promise<T | null>, set: (l: Loaded<T>) => void) => {
    const id = ++loadId.current
    const stale = () => id !== loadId.current
    set({ state: 'loading' })
    try {
      const d = await fetch(stale)
      if (d !== null && !stale()) set({ state: 'done', d })
    } catch (e) {
      if (!stale()) set({ state: 'failed', error: errorMessage(e) })
    }
  }
  /** The picked entity's details: its actions show when they're in. */
  const loadDetails = (entityId: string) =>
    load((stale) => loadEntityDetails(integration!, entityId, stale), (details) => setPick((p) => (p && 'provider' in p ? { ...p, details } : p)))
  /** The picked catalog device's profile, joined with its codeset. */
  const loadProfile = (m: CatalogModel) =>
    load(() => profileText(m.id), (text) => setPick((p) => (p && 'profile' in p ? { profile: { ...p.profile, text } } : p)))

  useEffect(() => {
    if (!open) {
      cancelLoad()
      setIntegration(null)
      setPick(null)
      setBrand(null)
      setBrandQuery('')
      setModelQuery('')
      setHaQuery('')
      setName('')
      setError(null)
      setFileError(null)
      setAdded(null)
    }
  }, [open])

  /** A picked file: a profile goes on to its name; anything else says why, and nothing is sent. */
  const chooseFile = async (f: File | undefined) => {
    if (!f) return
    const read = readProfileFile(f.name, await f.text())
    if ('error' in read) return setFileError(read.error)
    setFileError(null)
    chooseProfile({ ...read, text: { state: 'done', d: read.text } })
  }
  const chooseModel = (b: Brand, m: CatalogModel) => {
    chooseProfile({ manufacturer: b.name, model: m.model, platform: m.platform, kind: m.kind, text: { state: 'loading' }, fromCatalog: m })
    void loadProfile(m)
  }

  const chooseProfile = (p: ProfilePick) => {
    setError(null)
    setPick({ profile: p })
    setName(p.model.slice(0, 64))
    setKind((p.kind ?? 'other') as DeviceKind) // the Type picker shows a kind it doesn't list
  }
  const submit = async () => {
    if (!pick) return
    const text = 'profile' in pick && pick.profile.text.state === 'done' ? pick.profile.text.d : undefined
    if ('profile' in pick && text === undefined) return // Add waits for it
    setBusy(true)
    setError(null)
    try {
      const d =
        'profile' in pick
          ? await api.importProfile(text!, name.trim(), kind)
          : await api.registerDevice({ integration_id: integration!, provider: pick.provider, name: name.trim(), kind })
      invalidate('devices', 'discover', 'caps-all', 'integrations')
      if (d.provider.type === 'ble_hid' && onPair) {
        onPair(d)
        return onClose()
      }
      // Ready for the next one: HA back to its entities (the added one drops out), a catalog device to its manufacturer's list, a file to the start.
      setAdded(d.name)
      setPick(null)
    } catch (e) {
      // Already a device (e.g. the entity was renamed in HA): reload the details, which name it.
      if ('provider' in pick && toApiError(e).details.some((d) => d.code === 'duplicate_id')) return void loadDetails(pick.provider.entity_id)
      setError('provider' in pick ? await haDeviceError(e) : profileFileError(e, pick.profile.fromCatalog ? 'This device’s profile' : undefined))
    } finally {
      setBusy(false)
    }
  }

  const step = pick ? 3 : integration || brand ? 2 : 1
  // Back one step: from a pick to its list (a file's is the start), from a list to the start.
  const back = () => {
    if (pick) {
      setPick(null)
      cancelLoad()
    } else {
      setIntegration(null)
      setHaQuery('')
      setBrand(null)
      setModelQuery('')
    }
  }
  const profilePick = pick && 'profile' in pick ? pick.profile : null
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
            <Button onClick={submit} disabled={busy || !name.trim() || (!!profilePick && profilePick.text.state !== 'done') || (!!haPick?.details && (!loaded || !loaded.supported || !!loaded.registered_device_id))}>
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
          <CatalogBrands search={[brandQuery, setBrandQuery]} onPick={setBrand} />
          <Choice icon={FileUp} title="Import a profile file" subtitle="A device’s profile, saved as a .json file" onClick={() => fileInput.current?.click()} />
          <input
            ref={fileInput}
            type="file"
            accept=".json,application/json"
            aria-label="Profile file"
            className="hidden"
            onChange={(e) => {
              void chooseFile(e.target.files?.[0])
              e.target.value = '' // the same file again still counts as a pick
            }}
          />
          {fileError && <Alert variant="error">{fileError}</Alert>}
          {ha.map((i) => (
            <Choice key={i.id} icon={INTEGRATION_ICON.home_assistant} title={ha.length > 1 ? `From ${i.name}` : 'From Home Assistant'} subtitle="Media players, lights, covers, scenes and more" onClick={() => setIntegration(i.id)} />
          ))}
        </div>
      )}
      {step === 2 && brand && (
        <CatalogModels
          brand={brand}
          search={[modelQuery, setModelQuery]}
          onPick={(m) => chooseModel(brand, m)}
        />
      )}
      {step === 2 && integration && (
        <HaCandidates
          integration={integration}
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
                <p className="text-xs text-muted-foreground">{[pick.profile.manufacturer, integrations.data?.find((i) => i.type === pick.profile.platform)?.name].filter(Boolean).join(' · ')}</p>
              </>
            ) : (
              <>
                <p className="font-medium">{pick.label}</p>
                <EntityId provider={pick.provider} />
              </>
            )}
          </div>
          {(haPick?.details?.state === 'loading' || profilePick?.text.state === 'loading') && (
            <p className="flex items-center gap-2 rounded-lg border px-3 py-3 text-sm" role="status">
              <Loader2 className="size-4 animate-spin text-primary" /> {haPick ? 'Loading commands…' : 'Loading its profile…'}
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
          {profilePick?.fromCatalog && profilePick.text.state === 'failed' && (
            <Alert variant="error" title="Couldn’t load its profile" action={<Button size="sm" variant="outline" onClick={() => void loadProfile(profilePick.fromCatalog!)}>Retry</Button>}>
              {profilePick.text.error}
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
