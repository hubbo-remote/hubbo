import { Loader2 } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'

/** Someone else saved first: keep editing, load their copy, or save mine over it. Takes `useDraft().conflict`. */
export function ConflictDialog({ open, diff, busy, close, loadTheirs, overwrite }: { open: boolean; diff: string[]; busy: boolean; close: () => void; loadTheirs: () => void; overwrite: () => void }) {
  return (
    <Dialog
      open={open}
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
