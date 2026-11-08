import { useState } from 'react'
import { api } from '@/api/client'
import { getQueryData, invalidate, setQueryData } from '@/api/hooks'
import type { Integration, IntegrationConfig } from '@/api/types'
import { BlePanel } from '@/components/admin/BlePanel'
import { HomeAssistantSettings } from '@/components/admin/HomeAssistantForm'
import { ErrorState, HealthDot, PageHeader } from '@/components/common'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card'
import { Skeleton } from '@/components/ui/misc'
import { Switch } from '@/components/ui/switch'
import { toast, toastError } from '@/components/ui/toast'
import { useIntegrations } from '@/hooks/data'
import { healthLabel, INTEGRATION_ICON } from '@/lib/health'
import { plural } from '@/lib/utils'

export default function Integrations() {
  const integrations = useIntegrations()
  return (
    <>
      <PageHeader title="Integrations" description="How Hubbo reaches your devices. Each integration manages its own connection and pairing." />
      {integrations.error ? (
        <ErrorState error={integrations.error} retry={integrations.refetch} />
      ) : !integrations.data ? (
        <div className="grid gap-4 md:grid-cols-2">{[0, 1].map((i) => <Skeleton key={i} className="h-60" />)}</div>
      ) : (
        <div className="grid items-start gap-4 md:grid-cols-2">
          {[...integrations.data]
            .sort((a, b) => order(a) - order(b))
            .map((i) => (
              <IntegrationCard key={i.id} integration={i} />
            ))}
        </div>
      )}
    </>
  )
}

const order = (i: Integration) => ({ home_assistant: 0, ble_hid: 1, system: 2 })[i.type]

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
      toastError('Could not update integration', e)
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
            <CardDescription className="mt-1 flex flex-wrap items-center gap-x-2">
              <HealthDot state={i.state} /> {healthLabel(i.state)}
              {i.detail && <span>· {i.detail}</span>}
            </CardDescription>
          </div>
        </div>
        {i.type !== 'system' && <Switch checked={i.enabled} onCheckedChange={toggle} disabled={toggling} aria-label={`Enable ${i.name}`} />}
      </CardHeader>
      <CardContent className="grid gap-4">
        {i.type !== 'system' && (
          <p className="text-sm text-muted-foreground">
            {plural(i.device_count, 'registered device')}
          </p>
        )}
        {i.type === 'home_assistant' && <HomeAssistantSettings id={i.id} onSaved={() => toast('Home Assistant settings saved', { variant: 'success' })} />}
        {i.type === 'ble_hid' && <BlePanel id={i.id} />}
        {i.type === 'system' && (
          <div className="grid gap-2 text-sm text-muted-foreground">
            <p>Built-in commands. Always on.</p>
            <ul className="grid gap-1">
              {(i.commands ?? []).map((c) => (
                <li key={c.command_id}>
                  <span className="font-medium text-foreground">{c.label}</span>
                  {c.description && <> · {c.description}</>}
                </li>
              ))}
            </ul>
          </div>
        )}
      </CardContent>
    </Card>
  )
}
