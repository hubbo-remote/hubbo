import { Section } from '@/components/common'
import { Alert } from '@/components/ui/alert'
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
      <dl className="grid grid-cols-2 gap-x-6 gap-y-3 text-sm sm:grid-cols-3">
        {[
          ['Link', n.link === 'up' ? 'Up' : 'Down'],
          ['Address', n.ip ?? 'None yet'],
          ['Hostname', n.hostname ? `${n.hostname}.local` : '–'],
        ].map(([k, v]) => (
          <div key={k}>
            <dt className="text-xs text-muted-foreground">{k}</dt>
            <dd className="truncate font-medium">{v}</dd>
          </div>
        ))}
      </dl>
      <Alert variant="info">Hubbo gets its address from your router (DHCP). Reserve that address in the router so it doesn’t change. Static addressing comes later.</Alert>
    </div>
  )
  if (bare) return body
  return (
    <Section title="Ethernet" description="Hubbo uses wired Ethernet only. Don’t expose it to the internet.">
      {body}
    </Section>
  )
}
