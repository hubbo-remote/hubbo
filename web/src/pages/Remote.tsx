import { Loader2, Save } from 'lucide-react'
import { useId, useMemo, useState } from 'react'
import { useLocation } from 'wouter'
import { BindingsEditor } from '@/components/bindings/BindingsEditor'
import { ErrorState, PageHeader } from '@/components/common'
import { ConflictDialog } from '@/components/ConflictDialog'
import { Alert } from '@/components/ui/alert'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Select, ShowErrors } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { retryFailed, useActivities, useActivity, useCatalog, useGlobalBindings } from '@/hooks/data'
import { activityStore, globalStore, toDraft, useDraft, type Doc, type Store } from '@/hooks/draft'
import { cn } from '@/lib/utils'

/**
 * The remote's buttons: Global (every activity, and none) or one activity's, picked in the header
 * (`/remote[/:activityId]`, one route, so switching never remounts the page). The header, picker and
 * remote stay put while the next set loads; until then the last one shows, dimmed.
 */
export default function Remote({ activityId }: { activityId?: string }) {
  const [, navigate] = useLocation()
  const pickId = useId()
  const acts = useActivities()
  const g = useGlobalBindings()
  const act = useActivity(activityId ?? null)
  const cat = useCatalog()
  const [selected, setSelected] = useState<string | null>(null)

  const store = useMemo(() => (activityId ? activityStore(activityId) : globalStore) as Store<Doc>, [activityId])
  const a = act.data
  // The picked set's saved copy, once it (and what the editor needs around it) has loaded.
  const server = useMemo<Doc | undefined>(() => {
    if (!g.data || cat.loading) return undefined
    if (!activityId) return g.data
    return a?.id === activityId ? toDraft(a) : undefined
  }, [activityId, a, g.data, cat.loading])
  const ed = useDraft(store, server, cat.catalog)
  const { draft } = ed

  const err = g.error ?? act.error ?? cat.error
  if (err) return <ErrorState error={err} retry={retryFailed(g, act, cat)} />

  const pick = (id: string) => {
    if (ed.dirty && !confirm('Discard unsaved changes?')) return
    navigate(id ? `/remote/${id}` : '/remote')
  }

  return (
    <ShowErrors.Provider value={ed.showErrors}>
      <PageHeader
        title="Remote"
        description={
          <span className="flex flex-wrap items-center gap-2">
            <label htmlFor={pickId} className="sr-only">
              Buttons for
            </label>
            <Select id={pickId} value={activityId ?? ''} onChange={(e) => pick(e.target.value)} disabled={ed.busy} className="w-auto max-w-full">
              <option value="">Global</option>
              {(acts.data?.activities ?? []).map((a) => (
                <option key={a.id} value={a.id}>
                  {a.name}
                </option>
              ))}
            </Select>
            {ed.pending && draft && <Loader2 className="size-4 animate-spin text-muted-foreground" aria-label="Loading" />}
            {ed.dirty && !ed.pending && <Badge variant="warning">Unsaved changes</Badge>}
          </span>
        }
        actions={
          <Button onClick={() => ed.save()} disabled={!ed.canSave} title={ed.lock.title}>
            {ed.busy ? <Loader2 className="animate-spin" /> : <Save />} Save
          </Button>
        }
      />
      {ed.errors.length > 0 && (
        <Alert variant="error" title="Fix this to save" className="mb-4">
          {ed.errors[0]!.message}
        </Alert>
      )}
      {!draft || !g.data ? (
        <Skeleton className="h-[32rem]" />
      ) : (
        <div aria-busy={ed.pending} className={cn('transition-opacity', ed.pending && 'pointer-events-none opacity-60')}>
          <BindingsEditor
            set={'name' in draft ? { source: 'activity', globals: g.data.bindings } : { source: 'global' }}
            bindings={draft.bindings}
            onChange={(bindings) => ed.update({ bindings })}
            catalog={cat.catalog}
            errors={ed.errors}
            warnings={ed.warnings}
            selected={selected}
            onSelect={setSelected}
          />
        </div>
      )}
      <ConflictDialog {...ed.conflict} busy={ed.busy} />
    </ShowErrors.Provider>
  )
}
