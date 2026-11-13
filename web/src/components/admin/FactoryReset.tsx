import { Eraser, Loader2 } from 'lucide-react'
import { useEffect, useState } from 'react'
import { api, errorMessage } from '@/api/client'
import { Section, TypedWordField } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { followRestart, NOT_BACK } from '@/lib/restart'
import { typedMatches } from '@/lib/utils'

/** The word typed to confirm a factory reset (not "reset": that's the configuration reset). */
const ERASE_WORD = 'erase'

/**
 * Erases everything (POST /admin/factory-reset), then waits for the hub to come back fresh and
 * reloads into the setup wizard (a 401 elsewhere already sends the app to its session check, which
 * lands on setup too).
 */
export function FactoryReset() {
  const [confirm, setConfirm] = useState(false)
  const [typed, setTyped] = useState('')
  const [state, setState] = useState<'idle' | 'sending' | 'erasing'>('idle')
  const [error, setError] = useState<unknown>(null)
  // Erasing: wait for the hub to come back fresh, then reload into setup. Stops on unmount.
  const [slow, setSlow] = useState(false)
  useEffect(() => (state === 'erasing' ? followRestart((s) => s.auth_state === 'fresh', () => setSlow(true)) : undefined), [state])

  const close = () => {
    if (state !== 'idle') return
    setConfirm(false)
    setTyped('')
    setError(null)
  }

  const erase = async () => {
    setState('sending')
    setError(null)
    try {
      await api.factoryReset()
    } catch (e) {
      setState('idle')
      return setError(e)
    }
    setState('erasing')
  }

  return (
    <Section
      title="Factory reset"
      description="Erases everything and starts setup again."
    >
      <Button variant="outline" className="text-destructive hover:text-destructive" onClick={() => setConfirm(true)}>
        <Eraser /> Factory reset…
      </Button>
      <Dialog
        open={confirm}
        onClose={close}
        title="Erase everything?"
        description="Everything on Hubbo will be erased, including your login."
        footer={
          state === 'erasing' ? undefined : (
            <>
              <Button variant="ghost" onClick={close} disabled={state !== 'idle'}>
                Cancel
              </Button>
              <Button variant="destructive" onClick={erase} disabled={state !== 'idle' || !typedMatches(typed, ERASE_WORD)}>
                {state === 'sending' && <Loader2 className="animate-spin" />} Erase everything
              </Button>
            </>
          )
        }
      >
        {state === 'erasing' ? (
          <p className="inline-flex items-center gap-2 text-sm" role="status">
            <Loader2 className="size-4 animate-spin text-primary" /> {slow ? NOT_BACK : 'Erasing… Setup opens when Hubbo restarts.'}
          </p>
        ) : (
          <div className="grid gap-4">
            {error != null && (
              <Alert variant="error" title="Nothing was erased">
                <p>{errorMessage(error)}</p>
              </Alert>
            )}
            <TypedWordField word={ERASE_WORD} id="erase-confirm" value={typed} onChange={setTyped} />
          </div>
        )}
      </Dialog>
    </Section>
  )
}
