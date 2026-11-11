import { useState } from 'react'
import { api } from '@/api/client'
import { getQueryData, invalidate, setQueryData } from '@/api/hooks'
import type { Integration, IntegrationConfig } from '@/api/types'
import { HomeAssistantSettings } from '@/components/admin/HomeAssistantForm'
import { ErrorState, HealthDot, PageHeader } from '@/components/common'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card'
import { Skeleton } from '@/components/ui/misc'
import { Switch } from '@/components/ui/switch'
import { toastError } from '@/components/ui/toast'
import { useIntegrations } from '@/hooks/data'
import { healthLabel, INTEGRATION_ICON, isUserIntegration } from '@/lib/health'
import { plural } from '@/lib/utils'

export default function Integrations() {
  const integrations = useIntegrations(3000) // the cards' state lines are live
  return (
    <>
      <PageHeader title="Integrations" />
      {integrations.error ? (
        <ErrorState error={integrations.error} retry={integrations.refetch} />
      ) : !integrations.data ? (
        <div className="grid gap-4 md:grid-cols-2">{[0, 1].map((i) => <Skeleton key={i} className="h-60" />)}</div>
      ) : (
        <div className="grid items-start gap-4 md:grid-cols-2">
          {integrations.data
            .filter(isUserIntegration)
            .sort((a, b) => order(a) - order(b))
            .map((i) => (
              <IntegrationCard key={i.id} integration={i} />
            ))}
        </div>
      )}
    </>
  )
}

const order = (i: Integration) => (i.type === 'home_assistant' ? 0 : 1)

/**
 * The card's one state line. Off shows only that; Bluetooth's detail says it all ("Connected to
 * Family Room"); others add their detail to the state.
 */
function stateLine(i: Integration) {
  if (!i.enabled || i.state === 'disabled' || !i.detail) return healthLabel(i.state)
  if (i.type === 'ble_hid') return i.detail
  return (
    <>
      {healthLabel(i.state)} <span>· {i.detail}</span>
    </>
  )
}

function IntegrationCard({ integration: i }: { integration: Integration }) {
  const Icon = INTEGRATION_ICON[i.type]
  const [toggling, setToggling] = useState(false)
  const toggle = async (enabled: boolean) => {
    setToggling(true)
    try {
      // Writes are revision-checked; the settings card has usually cached the config already.
      const cfg = getQueryData<IntegrationConfig>(`integration-config/${i.id}`) ?? (await api.integrationConfig(i.id))
      setQueryData(`integration-config/${i.id}`, await api.configureIntegration(i.id, { enabled, revision: cfg.revision }))
      invalidate('integrations', 'status')
    } catch (e) {
      toastError('Couldn’t update', e)
    } finally {
      setToggling(false)
    }
  }
  return (
    <Card className={i.type === 'home_assistant' ? 'md:row-span-2' : undefined}>
      <CardHeader>
        <div className="flex items-start gap-3">
          <div className="rounded-lg bg-muted p-2">
            <Icon className="size-5" aria-hidden />
          </div>
          <div>
            <CardTitle>{i.name}</CardTitle>
            <CardDescription data-testid={`state-${i.id}`} className="mt-1 flex flex-wrap items-center gap-x-2">
              <HealthDot state={i.state} />
              {stateLine(i)}
            </CardDescription>
          </div>
        </div>
        <Switch checked={i.enabled} onCheckedChange={toggle} disabled={toggling} aria-label={`Enable ${i.name}`} />
      </CardHeader>
      <CardContent className="grid gap-4">
        <p className="text-sm text-muted-foreground">{plural(i.device_count, 'device')}</p>
        {i.type === 'home_assistant' && <HomeAssistantSettings id={i.id} />}
      </CardContent>
    </Card>
  )
}
