import { RADIO_POWERS } from '@/api/constants'
import type { RadioPower } from '@/api/types'
import { ErrorState } from '@/components/common'
import { Select } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { useHub, useSaveHub } from '@/hooks/data'
import { titleCase } from '@/lib/utils'

/** The remote radio's transmit power; saves on change. */
export function RadioPowerSetting() {
  const hub = useHub()
  const { save, saving } = useSaveHub()
  if (hub.error && !hub.data) return <ErrorState error={hub.error} retry={hub.refetch} />
  if (!hub.data) return <Skeleton className="h-9 w-40" />
  // In a flex row, Select's wrapper (which holds the chevron) is as wide as the select, as in ParamForm's rows.
  return (
    <div className="flex">
      <Select aria-label="Radio transmit power" value={hub.data.radio_power} onChange={(e) => void save({ radio_power: e.target.value as RadioPower })} disabled={saving} className="w-40">
        {RADIO_POWERS.map((p) => (
          <option key={p} value={p}>
            {titleCase(p)}
          </option>
        ))}
      </Select>
    </div>
  )
}
