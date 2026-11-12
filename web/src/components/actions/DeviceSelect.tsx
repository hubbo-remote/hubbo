import { Select } from '@/components/ui/input'
import type { Catalog } from '@/lib/activity'
import { SYSTEM_VALUE } from './start'

/** The pickers' Device select: every device, then Hubbo when it has commands there. "Choose…" only until one is picked; a removed device shows as unknown. */
export function DeviceSelect({ id, value, onChange, catalog, hubbo, className }: { id: string; value: string; onChange: (d: string) => void; catalog: Catalog; hubbo: boolean; className?: string }) {
  return (
    <Select id={id} value={value} onChange={(e) => onChange(e.target.value)} className={className}>
      {!value && (
        <option value="" disabled>
          Choose…
        </option>
      )}
      {value && value !== SYSTEM_VALUE && !catalog.devices.has(value) && <option value={value}>Unknown device</option>}
      {[...catalog.devices.values()].map((d) => (
        <option key={d.id} value={d.id}>
          {d.name}
        </option>
      ))}
      {hubbo && <option value={SYSTEM_VALUE}>Hubbo</option>}
    </Select>
  )
}
