import { ChevronRight, Plus, Tv2 } from 'lucide-react'
import { useState } from 'react'
import { Link, useLocation } from 'wouter'
import { AddDeviceDialog } from '@/components/admin/AddDeviceDialog'
import { EmptyState, ErrorState, PageHeader } from '@/components/common'
import type { Device } from '@/api/types'
import { bluetoothLabel, INTEGRATION_ICON } from '@/lib/health'
import { Button } from '@/components/ui/button'
import { Card } from '@/components/ui/card'
import { Skeleton } from '@/components/ui/misc'
import { toast } from '@/components/ui/toast'
import { cn } from '@/lib/utils'
import { useConfigLock, useDevices, useIntegrations } from '@/hooks/data'

export default function Devices() {
  const devices = useDevices()
  const integrations = useIntegrations()
  const [adding, setAdding] = useState(false)
  const lock = useConfigLock()
  const [, navigate] = useLocation()

  const source = new Map((integrations.data ?? []).map((i) => [i.id, i.name]))
  const sorted = [...(devices.data ?? [])].sort((a, b) => a.name.localeCompare(b.name))

  return (
    <>
      <PageHeader
        title="Devices"
        actions={
          <Button onClick={() => setAdding(true)} disabled={lock.locked} title={lock.title}>
            <Plus /> Add device
          </Button>
        }
      />
      {devices.error ? (
        <ErrorState error={devices.error} retry={devices.refetch} />
      ) : !devices.data || !integrations.data ? (
        <div className="grid gap-3">{[0, 1, 2].map((i) => <Skeleton key={i} className="h-16" />)}</div>
      ) : devices.data.length === 0 ? (
        <EmptyState icon={Tv2} title="No devices yet" action={<Button onClick={() => setAdding(true)} disabled={lock.locked} title={lock.title}>Add device</Button>}>
          Add a TV, receiver or anything else you want to control.
        </EmptyState>
      ) : (
        <Card className="divide-y overflow-hidden">
          {sorted.map((d) => {
            const action = todo(d)
            return (
              <Link key={d.id} href={`/devices/${d.id}`} className="flex items-center gap-3 px-4 py-3 outline-none hover:bg-accent/50 focus-visible:bg-accent">
                <PlatformIcon device={d} source={source.get(d.integration_id) ?? d.integration_id} />
                <div className="min-w-0 flex-1">
                  <p className="truncate font-medium">{d.name}</p>
                  {action && <p className={cn('text-xs', d.availability === 'stale' ? 'text-destructive' : 'text-muted-foreground')}>{action}</p>}
                </div>
                <ChevronRight className="size-4 text-muted-foreground" aria-hidden />
              </Link>
            )
          })}
        </Card>
      )}
      <AddDeviceDialog
        open={adding}
        onClose={() => setAdding(false)}
        onAdded={(d) => {
          toast(`Added ${d.name}`, { variant: 'success' })
          navigate(`/devices/${d.id}`)
        }}
      />
    </>
  )
}

const STALE = 'Needs re-link'

/** What the user has to do about a device, if anything: pair it, or re-link its entity. */
function todo(d: Device): string | null {
  if (d.bluetooth) return d.bluetooth.state === 'not_paired' || d.bluetooth.state === 'pairing' ? bluetoothLabel(d.bluetooth) : null
  return d.availability === 'stale' ? STALE : null
}

/** The device's platform, lit while it's live: a connected host, or an available entity. */
function PlatformIcon({ device: d, source }: { device: Device; source: string }) {
  const Icon = INTEGRATION_ICON[d.platform]
  const live = d.bluetooth ? d.bluetooth.state === 'connected' : d.availability === 'available'
  const state = d.bluetooth ? bluetoothLabel(d.bluetooth) : d.availability === 'stale' ? STALE : d.availability
  const label = `${source}, ${state.toLowerCase()}`
  return (
    <span role="img" aria-label={label} title={label} className={cn('shrink-0', live ? 'text-primary' : 'text-muted-foreground/50')}>
      <Icon className="size-5" aria-hidden />
    </span>
  )
}
