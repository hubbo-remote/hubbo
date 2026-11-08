import { Bluetooth, Cpu, Home, type LucideIcon } from 'lucide-react'
import type { IntegrationState, IntegrationType } from '@/api/types'

export const HEALTH: Record<IntegrationState, [dotClass: string, label: string]> = {
  connected: ['bg-success', 'Connected'],
  connecting: ['bg-warning', 'Connecting'],
  disconnected: ['bg-warning', 'Disconnected'],
  error: ['bg-destructive', 'Error'],
  disabled: ['bg-muted-foreground/40', 'Disabled'],
  not_configured: ['bg-muted-foreground/40', 'Not set up'],
}

export const healthLabel = (s: IntegrationState) => HEALTH[s][1]

export const INTEGRATION_ICON: Record<IntegrationType, LucideIcon> = {
  home_assistant: Home,
  ble_hid: Bluetooth,
  system: Cpu,
}

export const providerRef = (p: { type: string; entity_id?: string; peer_name?: string }) =>
  p.type === 'home_assistant' ? (p.entity_id ?? '') : p.peer_name ? `Bluetooth host: ${p.peer_name}` : 'Bluetooth host'
