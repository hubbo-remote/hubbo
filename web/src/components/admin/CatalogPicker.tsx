import { useMemo, useState, type ReactNode } from 'react'
import { useQuery } from '@/api/hooks'
import type { IntegrationType } from '@/api/types'
import { Choice } from '@/components/admin/Choice'
import { ErrorState } from '@/components/common'
import { SearchInput, Select } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { useIntegrations, useKinds } from '@/hooks/data'
import { brands, models, normalize, search, type Brand, type CatalogModel } from '@/lib/catalog'
import { INTEGRATION_ICON } from '@/lib/health'
import { humanize } from '@/lib/utils'

/** Catalog files don't change while the dialog's open: fetched once. */
const CATALOG_QUERY = { staleMs: Infinity }

/** How many rows a search shows: enough to pick from, few enough to type on. */
const BRANDS_SHOWN = 8
const MODELS_SHOWN = 50

/** The catalog's manufacturers: just the search box until something's typed, then the matches, alphabetically. */
export function CatalogBrands({ search: [q, setQ], onPick }: { search: [string, (q: string) => void]; onPick: (b: Brand) => void }) {
  const list = useQuery('catalog/brands', brands, CATALOG_QUERY)
  const shown = useMemo(() => search(list.data ?? [], q), [list.data, q])
  return (
    <>
      <SearchInput aria-label="Search manufacturers" placeholder="Search by manufacturer" value={q} onChange={(e) => setQ(e.target.value)} maxLength={64} autoFocus />
      {(normalize(q) || list.error) && (
        <Results list={list} shown={shown} limit={BRANDS_SHOWN}>
          {(b) => <Choice key={b.slug} title={b.name} subtitle={b.count === 1 ? '1 device' : `${b.count.toLocaleString()} devices`} onClick={() => onPick(b)} />}
        </Results>
      )}
    </>
  )
}

/** One manufacturer's devices: search by model, optionally narrowed to one type. A model whose integration is off shows, but can't be picked. */
export function CatalogModels({ brand, search: [q, setQ], onPick }: { brand: Brand; search: [string, (q: string) => void]; onPick: (m: CatalogModel) => void }) {
  const list = useQuery(`catalog/m/${brand.slug}`, () => models(brand.slug), CATALOG_QUERY)
  const hubKinds = useKinds().data
  const integrations = useIntegrations().data
  const kindLabel = (id: string) => hubKinds?.find((k) => k.id === id)?.label ?? humanize(id)
  const [kind, setKind] = useState('')
  const kinds = Object.entries(brand.kinds).sort((a, b) => b[1] - a[1])
  const shown = useMemo(() => search((list.data ?? []).filter((m) => !kind || m.kind === kind), q), [list.data, kind, q])
  return (
    <div className="grid gap-2">
      <p className="font-medium">{brand.name}</p>
      <div className="flex gap-2">
        <SearchInput aria-label="Search models" placeholder="Search by model" value={q} onChange={(e) => setQ(e.target.value)} className="min-w-0 flex-1" maxLength={64} autoFocus />
        {kinds.length > 1 && (
          <Select aria-label="Type" value={kind} onChange={(e) => setKind(e.target.value)} className="w-36">
            <option value="">All types</option>
            {kinds.map(([id]) => (
              <option key={id} value={id}>
                {kindLabel(id)}
              </option>
            ))}
          </Select>
        )}
      </div>
      <Results list={list} shown={shown} limit={MODELS_SHOWN}>
        {(m) => {
          const named = integrations?.filter((i) => i.type === m.platform)
          const name = named?.[0]?.name
          // Until the integrations are in, a model counts as available (the hub says if it isn't).
          const on = !named || named.some((i) => i.enabled)
          return (
            <Choice
              key={m.id}
              icon={INTEGRATION_ICON[m.platform as IntegrationType]}
              title={m.model}
              subtitle={[kindLabel(m.kind), on ? name : `${name ?? 'Its integration'} is off`].filter(Boolean).join(' · ')}
              disabled={!on}
              onClick={() => onPick(m)}
            />
          )
        }}
      </Results>
    </div>
  )
}

/** A catalog list: loading, its failure (with Retry), or the first `limit` matches. */
function Results<T>({ list, shown, limit, children }: { list: { data?: T[]; error?: unknown; refetch: () => void }; shown: T[]; limit: number; children: (item: T) => ReactNode }) {
  if (list.error) return <ErrorState error={list.error} retry={list.refetch} title="Couldn’t load the catalog" />
  if (!list.data) return <Skeleton className="h-24" />
  if (!shown.length) return <p className="px-1 text-sm text-muted-foreground">Nothing matches.</p>
  return (
    <>
      <div className="grid gap-2">{shown.slice(0, limit).map(children)}</div>
      {shown.length > limit && <p className="px-1 text-sm text-muted-foreground">{(shown.length - limit).toLocaleString()} more. Keep typing to narrow it down.</p>}
    </>
  )
}
