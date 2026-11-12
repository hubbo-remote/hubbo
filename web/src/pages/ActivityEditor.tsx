import { CheckCircle2, Loader2, Pencil, Save, ShieldCheck } from 'lucide-react'
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { api, ApiError } from '@/api/client'
import { invalidate, setQueryData } from '@/api/hooks'
import type { Activity, ActivityDraft, ValidationIssue } from '@/api/types'
import { SequenceEditor } from '@/components/actions/SequenceEditor'
import { BindingsEditor } from '@/components/bindings/BindingsEditor'
import { ErrorState, PageHeader } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card'
import { Dialog } from '@/components/ui/dialog'
import { Input, ShowErrors } from '@/components/ui/input'
import { Kbd, Skeleton } from '@/components/ui/misc'
import { Tabs } from '@/components/ui/tabs'
import { toast, toastError } from '@/components/ui/toast'
import { useActivity, useCatalog, useConfigLock, useGlobalBindings } from '@/hooks/data'
import { buttonForPath, LIMITS, parseTrigger, validateActivity, type Catalog } from '@/lib/activity'
import { cn, plural, same } from '@/lib/utils'

const toDraft = (a: Activity): ActivityDraft => ({ id: a.id, revision: a.revision, name: a.name, startup: a.startup, shutdown: a.shutdown, bindings: a.bindings })
const boundButtons = (b: ActivityDraft['bindings']) => new Set(Object.keys(b).map((k) => parseTrigger(k)?.button)).size

export default function ActivityEditor({ id }: { id: string }) {
  const remote = useActivity(id)
  const { catalog, loading: catLoading, error: catError } = useCatalog()
  const globals = useGlobalBindings() // fetched alongside, not after, the catalog

  if (remote.error) return <ErrorState error={remote.error} retry={remote.refetch} />
  if (catError) return <ErrorState error={catError} />
  if (!remote.data || catLoading) {
    return (
      <div className="grid gap-4">
        <Skeleton className="h-10 w-72" />
        <Skeleton className="h-[32rem]" />
      </div>
    )
  }
  return <Editor key={id} initial={toDraft(remote.data)} catalog={catalog} globalBindings={globals.data?.bindings} />
}

function Editor({ initial, catalog, globalBindings }: { initial: ActivityDraft; catalog: Catalog; globalBindings?: ActivityDraft['bindings'] }) {
  const [saved, setSaved] = useState(initial)
  const [draft, setDraft] = useState(initial)
  const [selected, setSelected] = useState<string | null>('select')
  const [tab, setTab] = useState('buttons')
  const [serverIssues, setServerIssues] = useState<ValidationIssue[] | null>(null)
  const [busy, setBusy] = useState<'save' | 'validate' | null>(null)
  const [conflict, setConflict] = useState<Activity | null>(null)
  const lock = useConfigLock()

  const dirty = useMemo(() => !same(draft, saved), [draft, saved])
  const local = useMemo(() => validateActivity(draft, catalog), [draft, catalog])
  // Errors show from the first Save (or Validate) on, then update live; never before.
  const [attempted, setAttempted] = useState(false)
  const showErrors = attempted || serverIssues !== null
  const errors = showErrors ? (serverIssues ?? local.errors) : []

  const update = useCallback((patch: Partial<ActivityDraft>) => {
    setDraft((d) => ({ ...d, ...patch }))
    setServerIssues(null)
  }, [])

  // Unsaved-changes guard for tab close / reload.
  useEffect(() => {
    if (!dirty) return
    const h = (e: BeforeUnloadEvent) => e.preventDefault()
    addEventListener('beforeunload', h)
    return () => removeEventListener('beforeunload', h)
  }, [dirty])

  const countFor = (prefix: string) => errors.filter((e) => e.path.startsWith(prefix)).length

  const save = async (overwriteRevision?: number) => {
    setAttempted(true)
    if (!local.valid) return
    setBusy('save')
    try {
      const result = await api.replaceActivity(draft.id, draft, overwriteRevision ?? draft.revision)
      const next = toDraft(result)
      setSaved(next)
      setDraft(next)
      setServerIssues(null)
      setAttempted(false)
      setConflict(null)
      setQueryData(`activity/${result.id}`, result)
      invalidate('activities', 'status')
      toast('Saved', { variant: 'success' })
    } catch (e) {
      if (e instanceof ApiError && e.code === 'revision_conflict') {
        // The 409 only carries current_revision; fetch the hub's copy to show what changed.
        try {
          setConflict(await api.activity(draft.id))
        } catch {
          toast('Changed somewhere else', { variant: 'error', description: 'Reload to get the latest.' })
        }
      } else if (e instanceof ApiError && e.details.length) {
        setServerIssues(e.details)
        toast('Couldn’t save', { variant: 'error', description: `${plural(e.details.length, 'problem')} to fix` })
      } else {
        toastError('Couldn’t save', e)
      }
    } finally {
      setBusy(null)
    }
  }

  const dryRun = async () => {
    setAttempted(true)
    setBusy('validate')
    try {
      const r = await api.validateActivity(draft)
      setServerIssues(r.errors)
      toast(r.valid ? 'No problems found' : `${plural(r.errors.length, 'problem')} found`, { variant: r.valid ? 'success' : 'error' })
    } catch (e) {
      toastError('Couldn’t check', e)
    } finally {
      setBusy(null)
    }
  }

  // Cmd/Ctrl+S saves. The listener is attached once and reads the latest state through a ref.
  const onSaveKey = useRef<() => void>(() => {})
  onSaveKey.current = () => {
    if (dirty && !busy && !lock.locked) void save()
  }
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

  const jumpTo = (issue: ValidationIssue) => {
    const b = buttonForPath(issue.path)
    if (b) {
      setTab('buttons')
      setSelected(b)
    } else if (issue.path.startsWith('/startup')) setTab('startup')
    else if (issue.path.startsWith('/shutdown')) setTab('shutdown')
  }

  const nameError = errors.find((e) => e.path === '/name')

  return (
    <ShowErrors.Provider value={showErrors}>
      <PageHeader
        back={{ href: '/', label: 'Activities' }}
        title={
          <span className="flex items-center gap-2">
            <label htmlFor="act-name" className="sr-only">
              Activity name
            </label>
            <Input
              id="act-name"
              value={draft.name}
              placeholder="Activity name"
              maxLength={LIMITS.nameMax}
              onChange={(e) => update({ name: e.target.value })}
              aria-invalid={!!nameError}
              className="h-10 w-[min(22rem,70vw)] border-transparent bg-transparent px-2 text-2xl font-semibold shadow-none placeholder:text-base placeholder:font-normal hover:border-input md:text-2xl"
            />
            <Pencil className="size-4 shrink-0 text-muted-foreground" aria-hidden />
          </span>
        }
        description={
          <span className="flex flex-wrap items-center gap-2 px-2">
            {dirty ? (
              <Badge variant="warning">Unsaved changes</Badge>
            ) : (
              <span className="inline-flex items-center gap-1 text-success">
                <CheckCircle2 className="size-3.5" /> Saved
              </span>
            )}
            {nameError && <span className="text-destructive">{nameError.message}</span>}
          </span>
        }
        actions={
          <>
            <Button variant="outline" onClick={dryRun} disabled={!!busy}>
              {busy === 'validate' ? <Loader2 className="animate-spin" /> : <ShieldCheck />}
              Check
            </Button>
            <Button onClick={() => save()} disabled={!!busy || !dirty || lock.locked} title={lock.title}>
              {busy === 'save' ? <Loader2 className="animate-spin" /> : <Save />}
              Save
              <Kbd className="hidden border-primary-foreground/30 bg-transparent text-primary-foreground/70 lg:inline">⌘S</Kbd>
            </Button>
          </>
        }
      />

      {errors.length > 0 && (
        <Alert variant="error" title={`${plural(errors.length, 'problem')} to fix`} className="mb-4">
          <ul className="grid gap-0.5">
            {errors.slice(0, 6).map((e, i) => (
              <li key={i}>
                <button className="text-left underline-offset-2 hover:underline" onClick={() => jumpTo(e)}>
                  {e.message}
                </button>
              </li>
            ))}
          </ul>
        </Alert>
      )}

      <Tabs
        value={tab}
        onChange={setTab}
        tabs={[
          { id: 'buttons', label: <TabLabel text="Buttons" count={boundButtons(draft.bindings)} errors={countFor('/bindings')} /> },
          { id: 'startup', label: <TabLabel text="Startup" count={draft.startup.length} errors={countFor('/startup')} /> },
          { id: 'shutdown', label: <TabLabel text="Shutdown" count={draft.shutdown.length} errors={countFor('/shutdown')} /> },
        ]}
      >
        {tab === 'buttons' && (
          <BindingsEditor
            bindings={draft.bindings}
            onChange={(bindings) => update({ bindings })}
            catalog={catalog}
            errors={errors}
            warnings={local.warnings}
            selected={selected}
            onSelect={setSelected}
            fallback={globalBindings}
          />
        )}
        {tab === 'startup' && (
          <Card>
            <CardHeader>
              <div>
                <CardTitle>Startup</CardTitle>
                <CardDescription>Runs when the activity starts.</CardDescription>
              </div>
            </CardHeader>
            <CardContent>
              <SequenceEditor label="Startup" actions={draft.startup} onChange={(startup) => update({ startup })} catalog={catalog} issues={errors} pathPrefix="/startup" />
            </CardContent>
          </Card>
        )}
        {tab === 'shutdown' && (
          <Card>
            <CardHeader>
              <div>
                <CardTitle>Shutdown</CardTitle>
                <CardDescription>Runs when you turn it off.</CardDescription>
              </div>
            </CardHeader>
            <CardContent>
              <SequenceEditor label="Shutdown" actions={draft.shutdown} onChange={(shutdown) => update({ shutdown })} catalog={catalog} issues={errors} pathPrefix="/shutdown" />
            </CardContent>
          </Card>
        )}
      </Tabs>

      <ConflictDialog
        mine={draft}
        theirs={conflict}
        busy={busy === 'save'}
        onClose={() => setConflict(null)}
        onLoadTheirs={() => {
          if (!conflict) return
          const t = toDraft(conflict)
          setSaved(t)
          setDraft(t)
          setConflict(null)
          setServerIssues(null)
          setQueryData(`activity/${conflict.id}`, conflict)
          toast('Loaded the latest version')
        }}
        onOverwrite={() => conflict && save(conflict.revision)}
      />
    </ShowErrors.Provider>
  )
}

function TabLabel({ text, count, errors }: { text: string; count: number; errors: number }) {
  return (
    <>
      {text}
      <span className={cn('rounded px-1.5 text-[11px] tabular-nums', errors ? 'bg-destructive text-destructive-foreground' : 'bg-muted-foreground/15')}>{errors ? `${errors}!` : count}</span>
    </>
  )
}

function ConflictDialog({ mine, theirs, busy, onClose, onLoadTheirs, onOverwrite }: { mine: ActivityDraft; theirs: Activity | null; busy: boolean; onClose: () => void; onLoadTheirs: () => void; onOverwrite: () => void }) {
  const diff = useMemo(() => {
    if (!theirs) return []
    const out: string[] = []
    if (mine.name !== theirs.name) out.push(`Name: “${theirs.name}” saved, “${mine.name}” here`)
    if (!same(mine.startup, theirs.startup)) out.push(`Startup: ${plural(theirs.startup.length, 'step')} saved, ${mine.startup.length} here`)
    if (!same(mine.shutdown, theirs.shutdown)) out.push(`Shutdown: ${plural(theirs.shutdown.length, 'step')} saved, ${mine.shutdown.length} here`)
    const keys = new Set([...Object.keys(mine.bindings), ...Object.keys(theirs.bindings)])
    const changed = new Set([...keys].filter((k) => !same(mine.bindings[k], theirs.bindings[k])).map((k) => parseTrigger(k)?.button ?? k))
    if (changed.size) out.push(`Buttons: ${plural(changed.size, 'button')} ${changed.size === 1 ? 'differs' : 'differ'}`)
    return out
  }, [mine, theirs])

  return (
    <Dialog
      open={!!theirs}
      onClose={onClose}
      title="Changed somewhere else"
      description="This activity was saved in another window while you were editing."
      footer={
        <>
          <Button variant="ghost" onClick={onClose}>
            Keep editing
          </Button>
          <Button variant="outline" onClick={onLoadTheirs}>
            Load theirs
          </Button>
          <Button variant="destructive" onClick={onOverwrite} disabled={busy}>
            {busy && <Loader2 className="animate-spin" />}
            Save mine
          </Button>
        </>
      }
    >
      {diff.length ? (
        <ul className="grid list-disc gap-1 pl-5 text-sm">
          {diff.map((d) => (
            <li key={d}>{d}</li>
          ))}
        </ul>
      ) : (
        <p className="text-sm text-muted-foreground">No differences.</p>
      )}
    </Dialog>
  )
}
