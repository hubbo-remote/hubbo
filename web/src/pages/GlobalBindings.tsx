import { Loader2, Save } from 'lucide-react'
import { useMemo, useState } from 'react'
import { api, ApiError } from '@/api/client'
import { invalidate, setQueryData } from '@/api/hooks'
import type { GlobalBindings as GB, ValidationIssue } from '@/api/types'
import { BindingsEditor } from '@/components/bindings/BindingsEditor'
import { ErrorState, PageHeader } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { ShowErrors } from '@/components/ui/input'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Skeleton } from '@/components/ui/misc'
import { toast, toastError } from '@/components/ui/toast'
import { useCatalog, useConfigLock, useGlobalBindings } from '@/hooks/data'
import { validateBindings, type Catalog } from '@/lib/activity'
import { same } from '@/lib/utils'

/** Bindings that apply in every activity (and with none active): activity buttons, Off, lights. */
export default function GlobalBindings() {
  const g = useGlobalBindings()
  const { catalog, loading, error } = useCatalog()
  if (g.error || error) return <ErrorState error={g.error ?? error} retry={g.refetch} />
  if (!g.data || loading) return <Skeleton className="h-[32rem]" />
  return <Editor key={g.data.revision} initial={g.data} catalog={catalog} />
}

function Editor({ initial, catalog }: { initial: GB; catalog: Catalog }) {
  const [saved, setSaved] = useState(initial)
  const [bindings, setBindings] = useState(initial.bindings)
  const [selected, setSelected] = useState<string | null>('tv')
  const [serverIssues, setServerIssues] = useState<ValidationIssue[] | null>(null)
  const [busy, setBusy] = useState(false)
  const [stale, setStale] = useState(false)
  const lock = useConfigLock()
  const dirty = useMemo(() => !same(bindings, saved.bindings), [bindings, saved.bindings])

  const local = useMemo(() => {
    const errors: ValidationIssue[] = []
    const warnings: ValidationIssue[] = []
    validateBindings(bindings, catalog, errors, warnings)
    return { errors, warnings }
  }, [bindings, catalog])
  // Errors show from the first Save on, then update live; never before.
  const [attempted, setAttempted] = useState(false)
  const showErrors = attempted || serverIssues !== null
  const errors = showErrors ? (serverIssues ?? local.errors) : []

  const save = async () => {
    setAttempted(true)
    if (local.errors.length) return
    setBusy(true)
    try {
      const r = await api.replaceGlobalBindings({ bindings, revision: saved.revision })
      setSaved(r)
      setBindings(r.bindings)
      setServerIssues(null)
      setAttempted(false)
      setQueryData('global-bindings', r)
      toast('Saved', { variant: 'success' })
    } catch (e) {
      if (e instanceof ApiError && e.code === 'revision_conflict') setStale(true)
      else if (e instanceof ApiError && e.details.length) setServerIssues(e.details)
      else toastError('Couldn’t save', e)
    } finally {
      setBusy(false)
    }
  }

  return (
    <ShowErrors.Provider value={showErrors}>
      <PageHeader
        back={{ href: '/', label: 'Activities' }}
        title="Global buttons"
        description={
          <span className="flex flex-wrap items-center gap-2">
            Work in every activity.
            {dirty && <Badge variant="warning">Unsaved changes</Badge>}
          </span>
        }
        actions={
          <Button onClick={save} disabled={busy || !dirty || lock.locked} title={lock.title}>
            {busy ? <Loader2 className="animate-spin" /> : <Save />} Save
          </Button>
        }
      />
      {stale && (
        <Alert
          variant="warning"
          title="Changed somewhere else"
          className="mb-4"
          action={
            <Button
              size="sm"
              variant="outline"
              onClick={() => {
                setStale(false)
                invalidate('global-bindings')
              }}
            >
              Reload
            </Button>
          }
        >
          Reload to get the latest. Your changes here will be lost.
        </Alert>
      )}
      {errors.length > 0 && (
        <Alert variant="error" title="Fix this to save" className="mb-4">
          {errors[0]!.message}
        </Alert>
      )}
      <BindingsEditor
        bindings={bindings}
        onChange={(b) => {
          setBindings(b)
          setServerIssues(null)
        }}
        catalog={catalog}
        errors={errors}
        warnings={local.warnings}
        selected={selected}
        onSelect={setSelected}
      />
    </ShowErrors.Provider>
  )
}
