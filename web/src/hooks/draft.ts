import { useEffect, useMemo, useRef, useState } from 'react'
import { api, ApiError } from '@/api/client'
import { invalidate, setQueryData } from '@/api/hooks'
import type { Activity, ActivityDraft, GlobalBindings, ValidationIssue } from '@/api/types'
import { toast, toastError } from '@/components/ui/toast'
import { useConfigLock } from '@/hooks/data'
import { validateActivity, validateBindings, type Catalog } from '@/lib/activity'
import { plural, same } from '@/lib/utils'

/** Something with bindings that saves as a whole: an activity, or the global buttons. */
export type Doc = ActivityDraft | GlobalBindings

/** Where a doc is cached, and how it saves and reloads. */
export interface Store<T extends Doc> {
  /** The query key of its saved copy; also the draft's identity. */
  key: string
  /** Writes it (the hub checks `revision`) and updates the caches; resolves to the saved copy. */
  save: (doc: T, revision: number) => Promise<T>
  /** The hub's current copy, to show in a conflict. */
  latest: () => Promise<T>
}

export const toDraft = (a: Activity): ActivityDraft => ({ id: a.id, revision: a.revision, name: a.name, startup: a.startup, shutdown: a.shutdown, bindings: a.bindings })

export const activityStore = (id: string): Store<ActivityDraft> => ({
  key: `activity/${id}`,
  save: async (d, revision) => {
    const r = await api.replaceActivity(d.id, d, revision)
    setQueryData(`activity/${r.id}`, r)
    invalidate('activities', 'status')
    return toDraft(r)
  },
  latest: async () => toDraft(await api.activity(id)),
})

export const globalStore: Store<GlobalBindings> = {
  key: 'global-bindings',
  save: async (d, revision) => {
    const r = await api.replaceGlobalBindings({ bindings: d.bindings, revision })
    setQueryData('global-bindings', r)
    return r
  },
  latest: () => api.globalBindings(),
}

function validate(d: Doc, catalog: Catalog) {
  if ('name' in d) return validateActivity(d, catalog)
  const errors: ValidationIssue[] = []
  const warnings: ValidationIssue[] = []
  validateBindings(d.bindings, catalog, errors, warnings)
  return { errors, warnings }
}

const fresh = <T extends Doc>(key: string, doc: T) => ({ key, saved: doc, draft: doc, serverIssues: null as ValidationIssue[] | null, attempted: false })

/**
 * The edit state of one saved doc, shared by every page that edits bindings: the draft and its saved
 * copy, validation (errors show from the first Save on, then live), Save (also Cmd/Ctrl+S), the
 * conflict when someone else saved first, and a guard against closing the tab with unsaved changes.
 *
 * `server` is the saved copy for `store.key`, or undefined while it loads; until it arrives the last
 * draft stays (`pending`). The draft starts over when the key changes, or when the saved copy gets a
 * new revision while there are no unsaved changes, so a background refetch never wipes edits.
 */
export function useDraft<T extends Doc>(store: Store<T>, server: T | undefined, catalog: Catalog) {
  const [s, setS] = useState(() => server && fresh(store.key, server))
  const dirty = !!s && !same(s.draft, s.saved)
  if (server && (!s || s.key !== store.key || (server.revision !== s.saved.revision && !dirty))) setS(fresh(store.key, server))
  const pending = !server || s?.key !== store.key
  const [busy, setBusy] = useState(false)
  const [conflict, setConflict] = useState<T | null>(null)
  const lock = useConfigLock()
  const keyNow = useRef(store.key)
  useEffect(() => void (keyNow.current = store.key))

  const local = useMemo(() => (s ? validate(s.draft, catalog) : { errors: [], warnings: [] }), [s?.draft, catalog]) // eslint-disable-line react-hooks/exhaustive-deps
  const showErrors = !!s && (s.attempted || s.serverIssues !== null)
  const errors = showErrors ? (s.serverIssues ?? local.errors) : []
  const canSave = !!s && dirty && !busy && !pending && !lock.locked

  const update = (patch: Partial<T>) => setS((x) => x && { ...x, draft: { ...x.draft, ...patch }, serverIssues: null })

  const save = async (overwriteRevision?: number) => {
    if (!s || pending) return
    const at = s.key
    const draft = s.draft
    setS((x) => x && { ...x, attempted: true })
    if (local.errors.length) return
    setBusy(true)
    try {
      const r = await store.save(draft, overwriteRevision ?? s.saved.revision)
      // The picker may have moved on while this saved; only the doc it saved takes the result.
      setS((x) => (x?.key === at ? fresh(at, r) : x))
      setConflict(null)
      toast('Saved', { variant: 'success' })
    } catch (e) {
      if (keyNow.current !== at) return
      if (e instanceof ApiError && e.code === 'revision_conflict') {
        // The 409 only carries current_revision; fetch the hub's copy to show what changed.
        try {
          const theirs = await store.latest()
          if (keyNow.current === at) setConflict(theirs)
        } catch {
          toast('Changed somewhere else', { variant: 'error', description: 'Reload to get the latest.' })
        }
      } else if (e instanceof ApiError && e.details.length) {
        setS((x) => x && { ...x, serverIssues: e.details })
        toast('Couldn’t save', { variant: 'error', description: `${plural(e.details.length, 'problem')} to fix` })
      } else {
        toastError('Couldn’t save', e)
      }
    } finally {
      setBusy(false)
    }
  }

  const loadTheirs = () => {
    if (!conflict) return
    setS(fresh(store.key, conflict))
    setConflict(null)
    invalidate(store.key)
    toast('Loaded the latest version')
  }

  // Unsaved-changes guard for tab close / reload.
  useEffect(() => {
    if (!dirty) return
    const h = (e: BeforeUnloadEvent) => e.preventDefault()
    addEventListener('beforeunload', h)
    return () => removeEventListener('beforeunload', h)
  }, [dirty])

  // Cmd/Ctrl+S saves. The listener is attached once and reads the latest state through a ref.
  const onSaveKey = useRef<() => void>(() => {})
  useEffect(() => {
    onSaveKey.current = () => canSave && void save()
  })
  useEffect(() => {
    const h = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && e.key === 's') {
        e.preventDefault()
        onSaveKey.current()
      }
    }
    addEventListener('keydown', h)
    return () => removeEventListener('keydown', h)
  }, [])

  return {
    /** The draft being edited, or undefined before the first load. */
    draft: s?.draft,
    update,
    dirty,
    pending,
    busy,
    canSave,
    lock,
    errors,
    warnings: local.warnings,
    /** True once a Save was tried: errors show from then on. */
    showErrors,
    save,
    conflict: { mine: s?.draft, theirs: conflict, close: () => setConflict(null), loadTheirs, overwrite: () => conflict && void save(conflict.revision) },
  }
}
