import { ChevronRight, Plus, Tv2 } from 'lucide-react'
import { useState } from 'react'
import { Link, useLocation } from 'wouter'
import { AddDeviceDialog } from '@/components/admin/AddDeviceDialog'
import { EmptyState, ErrorState, HealthDot, PageHeader } from '@/components/common'
import { INTEGRATION_ICON, providerRef } from '@/lib/health'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card } from '@/components/ui/card'
import { Skeleton } from '@/components/ui/misc'
import { toast } from '@/components/ui/toast'
import { useConfigLock, useDevices, useIntegrations } from '@/hooks/data'

export default function Devices() {
  const devices = useDevices()
  const integrations = useIntegrations()
  const [adding, setAdding] = useState(false)
  const lock = useConfigLock()
  const [, navigate] = useLocation()

  const groups = (integrations.data ?? [])
    .map((i) => ({ integration: i, devices: (devices.data ?? []).filter((d) => d.integration_id === i.id) }))
    .filter((g) => g.devices.length)

  return (
    <>
      <PageHeader
        title="Devices"
        description="The curated list of things Hubbo may control. Activities can only use registered devices."
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
        <EmptyState icon={Tv2} title="No devices yet" action={<Button onClick={() => setAdding(true)} disabled={lock.locked} title={lock.title}>Add your first device</Button>}>
          Register a receiver, TV or Apple TV from one of your integrations.
        </EmptyState>
      ) : (
        <div className="grid gap-6">
          {groups.map(({ integration: i, devices: list }) => {
            const Icon = INTEGRATION_ICON[i.type]
            return (
              <section key={i.id} aria-labelledby={`grp-${i.id}`}>
                <h2 id={`grp-${i.id}`} className="mb-2 flex items-center gap-2 text-sm font-medium text-muted-foreground">
                  <Icon className="size-4" /> {i.name}
                  <HealthDot state={i.state} className="ml-1" />
                </h2>
                <Card className="divide-y overflow-hidden">
                  {list.map((d) => (
                    <Link key={d.id} href={`/devices/${d.id}`} className="flex items-center gap-3 px-4 py-3 outline-none hover:bg-accent/50 focus-visible:bg-accent">
                      <div className="min-w-0 flex-1">
                        <p className="truncate font-medium">{d.name}</p>
                        <p className="truncate font-mono text-xs text-muted-foreground">{providerRef(d.provider)}</p>
                      </div>
                      {d.availability !== 'available' && <Badge variant={d.availability === 'stale' ? 'destructive' : 'secondary'}>{d.availability === 'stale' ? 'Needs re-link' : d.availability}</Badge>}
                      <span className="hidden font-mono text-xs text-muted-foreground sm:inline">{d.id}</span>
                      <ChevronRight className="size-4 text-muted-foreground" aria-hidden />
                    </Link>
                  ))}
                </Card>
              </section>
            )
          })}
        </div>
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
