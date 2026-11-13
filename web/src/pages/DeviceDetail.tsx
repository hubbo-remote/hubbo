import { Loader2, RefreshCw, Trash2 } from 'lucide-react'
import { useState } from 'react'
import { useLocation } from 'wouter'
import { api, errorMessage } from '@/api/client'
import { invalidate, setQueryData } from '@/api/hooks'
import type { Command, Device, DiscoverCandidate } from '@/api/types'
import { HaCandidates } from '@/components/admin/HaCandidates'
import { TestButton, TestResultText, type TestResult } from '@/components/actions/ActionPicker'
import { ParamForm } from '@/components/actions/ParamForm'
import { EntityId, ErrorState, PageHeader } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { Dialog } from '@/components/ui/dialog'
import { Field, Input, Select, ShowErrors } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { toast, toastError } from '@/components/ui/toast'
import { useCapabilities, useConfigLock, useDevice, useKinds } from '@/hooks/data'
import { BluetoothPairing } from '@/components/admin/BluetoothPairing'
import { actionFor, choiceName, choiceParams, choices, domainOf, type Choice } from '@/lib/commands'
import { haDeviceError, loadEntityDetails } from '@/lib/ha'

export default function DeviceDetail({ id }: { id: string }) {
  const device = useDevice(id)
  const caps = useCapabilities(id)
  const d = device.data
  const commands = caps.data?.commands ?? []
  const list = choices(commands, 'test', domainOf(d))
  const more = list.filter((c) => c.more)

  if (device.error) return <ErrorState error={device.error} retry={device.refetch} />

  return (
    <>
      <PageHeader
        back={{ href: '/devices', label: 'Devices' }}
        title={d?.name ?? <Skeleton className="h-8 w-56" />}
        description={d?.provider.type === 'home_assistant' && <EntityId provider={d.provider} />}
      />
      {d?.availability === 'stale' && (
        <Alert variant="error" title="Device not found" className="mb-4" action={d.provider.type === 'home_assistant' ? <HaRelink device={d} label="Fix" /> : undefined}>
          {d.provider.type === 'home_assistant' ? `${d.provider.entity_id} is gone from Home Assistant. Change the entity to keep it working.` : 'Remove it and add it again.'}
        </Alert>
      )}
      <div className="grid max-w-3xl gap-4">
        {d && <DeviceAdmin device={d} />}
        {d?.provider.type === 'ble_hid' && <BluetoothPairing device={d} />}
        <Card>
          <CardHeader>
            <CardTitle>Commands</CardTitle>
          </CardHeader>
          <CardContent className="grid gap-2">
            {caps.error && <ErrorState error={caps.error} retry={caps.refetch} />}
            {caps.loading && [0, 1, 2, 3].map((i) => <Skeleton key={i} className="h-12" />)}
            {caps.data && list.length === 0 && <p className="text-sm text-muted-foreground">No commands.</p>}
            {caps.data && list.filter((c) => !c.more).map((c) => <CommandRow key={rowKey(c, commands)} deviceId={id} choice={c} commands={commands} />)}
            {caps.data && more.length > 0 && <p className="mt-2 text-xs font-medium text-muted-foreground">More</p>}
            {caps.data && more.map((c) => <CommandRow key={rowKey(c, commands)} deviceId={id} choice={c} commands={commands} />)}
          </CardContent>
        </Card>
      </div>
    </>
  )
}

/** A row's identity: its choice and what it asks for, so a refetch that changes the spec (a source gone) starts it afresh, and one that doesn't keeps the user's edits. */
const rowKey = (c: Choice, commands: Command[]) => `${c.id} ${JSON.stringify(choiceParams(c.id, commands, 'test'))}`

/** One of the device's commands by name: any value it needs inline (a source, a key, a code), then Test (a tap). */
function CommandRow({ deviceId, choice, commands }: { deviceId: string; choice: Choice; commands: Command[] }) {
  const [action, setAction] = useState(() => actionFor(choice.id, { integration_id: '', device_id: deviceId }, commands))
  const [tried, setTried] = useState(false) // param errors show once Test is pressed
  const [res, setRes] = useState<TestResult | null>(null) // shown under the name, so Test stays in its column
  const params = choiceParams(choice.id, commands, 'test')
  const name = choiceName(choice)
  return (
    <div role="group" aria-label={name} className="rounded-lg border px-4 py-2">
      <ShowErrors.Provider value={tried}>
        <ParamForm
          commandId={action.command_id}
          name={name}
          params={params}
          values={action.params}
          onChange={(p) => setAction({ ...action, params: p })}
          inline
          before={
            <div className="min-w-0 flex-1 basis-40">
              <p className="font-medium">{name}</p>
              <p aria-live="polite" className="text-xs">
                {res && <TestResultText res={res} />}
              </p>
            </div>
          }
          after={<TestButton action={action} onRun={() => setTried(true)} onResult={setRes} />}
        />
      </ShowErrors.Provider>
    </div>
  )
}

const refreshDevice = (id: string) => invalidate('devices', `devices/${id}`, `caps/${id}`, 'caps-all', 'discover')

/** Point an HA device at another entity (after a rename), which also re-reads its capabilities. */
function HaRelink({ device, label, quiet }: { device: Device; label: string; quiet?: boolean }) {
  const [open, setOpen] = useState(false)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const lock = useConfigLock()
  const relink = async (c: DiscoverCandidate) => {
    setBusy(true)
    setError(null)
    try {
      // The hub snapshots the entity's loaded details, so load them first (about 2 s).
      await loadEntityDetails(device.integration_id, c.provider.entity_id)
      await api.updateDevice(device.id, { provider: c.provider, revision: device.revision })
      refreshDevice(device.id)
      toast('Replaced', { variant: 'success' })
      setOpen(false)
    } catch (e) {
      setError(await haDeviceError(e))
    } finally {
      setBusy(false)
    }
  }
  return (
    <>
      <Button size="sm" variant={quiet ? 'ghost' : 'outline'} onClick={() => (setError(null), setOpen(true))} disabled={lock.locked} title={lock.title}>
        {label}
      </Button>
      <Dialog open={open} onClose={() => setOpen(false)} title={`Replace ${device.name}`} description="Pick the Home Assistant device to use instead. Buttons and steps stay as they are.">
        <div className="grid gap-3">
          {error && <Alert variant="error"><p className="whitespace-pre-line">{error}</p></Alert>}
          {busy ? (
            <p className="flex items-center gap-2 text-sm"><Loader2 className="size-4 animate-spin" /> Replacing…</p>
          ) : (
            <HaCandidates integration={device.integration_id} onPick={relink} emptyText="No other devices to choose from" />
          )}
        </div>
      </Dialog>
    </>
  )
}

/** Re-read an HA device's entity and services (sending its provider unchanged). */
function HaRefresh({ device }: { device: Device }) {
  const [busy, setBusy] = useState(false)
  const lock = useConfigLock()
  const refresh = async () => {
    setBusy(true)
    try {
      // The hub snapshots the entity's loaded details, so load them first (about 2 s).
      if (device.provider.type === 'home_assistant') await loadEntityDetails(device.integration_id, device.provider.entity_id)
      await api.updateDevice(device.id, { provider: device.provider, revision: device.revision })
      refreshDevice(device.id)
      toast('Commands refreshed', { variant: 'success' })
    } catch (e) {
      toast('Couldn’t refresh', { variant: 'error', description: await haDeviceError(e) })
    } finally {
      setBusy(false)
    }
  }
  return (
    <Button size="sm" variant="outline" onClick={refresh} disabled={busy || lock.locked} title={lock.title}>
      {busy ? <Loader2 className="animate-spin" /> : <RefreshCw />} Refresh commands
    </Button>
  )
}

function DeviceAdmin({ device }: { device: Device }) {
  const [name, setName] = useState(device.name)
  const [saving, setSaving] = useState<'name' | 'kind' | null>(null)
  const [confirm, setConfirm] = useState(false)
  const [delError, setDelError] = useState<string | null>(null)
  const lock = useConfigLock()
  const [, navigate] = useLocation()
  const kinds = useKinds().data

  // Name and Type save one at a time (both controls wait), each on the revision the last one returned.
  const save = async (change: { name: string } | { kind: Device['kind'] }, done: string, failed: string) => {
    setSaving('name' in change ? 'name' : 'kind')
    try {
      setQueryData(`devices/${device.id}`, await api.updateDevice(device.id, { ...change, revision: device.revision }))
      invalidate('devices')
      toast(done, { variant: 'success' })
    } catch (e) {
      toastError(failed, e)
    } finally {
      setSaving(null)
    }
  }
  const rename = () => save({ name: name.trim() }, 'Renamed', 'Couldn’t rename')
  const changeKind = (kind: Device['kind']) => save({ kind }, 'Type changed', 'Couldn’t change the type')

  const remove = async () => {
    try {
      await api.deleteDevice(device.id, device.revision)
      invalidate('devices', 'caps-all', 'discover')
      toast(`Removed ${device.name}`, { variant: 'success' })
      navigate('/devices')
    } catch (e) {
      setDelError(errorMessage(e, { details: true }))
    }
  }

  return (
    <Card className="self-start">
      <CardHeader>
        <CardTitle>Device</CardTitle>
      </CardHeader>
      <CardContent className="grid gap-4">
        <Field label="Name" htmlFor="dn">
          <div className="flex gap-2">
            <Input id="dn" value={name} onChange={(e) => setName(e.target.value)} maxLength={64} />
            <Button variant="outline" onClick={rename} disabled={!!saving || !name.trim() || name === device.name || lock.locked} title={lock.title}>
              {saving === 'name' ? <Loader2 className="animate-spin" /> : 'Save'}
            </Button>
          </div>
        </Field>
        <Field label="Type" htmlFor="dk">
          <Select id="dk" value={device.kind} onChange={(e) => void changeKind(e.target.value as Device['kind'])} disabled={!kinds || !!saving || lock.locked} title={lock.title}>
            {(kinds ?? [{ id: device.kind, label: device.kind }]).map((k) => (
              <option key={k.id} value={k.id}>
                {k.label}
              </option>
            ))}
          </Select>
        </Field>
        {device.provider.type === 'home_assistant' && (
          <div className="flex flex-wrap gap-2">
            <HaRefresh device={device} />
          </div>
        )}
        <div className="flex flex-wrap items-center justify-end gap-2">
          {device.provider.type === 'home_assistant' && <HaRelink device={device} label="Replace…" quiet />}
          <Button variant="outline" size="sm" className="text-destructive hover:text-destructive" onClick={() => (setDelError(null), setConfirm(true))} disabled={lock.locked} title={lock.title}>
            <Trash2 /> Remove device
          </Button>
        </div>
      </CardContent>
      <Dialog
        open={confirm}
        onClose={() => setConfirm(false)}
        title={`Remove ${device.name}?`}
        description="Activities that use it must be changed first."
        footer={
          <>
            <Button variant="ghost" onClick={() => setConfirm(false)}>
              Cancel
            </Button>
            <Button variant="destructive" onClick={remove}>
              Remove
            </Button>
          </>
        }
      >
        {delError && <Alert variant="error"><p className="whitespace-pre-line">{delError}</p></Alert>}
      </Dialog>
    </Card>
  )
}
