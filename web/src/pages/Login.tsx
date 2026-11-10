import { Loader2 } from 'lucide-react'
import { useState, type FormEvent } from 'react'
import { api, ApiError, errorMessage } from '@/api/client'
import { Logo } from '@/components/Logo'
import { ThemeToggle } from '@/components/AppShell'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Field, Input } from '@/components/ui/input'

export function Login({ onSignedIn }: { onSignedIn: () => void }) {
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setBusy(true)
    setError(null)
    try {
      await api.login(username, password)
      setPassword('')
      onSignedIn()
    } catch (err) {
      // The hub can't read its login data: re-check the session, which shows the right screen.
      if (err instanceof ApiError && err.code === 'storage_unavailable') return onSignedIn()
      setError(errorMessage(err))
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="grid min-h-dvh place-items-center bg-muted/40 p-4">
      <div className="fixed top-4 right-4">
        <ThemeToggle />
      </div>
      <form onSubmit={submit} className="grid w-full max-w-sm gap-5 rounded-2xl border bg-card p-7 shadow-sm">
        <div className="grid justify-items-center gap-2 text-center">
          <Logo className="size-10" />
          <h1 className="text-xl font-semibold">Sign in to Hubbo</h1>
        </div>
        {error && <Alert variant="error">{error}</Alert>}
        <Field label="Username" htmlFor="u">
          <Input id="u" autoComplete="username" autoCapitalize="none" autoCorrect="off" spellCheck={false} value={username} onChange={(e) => setUsername(e.target.value)} required autoFocus />
        </Field>
        <Field label="Password" htmlFor="p">
          <Input id="p" type="password" autoComplete="current-password" value={password} onChange={(e) => setPassword(e.target.value)} required />
        </Field>
        <Button type="submit" disabled={busy} className="w-full">
          {busy && <Loader2 className="animate-spin" />}
          Sign in
        </Button>
      </form>
    </div>
  )
}
