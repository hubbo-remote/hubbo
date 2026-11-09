import { ChevronDown, Loader2, RefreshCw, Trash2 } from 'lucide-react'
import { useState } from 'react'
import { useLocation } from 'wouter'
import { api, errorMessage } from '@/api/client'
import { invalidate } from '@/api/hooks'
import type { Command, Device, DiscoverCandidate } from '@/api/types'
import { HaCandidates } from '@/components/admin/HaCandidates'
import { TestButton } from '@/components/actions/ActionPicker'
import { ParamForm } from '@/components/actions/ParamForm'
import { ErrorState, PageHeader } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card'
import { Dialog } from '@/components/ui/dialog'
import { Field, Input } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { toast, toastError } from '@/components/ui/toast'
import { useCapabilities, useConfigLock, useDevice, useIntegrations } from '@/hooks/data'
import { providerRef } from '@/lib/health'
import { defaultParams } from '@/lib/activity'
import { haDeviceError, runDiscovery } from '@/lib/ha'
import { cn } from '@/lib/utils'

export default function DeviceDetail({ id }: { id: string }) {
  const device = useDevice(id)
  const caps = useCapabilities(id)
  const integrations = useIntegrations()
  const [open, setOpen] = useState<string | null>(null)
  const d = device.data
  const integration = integrations.data?.find((i) => i.id === d?.integration_id)

  if (device.error) return <ErrorState error={device.error} retry={device.refetch} />

  return (
    <>
      <PageHeader
        back={{ href: '/devices', label: 'Devices' }}
        title={d?.name ?? <Skeleton className="h-8 w-56" />}
        description={d ? `${integration?.name ?? d.integration_id} · ${providerRef(d.provider)}` : undefined}
      />
      {d?.availability === 'stale' && (
        <Alert variant="error" title="This device needs re-linking" className="mb-4" action={d.provider.type === 'home_assistant' ? <HaRelink device={d} /> : undefined}>
          {d.provider.type === 'home_assistant'
            ? `Home Assistant no longer has ${d.provider.entity_id} (it was renamed or removed). Its commands fail until you re-link it to the entity’s new name.`
            : 'Its integration no longer knows this target. Its commands fail until you remove this device and add it again.'}
        </Alert>
      )}
      <div className="grid gap-4 lg:grid-cols-[minmax(0,1fr)_20rem]">
        <Card>
          <CardHeader>
            <div>
              <CardTitle>Commands</CardTitle>
              <CardDescription>Discovered from the device’s capabilities. Test sends the command now; “accepted” means the integration took it, not that the device reacted.</CardDescription>
            </div>
          </CardHeader>
          <CardContent className="grid gap-2">
            {caps.error && <ErrorState error={caps.error} retry={caps.refetch} />}
            {caps.loading && [0, 1, 2, 3].map((i) => <Skeleton key={i} className="h-12" />)}
            {caps.data?.commands.length === 0 && <p className="text-sm text-muted-foreground">This device reports no commands.</p>}
            {caps.data?.commands.map((c) => (
              <CommandRow key={c.command_id} deviceId={id} command={c} open={open === c.command_id} onToggle={() => setOpen(open === c.command_id ? null : c.command_id)} />
            ))}
          </CardContent>
        </Card>
        {d && <DeviceAdmin device={d} />}
      </div>
    </>
  )
}

function CommandRow({ deviceId, command, open, onToggle }: { deviceId: string; command: Command; open: boolean; onToggle: () => void }) {
  const [params, setParams] = useState(() => defaultParams(command.params))
  // A hold (e.g. key down) needs its release on a button's up event, so the hub
  // refuses it here; it can only be used in a binding.
  const hold = command.effect === 'hold'
  return (
    <div className={cn('rounded-lg border', open && 'bg-muted/30')}>
      <button onClick={onToggle} aria-expanded={open} className="flex w-full items-center gap-3 rounded-lg px-4 py-3 text-left outline-none focus-visible:ring-2 focus-visible:ring-ring cursor-pointer">
        <div className="min-w-0 flex-1">
          <p className="font-medium">{command.label}</p>
          <p className="truncate font-mono text-xs text-muted-foreground">
            {command.command_id}
            {command.params.length > 0 && ` (${command.params.map((p) => p.name).join(', ')})`}
          </p>
        </div>
        {hold && <Badge variant="secondary">Binding only</Badge>}
        <ChevronDown className={cn('size-4 text-muted-foreground transition-transform', open && 'rotate-180')} aria-hidden />
      </button>
      {open && (
        <div className="grid gap-4 border-t px-4 py-4">
          {command.description && <p className="text-sm text-muted-foreground">{command.description}</p>}
          {hold ? (
            <p className="text-sm text-muted-foreground">
              This command holds until it is released, so it can’t be tested on its own. Bind it to a button’s <b>down</b> event; Hubbo adds the release on <b>up</b>.
            </p>
          ) : (
            <>
              <ParamForm params={command.params} values={params} onChange={setParams} compact />
              <TestButton action={{ integration_id: '', device_id: deviceId, command_id: command.command_id, params }} />
            </>
          )}
        </div>
      )}
    </div>
  )
}

const refreshDevice = (id: string) => invalidate('devices', `devices/${id}`, `caps/${id}`, 'caps-all', 'discover')

/** Point an HA device at another entity (after a rename), which also re-reads its capabilities. */
function HaRelink({ device }: { device: Device }) {
  const [open, setOpen] = useState(false)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const lock = useConfigLock()
  const relink = async (c: DiscoverCandidate) => {
    setBusy(true)
    setError(null)
    try {
      await api.updateDevice(device.id, { provider: c.provider, revision: device.revision })
      refreshDevice(device.id)
      toast(`Linked to ${c.provider.entity_id}`, { variant: 'success' })
      setOpen(false)
    } catch (e) {
      setError(haDeviceError(e))
    } finally {
      setBusy(false)
    }
  }
  return (
    <>
      <Button size="sm" variant="outline" onClick={() => (setError(null), setOpen(true))} disabled={lock.locked} title={lock.title}>
        Re-link…
      </Button>
      <Dialog open={open} onClose={() => setOpen(false)} title={`Re-link ${device.name}`} description="Pick the Home Assistant entity this device should control. Its commands are read again from Home Assistant.">
        <div className="grid gap-3">
          {error && <Alert variant="error"><p className="whitespace-pre-line">{error}</p></Alert>}
          {busy ? (
            <p className="flex items-center gap-2 text-sm"><Loader2 className="size-4 animate-spin" /> Re-linking…</p>
          ) : (
            <HaCandidates
              integration={device.integration_id}
              onPick={relink}
              blocker={(c) => (c.registered_device_id && c.registered_device_id !== device.id ? 'Another device' : !c.supported ? 'Not supported' : null)}
            />
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
      // The hub reads the entity from its latest discovery, so take a fresh one first.
      await runDiscovery(device.integration_id)
      await api.updateDevice(device.id, { provider: device.provider, revision: device.revision })
      refreshDevice(device.id)
      toast('Commands refreshed from Home Assistant', { variant: 'success' })
    } catch (e) {
      toast('Could not refresh', { variant: 'error', description: haDeviceError(e) })
    } finally {
      setBusy(false)
    }
  }
  return (
    <Button size="sm" variant="outline" onClick={refresh} disabled={busy || lock.locked} title={lock.title ?? 'Read the entity and its services from Home Assistant again'}>
      {busy ? <Loader2 className="animate-spin" /> : <RefreshCw />} Refresh from Home Assistant
    </Button>
  )
}

function DeviceAdmin({ device }: { device: Device }) {
  const [name, setName] = useState(device.name)
  const [busy, setBusy] = useState(false)
  const [confirm, setConfirm] = useState(false)
  const [delError, setDelError] = useState<string | null>(null)
  const lock = useConfigLock()
  const [, navigate] = useLocation()

  const rename = async () => {
    setBusy(true)
    try {
      await api.updateDevice(device.id, { name: name.trim(), revision: device.revision })
      invalidate('devices')
      toast('Renamed', { variant: 'success' })
    } catch (e) {
      toastError('Could not rename', e)
    } finally {
      setBusy(false)
    }
  }

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
        <dl className="grid gap-1.5 text-sm">
          {[
            ['Hub ID', device.id],
            ['Integration', device.integration_id],
            ['Target', providerRef(device.provider)],
            ['Kind', device.kind],
            ['Availability', device.availability],
          ].map(([k, v]) => (
            <div key={k} className="flex justify-between gap-3">
              <dt className="text-muted-foreground">{k}</dt>
              <dd className="truncate font-mono text-xs">{v}</dd>
            </div>
          ))}
        </dl>
        {device.provider.type === 'home_assistant' && (
          <div className="flex flex-wrap gap-2">
            <HaRefresh device={device} />
            <HaRelink device={device} />
          </div>
        )}
        <Field label="Name" htmlFor="dn">
          <div className="flex gap-2">
            <Input id="dn" value={name} onChange={(e) => setName(e.target.value)} maxLength={64} />
            <Button variant="outline" onClick={rename} disabled={busy || !name.trim() || name === device.name || lock.locked} title={lock.title}>
              {busy ? <Loader2 className="animate-spin" /> : 'Save'}
            </Button>
          </div>
        </Field>
        <Button variant="outline" className="text-destructive hover:text-destructive" onClick={() => (setDelError(null), setConfirm(true))} disabled={lock.locked} title={lock.title}>
          <Trash2 /> Remove device
        </Button>
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
