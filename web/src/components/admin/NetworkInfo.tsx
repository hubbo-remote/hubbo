import { Facts, Section } from '@/components/common'
import { Skeleton } from '@/components/ui/misc'
import { useStatus } from '@/hooks/data'

/**
 * Read-only for now: api/openapi.yaml v0.1 has no network configuration
 * endpoint, so Hubbo uses DHCP and this shows what the hub reports.
 */
export function NetworkInfo({ bare }: { bare?: boolean }) {
  const status = useStatus()
  const n = status.data?.network
  const body = !n ? (
    <Skeleton className="h-20" />
  ) : (
    <div className="grid gap-4">
      <Facts
        className="sm:grid-cols-3"
        rows={[
          ['Status', n.link === 'up' ? 'Connected' : 'Not connected'],
          ['Address', n.ip ?? 'None yet'],
          ['Hostname', n.hostname ? `${n.hostname}.local` : '–'],
        ]}
      />
      <p className="text-sm text-muted-foreground">Reserve this address in your router so it doesn’t change.</p>
    </div>
  )
  if (bare) return body
  return (
    <Section title="Ethernet">
      {body}
    </Section>
  )
}
