import { Loader2 } from 'lucide-react'
import { useMemo } from 'react'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import type { Doc } from '@/hooks/draft'
import { parseTrigger } from '@/lib/activity'
import { plural, same } from '@/lib/utils'

/** What differs between my draft and the copy someone else saved, one line per part. */
function conflictDiff(mine: Doc, theirs: Doc): string[] {
  const out: string[] = []
  if ('name' in mine && 'name' in theirs) {
    if (mine.name !== theirs.name) out.push(`Name: “${theirs.name}” saved, “${mine.name}” here`)
    if (!same(mine.startup, theirs.startup)) out.push(`Startup: ${plural(theirs.startup.length, 'step')} saved, ${mine.startup.length} here`)
    if (!same(mine.shutdown, theirs.shutdown)) out.push(`Shutdown: ${plural(theirs.shutdown.length, 'step')} saved, ${mine.shutdown.length} here`)
  }
  const keys = new Set([...Object.keys(mine.bindings), ...Object.keys(theirs.bindings)])
  const changed = new Set([...keys].filter((k) => !same(mine.bindings[k], theirs.bindings[k])).map((k) => parseTrigger(k)?.button ?? k))
  if (changed.size) out.push(`Buttons: ${plural(changed.size, 'button')} ${changed.size === 1 ? 'differs' : 'differ'}`)
  return out
}

/** Someone else saved first: keep editing, load their copy, or save mine over it. Takes `useDraft().conflict`. */
export function ConflictDialog({ mine, theirs, busy, close, loadTheirs, overwrite }: { mine?: Doc; theirs: Doc | null; busy: boolean; close: () => void; loadTheirs: () => void; overwrite: () => void }) {
  const diff = useMemo(() => (mine && theirs ? conflictDiff(mine, theirs) : []), [mine, theirs])
  return (
    <Dialog
      open={!!theirs}
      onClose={close}
      title="Changed somewhere else"
      description="Saved in another window while you were editing."
      footer={
        <>
          <Button variant="ghost" onClick={close}>
            Keep editing
          </Button>
          <Button variant="outline" onClick={loadTheirs}>
            Load theirs
          </Button>
          <Button variant="destructive" onClick={overwrite} disabled={busy}>
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
