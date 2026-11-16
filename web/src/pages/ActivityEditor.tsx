import { CheckCircle2, Loader2, Save } from 'lucide-react'
import { useMemo, useRef, useState } from 'react'
import type { Activity, ActivityDraft, ValidationIssue } from '@/api/types'
import { SequenceEditor } from '@/components/actions/SequenceEditor'
import { BindingsEditor } from '@/components/bindings/BindingsEditor'
import { smartOf } from '@/lib/smartButtons'
import { ErrorState, PageHeader } from '@/components/common'
import { ConflictDialog } from '@/components/ConflictDialog'
import { Alert } from '@/components/ui/alert'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card'
import { Input, ShowErrors } from '@/components/ui/input'
import { Kbd, Skeleton } from '@/components/ui/misc'
import { Tabs } from '@/components/ui/tabs'
import { retryFailed, useActivity, useActivityRuns, useCatalog, useGlobalBindings, useSmartButtons } from '@/hooks/data'
import { failuresFor } from '@/lib/runs'
import { activityStore, toDraft, useDraft } from '@/hooks/draft'
import { buttonForPath, LIMITS, parseTrigger, type Catalog } from '@/lib/activity'
import { cn, plural } from '@/lib/utils'

const boundButtons = (b: ActivityDraft['bindings']) => new Set(Object.keys(b).map((k) => parseTrigger(k)?.button)).size

export default function ActivityEditor({ id }: { id: string }) {
  const remote = useActivity(id)
  const cat = useCatalog()
  const globals = useGlobalBindings() // fetched alongside, not after, the catalog

  if (!remote.data || !globals.data || cat.loading) {
    const err = remote.error ?? globals.error ?? cat.error
    if (err) return <ErrorState error={err} retry={retryFailed(remote, globals, cat)} />
    return (
      <div className="grid gap-4">
        <Skeleton className="h-10 w-72" />
        <Skeleton className="h-[32rem]" />
      </div>
    )
  }
  return <Editor key={id} activity={remote.data} catalog={cat.catalog} globalBindings={globals.data.bindings} />
}

function Editor({ activity, catalog, globalBindings }: { activity: Activity; catalog: Catalog; globalBindings: ActivityDraft['bindings'] }) {
  const store = useMemo(() => activityStore(activity.id), [activity.id])
  const ed = useDraft(store, useMemo(() => toDraft(activity), [activity]), catalog)
  const draft = ed.draft! // the saved copy is in hand, so the draft is too
  const { update, errors } = ed
  const [selected, setSelected] = useState<string | null>(null)
  const [tab, setTab] = useState('buttons')
  const smartData = useSmartButtons().data
  // The newest run of this activity: its failing steps carry a warning (where the step is still the same).
  const lastRun = useActivityRuns().data?.runs.find((r) => r.activity_id === activity.id)
  const smart = useMemo(() => smartOf(smartData), [smartData])
  const countFor = (prefix: string) => errors.filter((e) => e.path.startsWith(prefix)).length

  const jumpTo = (issue: ValidationIssue) => {
    const b = buttonForPath(issue.path)
    if (b) {
      setTab('buttons')
      setSelected(b)
    } else if (issue.path.startsWith('/startup')) setTab('startup')
    else if (issue.path.startsWith('/shutdown')) setTab('shutdown')
  }

  const nameError = errors.find((e) => e.path === '/name')
  // The name edits in place: Enter or leaving keeps it (for Save), Esc puts back what it was.
  const nameBefore = useRef(draft.name)

  return (
    <ShowErrors.Provider value={ed.showErrors}>
      <PageHeader
        back={{ href: '/', label: 'Activities' }}
        title={
          <span className="flex items-center">
            <label htmlFor="act-name" className="sr-only">
              Activity name
            </label>
            <Input
              id="act-name"
              value={draft.name}
              placeholder="Activity name"
              maxLength={LIMITS.nameMax}
              onFocus={() => (nameBefore.current = draft.name)}
              onChange={(e) => update({ name: e.target.value })}
              onKeyDown={(e) => {
                if (e.key === 'Enter') e.currentTarget.blur()
                else if (e.key === 'Escape') {
                  e.preventDefault()
                  update({ name: nameBefore.current })
                  e.currentTarget.blur()
                }
              }}
              aria-invalid={!!nameError}
              className="h-10 w-[min(22rem,70vw)] cursor-text border-transparent bg-transparent px-2 text-2xl font-semibold shadow-none underline decoration-transparent decoration-dashed underline-offset-8 placeholder:text-base placeholder:font-normal hover:bg-accent/50 hover:decoration-muted-foreground/50 focus-visible:bg-card focus-visible:decoration-transparent md:text-2xl"
            />
          </span>
        }
        description={
          <span className="flex flex-wrap items-center gap-2 px-2">
            {ed.dirty ? (
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
          <Button onClick={() => ed.save()} disabled={!ed.canSave} title={ed.lock.title}>
            {ed.busy ? <Loader2 className="animate-spin" /> : <Save />}
            Save
            <Kbd className="hidden border-primary-foreground/30 bg-transparent text-primary-foreground/70 lg:inline">⌘S</Kbd>
          </Button>
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
            warnings={ed.warnings}
            selected={selected}
            onSelect={setSelected}
            set={{ source: 'activity', globals: globalBindings }}
            smart={smart}
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
              <SequenceEditor label="Startup" actions={draft.startup} onChange={(startup) => update({ startup })} catalog={catalog} issues={errors} pathPrefix="/startup" lastRun={failuresFor(lastRun, 'startup', draft.startup)} />
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
              <SequenceEditor label="Shutdown" actions={draft.shutdown} onChange={(shutdown) => update({ shutdown })} catalog={catalog} issues={errors} pathPrefix="/shutdown" lastRun={failuresFor(lastRun, 'shutdown', draft.shutdown)} />
            </CardContent>
          </Card>
        )}
      </Tabs>

      <ConflictDialog {...ed.conflict} busy={ed.busy} />
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

