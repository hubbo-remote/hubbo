import { Activity, Gauge, LogOut, Monitor, Moon, Plug, Settings, Sun, Tv2, type LucideIcon } from 'lucide-react'
import type { ReactNode } from 'react'
import { Link, useLocation } from 'wouter'
import { api } from '@/api/client'
import type { Session } from '@/api/types'
import { Button } from '@/components/ui/button'
import { useActivities, useStatus } from '@/hooks/data'
import { useTheme, type ThemePref } from '@/lib/theme'
import { cn } from '@/lib/utils'
import { ConfigBanner } from './ConfigBanner'
import { Logo } from './Logo'

const NAV: { href: string; label: string; icon: LucideIcon }[] = [
  { href: '/', label: 'Dashboard', icon: Gauge },
  { href: '/activities', label: 'Activities', icon: Activity },
  { href: '/devices', label: 'Devices', icon: Tv2 },
  { href: '/integrations', label: 'Integrations', icon: Plug },
  { href: '/settings', label: 'Settings', icon: Settings },
]

function useActive() {
  const [loc] = useLocation()
  return (href: string) => (href === '/' ? loc === '/' : loc === href || loc.startsWith(href + '/'))
}

const THEMES: { id: ThemePref; icon: LucideIcon; label: string }[] = [
  { id: 'light', icon: Sun, label: 'Light' },
  { id: 'dark', icon: Moon, label: 'Dark' },
  { id: 'system', icon: Monitor, label: 'System' },
]

export function ThemeToggle({ className }: { className?: string }) {
  const [theme, setTheme] = useTheme()
  return (
    <div role="radiogroup" aria-label="Theme" className={cn('inline-flex rounded-lg bg-muted p-0.5', className)}>
      {THEMES.map((t) => (
        <button
          key={t.id}
          role="radio"
          aria-checked={theme === t.id}
          aria-label={t.label}
          title={t.label}
          onClick={() => setTheme(t.id)}
          className={cn('grid size-7 place-items-center rounded-md text-muted-foreground transition-colors outline-none focus-visible:ring-2 focus-visible:ring-ring cursor-pointer', theme === t.id && 'bg-card text-foreground shadow-sm')}
        >
          <t.icon className="size-3.5" />
        </button>
      ))}
    </div>
  )
}

function ActiveChip() {
  const status = useStatus()
  const acts = useActivities()
  const id = status.data?.active_activity_id
  const name = id ? (acts.data?.activities.find((a) => a.id === id)?.name ?? id) : null
  if (!status.data) return null
  return (
    <Link href="/" className="inline-flex min-w-0 items-center gap-2 rounded-full border bg-card px-3 py-1 text-xs font-medium shadow-xs hover:bg-accent">
      <span className={cn('size-2 shrink-0 rounded-full', name ? 'bg-success' : 'bg-muted-foreground/40')} aria-hidden />
      <span className="truncate">{name ?? 'Off'}</span>
    </Link>
  )
}

export function AppShell({ session, onSignedOut, children }: { session: Session; onSignedOut: () => void; children: ReactNode }) {
  const isActive = useActive()
  const status = useStatus()
  const sim = status.data?.auth_mode === 'simulator_no_auth' || session.auth_mode === 'simulator_no_auth'

  const logout = async () => {
    await api.logout().catch(() => undefined)
    onSignedOut()
  }

  return (
    <div className="min-h-dvh md:grid md:grid-cols-[15rem_minmax(0,1fr)]">
      <a href="#main" className="sr-only focus:not-sr-only focus:fixed focus:top-2 focus:left-2 focus:z-50 focus:rounded-md focus:bg-card focus:px-3 focus:py-2 focus:shadow">
        Skip to content
      </a>
      {/* Desktop sidebar */}
      <div className="hidden border-r bg-card/50 md:block">
      <aside className="sticky top-0 flex h-dvh flex-col">
        <div className="flex h-16 items-center gap-2 px-5">
          <Logo className="size-7" />
          <span className="text-lg font-semibold tracking-tight">Hubbo</span>
        </div>
        <nav aria-label="Main" className="grid gap-0.5 px-3">
          {NAV.map((n) => (
            <Link
              key={n.href}
              href={n.href}
              aria-current={isActive(n.href) ? 'page' : undefined}
              className={cn(
                'flex items-center gap-3 rounded-md px-3 py-2 text-sm font-medium text-muted-foreground transition-colors outline-none hover:bg-accent hover:text-accent-foreground focus-visible:ring-2 focus-visible:ring-ring',
                isActive(n.href) && 'bg-accent text-accent-foreground',
              )}
            >
              <n.icon className="size-4" aria-hidden />
              {n.label}
            </Link>
          ))}
        </nav>
        <div className="mt-auto grid gap-3 border-t p-4">
          <div className="flex items-center justify-between">
            <ThemeToggle />
            <Button variant="ghost" size="icon" onClick={logout} aria-label="Sign out" title="Sign out">
              <LogOut />
            </Button>
          </div>
          <p className="truncate text-xs text-muted-foreground">
            Signed in as <span className="font-medium text-foreground">{session.username}</span>
            {status.data && <> · v{status.data.hubbo.version}</>}
          </p>
        </div>
      </aside>
      </div>

      <div className="flex min-w-0 flex-col">
        {sim && (
          <div role="alert" className="bg-warning px-4 py-1.5 text-center text-xs font-bold tracking-wide text-black">
            AUTH DISABLED — SIMULATOR
          </div>
        )}
        <ConfigBanner admin={session.role === 'admin'} />
        {/* Mobile header */}
        <header className="sticky top-0 z-30 flex h-14 items-center gap-3 border-b bg-background/85 px-4 backdrop-blur md:hidden">
          <Logo className="size-6" />
          <span className="font-semibold">Hubbo</span>
          <div className="ml-auto flex min-w-0 items-center gap-2">
            <ActiveChip />
            <ThemeToggle />
          </div>
        </header>
        <div className="hidden h-16 items-center justify-end gap-3 px-8 md:flex">
          <ActiveChip />
        </div>
        <main id="main" className="mx-auto w-full max-w-6xl flex-1 px-4 pt-4 pb-28 sm:px-6 md:px-8 md:pt-0 md:pb-12">
          {children}
        </main>
      </div>

      {/* Mobile tab bar */}
      <nav aria-label="Main" className="fixed inset-x-0 bottom-0 z-30 grid grid-cols-5 border-t bg-background/95 pb-[env(safe-area-inset-bottom)] backdrop-blur md:hidden">
        {NAV.map((n) => (
          <Link
            key={n.href}
            href={n.href}
            aria-current={isActive(n.href) ? 'page' : undefined}
            className={cn('flex flex-col items-center gap-0.5 py-2 text-[11px] font-medium text-muted-foreground outline-none focus-visible:bg-accent', isActive(n.href) && 'text-primary')}
          >
            <n.icon className="size-5" aria-hidden />
            {n.label}
          </Link>
        ))}
      </nav>
    </div>
  )
}
