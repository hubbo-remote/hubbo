import { ArrowDown, ArrowUp, Clock, GripVertical, Pencil, Plus, Trash2 } from 'lucide-react'
import { useState } from 'react'
import type { Action, ValidationIssue } from '@/api/types'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { isWait, LIMITS, waitAction, type Catalog } from '@/lib/activity'
import { cn } from '@/lib/utils'
import { ActionPicker } from './ActionPicker'
import { commandLabel, deviceName, paramSummary } from './describe'

/** Ordered list of actions (startup/shutdown) with add, edit, reorder, wait. */
export function SequenceEditor({ label, actions, onChange, catalog, issues, pathPrefix }: { label: string; actions: Action[]; onChange: (a: Action[]) => void; catalog: Catalog; issues: ValidationIssue[]; pathPrefix: string }) {
  const [editing, setEditing] = useState<{ index: number; action: Action | null } | null>(null)
  const move = (i: number, d: number) => {
    const next = [...actions]
    const [x] = next.splice(i, 1)
    next.splice(i + d, 0, x!)
    onChange(next)
  }
  const issueFor = (i: number) => issues.filter((x) => x.path === `${pathPrefix}/${i}` || x.path.startsWith(`${pathPrefix}/${i}/`))
  const full = actions.length >= LIMITS.maxSequence

  return (
    <div className="grid gap-3">
      {actions.length === 0 ? (
        <p className="rounded-lg border border-dashed px-4 py-6 text-center text-sm text-muted-foreground">No steps. {label} does nothing.</p>
      ) : (
        <ol className="grid gap-2" aria-label={`${label} steps`}>
          {actions.map((a, i) => {
            const errs = issueFor(i)
            const wait = isWait(a)
            return (
              <li
                key={i}
                className={cn(
                  'group flex items-center gap-2 rounded-lg border bg-card py-2 pr-2 pl-1.5 text-sm',
                  wait && 'border-dashed bg-muted/40',
                  errs.length && 'border-destructive/60 bg-destructive/5',
                )}
              >
                <GripVertical className="size-4 shrink-0 text-muted-foreground/50" aria-hidden />
                <span className="w-5 shrink-0 text-right font-mono text-xs text-muted-foreground tabular-nums">{i + 1}</span>
                {wait ? <Clock className="size-4 shrink-0 text-muted-foreground" aria-hidden /> : null}
                <div className="min-w-0 flex-1">
                  <p className="truncate">
                    {wait ? (
                      <span className="text-muted-foreground">Wait {paramSummary(a)}</span>
                    ) : (
                      <>
                        <span className="font-medium">{deviceName(a, catalog)}</span>
                        <span className="text-muted-foreground"> · </span>
                        {commandLabel(a, catalog)}
                        {paramSummary(a, catalog) && <span className="text-muted-foreground"> ({paramSummary(a, catalog)})</span>}
                      </>
                    )}
                  </p>
                  {errs.map((e) => (
                    <p key={e.path + e.code} className="text-xs text-destructive">
                      {e.message}
                    </p>
                  ))}
                </div>
                <div className="flex shrink-0 items-center">
                  <Button variant="ghost" size="icon" className="size-8" aria-label={`Move step ${i + 1} up`} disabled={i === 0} onClick={() => move(i, -1)}>
                    <ArrowUp />
                  </Button>
                  <Button variant="ghost" size="icon" className="size-8" aria-label={`Move step ${i + 1} down`} disabled={i === actions.length - 1} onClick={() => move(i, 1)}>
                    <ArrowDown />
                  </Button>
                  <Button variant="ghost" size="icon" className="size-8" aria-label={`Edit step ${i + 1}`} onClick={() => setEditing({ index: i, action: a })}>
                    <Pencil />
                  </Button>
                  <Button variant="ghost" size="icon" className="size-8 hover:text-destructive" aria-label={`Remove step ${i + 1}`} onClick={() => onChange(actions.filter((_, j) => j !== i))}>
                    <Trash2 />
                  </Button>
                </div>
              </li>
            )
          })}
        </ol>
      )}
      <div className="flex flex-wrap gap-2">
        <Button variant="outline" size="sm" disabled={full} onClick={() => setEditing({ index: actions.length, action: null })}>
          <Plus /> Add step
        </Button>
        <Button variant="ghost" size="sm" disabled={full} onClick={() => onChange([...actions, waitAction()])}>
          <Clock /> Add wait
        </Button>
      </div>
      <Dialog
        open={!!editing}
        onClose={() => setEditing(null)}
        title={editing && editing.index < actions.length ? `Edit step ${editing.index + 1}` : `Add ${label.toLowerCase()} step`}
        footer={
          <>
            <Button variant="ghost" onClick={() => setEditing(null)}>
              Cancel
            </Button>
            <Button
              disabled={!editing?.action || !editing.action.command_id}
              onClick={() => {
                if (!editing?.action) return
                const next = [...actions]
                next[editing.index] = editing.action
                onChange(next)
                setEditing(null)
              }}
            >
              {editing && editing.index < actions.length ? 'Update step' : 'Add step'}
            </Button>
          </>
        }
      >
        {editing && <ActionPicker value={editing.action} onChange={(action) => setEditing({ ...editing, action })} catalog={catalog} context="sequence" />}
      </Dialog>
    </div>
  )
}
