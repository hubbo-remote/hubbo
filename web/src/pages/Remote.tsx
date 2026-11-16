import { Loader2, Save } from 'lucide-react'
import { useEffect, useId, useMemo, useState } from 'react'
import { useLocation } from 'wouter'
import { BindingsEditor } from '@/components/bindings/BindingsEditor'
import { ErrorState, PageHeader } from '@/components/common'
import { ConflictDialog } from '@/components/ConflictDialog'
import { Alert } from '@/components/ui/alert'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Select, ShowErrors } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { retryFailed, useActivities, useActivity, useCatalog, useGlobalBindings, useSmartActions, useSmartButtons } from '@/hooks/data'
import type { SlotId } from '@/api/types'
import { activityStore, globalStore, smartStore, toDraft, useDraft, useSaveKey, type Doc, type Store } from '@/hooks/draft'
import { pickedDevices, smartOf, withoutSmartButtons } from '@/lib/smartButtons'
import { cn, same } from '@/lib/utils'

/**
 * The remote's buttons: Global (every activity, and none) or one activity's, picked in the header
 * (`/remote[/:set]`, one route, so switching never remounts the page). The header, picker and remote stay put
 * while the next set loads; until then the last one shows, dimmed. Global also holds the smart buttons (Light 1,
 * Light 2, Switch 1 and Switch 2's devices), a second doc saved with the same Save.
 */
export default function Remote({ activityId }: { activityId?: string }) {
  const [, navigate] = useLocation()
  const pickId = useId()
  const isGlobal = !activityId
  const acts = useActivities()
  const g = useGlobalBindings()
  const smart = useSmartButtons()
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
  // Save (and Cmd/Ctrl+S, below) saves both drafts in order, so neither takes the key itself.
  const ed = useDraft(store, server, cat.catalog, { saveKey: false })
  const sd = useDraft(smartStore, isGlobal && !cat.loading ? smart.data : undefined, cat.catalog, { saveKey: false })
  const { draft } = ed
  const sDraft = sd.draft
  // On Global the slots are the draft's; a device picked but not saved yet gets its actions from the hub at once.
  const picked = useSmartActions(isGlobal ? pickedDevices(smart.data, sDraft?.slots) : [])
  const smartView = useMemo(() => smartOf(smart.data, isGlobal ? sDraft?.slots : undefined, picked.data), [smart.data, isGlobal, sDraft, picked.data])
  // Leaving Global any way (picker, link, back) drops its unsaved smart buttons, as a set change does the bindings.
  useEffect(() => void (isGlobal || sd.discard()), [isGlobal]) // eslint-disable-line react-hooks/exhaustive-deps
  const pickSlot = (slot: SlotId, device: string | null) => sd.update((d) => ({ slots: { ...d.slots, [slot]: device } }))

  const smartDirty = isGlobal && sd.dirty
  const dirty = ed.dirty || smartDirty
  const busy = ed.busy || sd.busy
  const canSave = (ed.canSave || (isGlobal && sd.canSave)) && !busy
  const errors = [...ed.errors, ...(isGlobal ? sd.errors : [])]
  /**
   * The smart buttons first; only once the hub takes them, Global's bindings, without those the new devices
   * take (worked out from the slots as they were, so a pick undone before saving strips nothing).
   */
  const save = async () => {
    const strip = (d: Doc): Doc => (smartDirty && smart.data && sDraft ? { ...d, bindings: withoutSmartButtons(d.bindings, smart.data.slots, sDraft.slots) } : d)
    if (smartDirty && !(await sd.save())) return
    if (ed.dirty || (draft && !same(strip(draft).bindings, draft.bindings))) await ed.save({ prepare: strip })
  }
  useSaveKey(() => canSave && void save())

  const err = g.error ?? act.error ?? smart.error ?? cat.error
  if (err) return <ErrorState error={err} retry={retryFailed(g, act, smart, cat)} />

  const pick = (id: string) => {
    if (dirty && !confirm('Discard unsaved changes?')) return
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
            <Select id={pickId} value={activityId ?? ''} onChange={(e) => pick(e.target.value)} disabled={busy} className="w-auto max-w-full">
              <option value="">Global</option>
              {(acts.data?.activities ?? []).map((a) => (
                <option key={a.id} value={a.id}>
                  {a.name}
                </option>
              ))}
            </Select>
            {ed.pending && draft && <Loader2 className="size-4 animate-spin text-muted-foreground" aria-label="Loading" />}
            {dirty && !ed.pending && <Badge variant="warning">Unsaved changes</Badge>}
          </span>
        }
        actions={
          <Button onClick={() => void save()} disabled={!canSave} title={ed.lock.title}>
            {busy ? <Loader2 className="animate-spin" /> : <Save />} Save
          </Button>
        }
      />
      {errors.length > 0 && (
        <Alert variant="error" title="Fix this to save" className="mb-4">
          {errors[0]!.message}
        </Alert>
      )}
      {!draft || !g.data || (isGlobal && !sDraft) ? (
        <Skeleton className="h-[32rem]" />
      ) : (
        <div aria-busy={ed.pending} className={cn('transition-opacity', ed.pending && 'pointer-events-none opacity-60')}>
          <BindingsEditor
            set={'name' in draft ? { source: 'activity', globals: g.data, activityId: draft.id } : { source: 'global' }}
            smart={smartView}
            onPickSlot={'name' in draft ? undefined : pickSlot}
            bindings={draft.bindings}
            macros={draft.macros}
            onChange={ed.update}
            catalog={cat.catalog}
            errors={ed.errors}
            warnings={ed.warnings}
            selected={selected}
            onSelect={setSelected}
          />
        </div>
      )}
      <ConflictDialog {...ed.conflict} busy={ed.busy} />
      <ConflictDialog {...sd.conflict} busy={sd.busy} />
    </ShowErrors.Provider>
  )
}
