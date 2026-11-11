import { Bluetooth, Cpu, Home, type LucideIcon } from 'lucide-react'
import type { BluetoothStatus, DeviceKind, IntegrationState, IntegrationType } from '@/api/types'

export const HEALTH: Record<IntegrationState, [dotClass: string, label: string]> = {
  connected: ['bg-success', 'Connected'],
  connecting: ['bg-warning', 'Connecting'],
  authenticating: ['bg-warning', 'Signing in'],
  auth_failed: ['bg-destructive', 'Token rejected'],
  unreachable: ['bg-destructive', 'Unreachable'],
  disconnected: ['bg-warning', 'Disconnected'],
  error: ['bg-destructive', 'Error'],
  disabled: ['bg-muted-foreground/40', 'Disabled'],
  not_configured: ['bg-muted-foreground/40', 'Not set up'],
}

/** Unknown states (a newer hub) read as their raw name rather than crashing. */
export const healthLabel = (s: IntegrationState) => HEALTH[s]?.[1] ?? s

export const INTEGRATION_ICON: Record<IntegrationType, LucideIcon> = {
  home_assistant: Home,
  ble_hid: Bluetooth,
  system: Cpu,
}

/** What a device points at, if anything worth showing: an HA entity id. */
export const providerRef = (p: { type: string; entity_id?: string }) => (p.type === 'home_assistant' ? (p.entity_id ?? '') : '')

const BLUETOOTH: Record<BluetoothStatus['state'], [dot: IntegrationState, label: string]> = {
  not_paired: ['not_configured', 'Not paired'],
  pairing: ['connecting', 'Ready to pair'],
  connected: ['connected', 'Connected'],
  not_connected: ['disconnected', 'Not connected'],
}

/** A Bluetooth device's status line, and the dot state that goes with it. */
export function bluetoothLabel(b: BluetoothStatus): string {
  const label = BLUETOOTH[b.state]?.[1] ?? b.state
  return b.state === 'pairing' ? `${label} · ${b.window_s ?? 0} s` : label
}
export const bluetoothDot = (b: BluetoothStatus): IntegrationState => BLUETOOTH[b.state]?.[0] ?? 'error'

/** Integrations the admin sets up: everything but the built-in system one. */
export const isUserIntegration = (i: { type: string }) => i.type !== 'system'

export const KIND_LABEL: Record<DeviceKind, string> = { tv: 'TV', receiver: 'Receiver', speaker: 'Speaker', streamer: 'Streamer', console: 'Game console', light: 'Light', switch: 'Switch', other: 'Other' }
