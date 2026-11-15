import { useEffect, useMemo, useRef, useState } from 'react'
import { api, ApiError } from '@/api/client'
import { invalidate, setQueryData } from '@/api/hooks'
import type { Activity, ActivityDraft, GlobalBindings, LocalValidation, SmartButtons, ValidationIssue } from '@/api/types'
import { toast, toastError } from '@/components/ui/toast'
import { useConfigLock } from '@/hooks/data'
import { parseTrigger, validateActivity, validateBindings, type Catalog } from '@/lib/activity'
import { SLOTS, validateSmartButtons } from '@/lib/smartButtons'
import { buttonLabel } from '@/lib/buttons'
import { plural, same } from '@/lib/utils'

/** A doc with bindings: an activity, or the global buttons. */
export type Doc = ActivityDraft | GlobalBindings
/** Anything saved whole against a revision. */
type Revisioned = { revision: number }

/** Where a doc is cached, and how it saves, reloads, checks itself and tells two copies apart. */
export interface Store<T extends Revisioned> {
  /** The query key of its saved copy; also the draft's identity. */
  key: string
  /** Writes it (the hub checks `revision`) and updates the caches; resolves to the saved copy. */
  save: (doc: T, revision: number) => Promise<T>
  /** The hub's current copy, to show in a conflict. */
  latest: () => Promise<T>
  /** The client's own checks; the hub is authoritative. */
  validate: (doc: T, catalog: Catalog) => Pick<LocalValidation, 'errors' | 'warnings'>
  /** What differs between my draft and the copy someone else saved, one line per part. */
  diff: (mine: T, theirs: T) => string[]
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
  validate: validateActivity,
  diff: (mine, theirs) => [
    ...(mine.name !== theirs.name ? [`Name: “${theirs.name}” saved, “${mine.name}” here`] : []),
    ...(!same(mine.startup, theirs.startup) ? [`Startup: ${plural(theirs.startup.length, 'step')} saved, ${mine.startup.length} here`] : []),
    ...(!same(mine.shutdown, theirs.shutdown) ? [`Shutdown: ${plural(theirs.shutdown.length, 'step')} saved, ${mine.shutdown.length} here`] : []),
    ...bindingsDiff(mine, theirs),
  ],
})

export const globalStore: Store<GlobalBindings> = {
  key: 'global-bindings',
  save: async (d, revision) => {
    const r = await api.replaceGlobalBindings({ bindings: d.bindings, revision })
    setQueryData('global-bindings', r)
    return r
  },
  latest: () => api.globalBindings(),
  validate: (d, catalog) => {
    const errors: ValidationIssue[] = []
    const warnings: ValidationIssue[] = []
    validateBindings(d.bindings, catalog, errors, warnings)
    return { errors, warnings }
  },
  diff: bindingsDiff,
}

export const smartStore: Store<SmartButtons> = {
  key: 'smart-buttons',
  save: async (d, revision) => {
    const r = await api.replaceSmartButtons({ slots: d.slots, revision })
    setQueryData('smart-buttons', r)
    return r
  },
  latest: () => api.smartButtons(),
  validate: validateSmartButtons,
  diff: (mine, theirs) => {
    const changed = SLOTS.filter((s) => !same(mine.slots[s], theirs.slots[s]))
    return changed.length ? [`${changed.map(buttonLabel).join(', ')}: changed`] : []
  },
}

/** How many buttons' bindings differ. */
function bindingsDiff(mine: Doc, theirs: Doc): string[] {
  const keys = new Set([...Object.keys(mine.bindings), ...Object.keys(theirs.bindings)])
  const changed = new Set([...keys].filter((k) => !same(mine.bindings[k], theirs.bindings[k])).map((k) => parseTrigger(k)?.button ?? k))
  return changed.size ? [`Buttons: ${plural(changed.size, 'button')} ${changed.size === 1 ? 'differs' : 'differ'}`] : []
}

const NO_ISSUES = { errors: [] as ValidationIssue[], warnings: [] as ValidationIssue[] }

const fresh = <T extends Revisioned>(key: string, doc: T) => ({ key, saved: doc, draft: doc, serverIssues: null as ValidationIssue[] | null, attempted: false })

/**
 * The edit state of one saved doc, shared by every page that edits bindings: the draft and its saved
 * copy, validation (errors show from the first Save on, then live), Save (also Cmd/Ctrl+S), the
 * conflict when someone else saved first, and a guard against closing the tab with unsaved changes.
 *
 * `server` is the saved copy for `store.key`, or undefined while it loads; until it arrives the last
 * draft stays (`pending`). The draft starts over when the key changes, or when the saved copy gets a
 * new revision while there are no unsaved changes, so a background refetch never wipes edits.
 * `saveKey: false` leaves Cmd/Ctrl+S to the page (one that saves several drafts in order).
 */
export function useDraft<T extends Revisioned>(store: Store<T>, server: T | undefined, catalog: Catalog, { saveKey = true }: { saveKey?: boolean } = {}) {
  const [s, setS] = useState(() => server && fresh(store.key, server))
  const dirty = !!s && !same(s.draft, s.saved)
  if (server && (!s || s.key !== store.key || (server.revision !== s.saved.revision && !dirty))) setS(fresh(store.key, server))
  const pending = !server || s?.key !== store.key
  const [busy, setBusy] = useState(false)
  const [conflict, setConflict] = useState<T | null>(null)
  const lock = useConfigLock()
  const keyNow = useRef(store.key)
  useEffect(() => void (keyNow.current = store.key))

  // Only the store's own draft: until the next set loads, the last one (another kind of doc) stays on show.
  const local = useMemo(() => (s && s.key === store.key ? store.validate(s.draft, catalog) : NO_ISSUES), [s?.draft, s?.key, store, catalog]) // eslint-disable-line react-hooks/exhaustive-deps
  const showErrors = !!s && (s.attempted || s.serverIssues !== null)
  const errors = showErrors ? (s.serverIssues ?? local.errors) : []
  const canSave = !!s && dirty && !busy && !pending && !lock.locked

  /** A patch, or one made from the latest draft (for edits that land after an await). */
  const update = (patch: Partial<T> | ((draft: T) => Partial<T>)) => setS((x) => x && { ...x, draft: { ...x.draft, ...(typeof patch === 'function' ? patch(x.draft) : patch) }, serverIssues: null })

  /**
   * Saves the draft (as `prepare` makes it, if given); resolves to whether the hub took it. A refusal (a
   * conflict, the hub's or the client's checks) shows here and resolves false.
   */
  const save = async ({ revision, prepare }: { revision?: number; prepare?: (draft: T) => T } = {}): Promise<boolean> => {
    if (!s || pending) return false
    const at = s.key
    const draft = prepare ? prepare(s.draft) : s.draft
    setS((x) => x && { ...x, attempted: true })
    if (local.errors.length) return false
    setBusy(true)
    try {
      const r = await store.save(draft, revision ?? s.saved.revision)
      // The picker may have moved on while this saved; only the doc it saved takes the result.
      setS((x) => (x?.key === at ? fresh(at, r) : x))
      setConflict(null)
      toast('Saved', { variant: 'success' })
      return true
    } catch (e) {
      if (keyNow.current !== at) return false
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
      return false
    } finally {
      setBusy(false)
    }
  }

  /** Drop unsaved changes (back to the saved copy). */
  const discard = () => setS((x) => x && fresh(x.key, x.saved))

  const loadTheirs = () => {
    if (!conflict) return
    setS(fresh(store.key, conflict))
    setConflict(null)
    invalidate(store.key)
    toast('Loaded the latest version')
  }

  const conflictDiff = useMemo(() => (s && conflict ? store.diff(s.draft, conflict) : []), [s?.draft, conflict]) // eslint-disable-line react-hooks/exhaustive-deps

  // Unsaved-changes guard for tab close / reload.
  useEffect(() => {
    if (!dirty) return
    const h = (e: BeforeUnloadEvent) => e.preventDefault()
    addEventListener('beforeunload', h)
    return () => removeEventListener('beforeunload', h)
  }, [dirty])

  useSaveKey(() => saveKey && canSave && void save())

  return {
    /** The draft being edited, or undefined before the first load. */
    draft: s?.draft,
    update,
    discard,
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
    conflict: { diff: conflictDiff, open: !!conflict, close: () => setConflict(null), loadTheirs, overwrite: () => conflict && void save({ revision: conflict.revision }) },
  }
}

/** Cmd/Ctrl+S runs `onSave`. The listener is attached once and calls the latest `onSave` through a ref. */
export function useSaveKey(onSave: () => void) {
  const latest = useRef(onSave)
  useEffect(() => void (latest.current = onSave))
  useEffect(() => {
    const h = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && e.key === 's') {
        e.preventDefault()
        latest.current()
      }
    }
    addEventListener('keydown', h)
    return () => removeEventListener('keydown', h)
  }, [])
}
