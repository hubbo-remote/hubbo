import { Check, ChevronLeft, ChevronRight, Loader2 } from 'lucide-react'
import { useMemo, useState, type ReactNode } from 'react'
import { Link, useLocation } from 'wouter'
import { api, errorMessage } from '@/api/client'
import { invalidate, setQueryData } from '@/api/hooks'
import type { Action } from '@/api/types'
import { commandLabel, deviceName, paramSummary } from '@/components/actions/describe'
import { DeviceModel, ErrorState, PageHeader } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Field, Input, Select } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { Switch } from '@/components/ui/switch'
import { useCatalog, useConfigLock, useDeviceDetails } from '@/hooks/data'
import type { Catalog } from '@/lib/activity'
import { buildStarter, matchButton, powerOf, wizardDevice, type Starter, type WizardAnswers, type WizardDevice } from '@/lib/wizard/build'
import { BUTTON_RULES } from '@/lib/wizard/rules'
import { cn } from '@/lib/utils'

type StepId = 'start' | 'watch' | 'volume' | 'inputs' | 'power' | 'review'
const TITLES: Record<StepId, string> = { start: 'Devices', watch: 'Watch', volume: 'Volume', inputs: 'Inputs', power: 'Power', review: 'Review' }

/** A checkbox or radio row. */
const OPTION_ROW = 'flex items-center gap-3 rounded-lg border px-3 py-2.5 text-sm has-[:checked]:border-primary has-[:checked]:bg-primary/5'

const hasRole = (d: WizardDevice, role: 'watch' | 'volume') => BUTTON_RULES.some((r) => r.role === role && matchButton(d, r))

/**
 * The new-activity wizard: devices → what you watch on → what controls volume → inputs → power →
 * review. The answers only build a starter activity; saving creates an ordinary one and opens it
 * in the editor. Steps with nothing to ask are skipped.
 */
export default function ActivityWizard() {
  const { catalog, loading, error } = useCatalog()
  const [, navigate] = useLocation()
  const [answers, setAnswers] = useState<WizardAnswers>({ name: '', devices: [], watch: null, volume: null, inputs: {}, power: {} })
  const [at, setAt] = useState<StepId>('start')
  const [saving, setSaving] = useState(false)
  const [saveError, setSaveError] = useState<string | null>(null)
  const lock = useConfigLock()
  const set = (patch: Partial<WizardAnswers>) => setAnswers((a) => ({ ...a, ...patch }))

  // Profile devices (Bluetooth, infrared) need the detail form (their profile's commands, power and
  // inputs); they load once when the wizard opens, alongside the capabilities. Ticking a device never fetches.
  const profileIds = [...catalog.devices.values()].filter((d) => d.profile).map((d) => d.id).sort()
  const details = useDeviceDetails(profileIds)
  const waiting = loading || (profileIds.length > 0 && !details.data)
  const usable = useMemo(
    () => new Map([...catalog.devices.values()].map((d) => [d.id, wizardDevice(d, details.data?.get(d.id), catalog.capabilities.get(d.id))])),
    [details.data, catalog],
  )
  const devices = useMemo(() => answers.devices.flatMap((id) => usable.get(id) ?? []), [answers.devices, usable])

  const watchers = devices.filter((d) => hasRole(d, 'watch'))
  const volumes = devices.filter((d) => hasRole(d, 'volume'))
  const withInputs = devices.filter((d) => d.inputs.length > 0)
  const powered = devices.filter((d) => d.power.type !== 'none')
  const asked: Partial<Record<StepId, WizardDevice[]>> = { watch: watchers, volume: volumes, inputs: withInputs, power: powered }
  const steps = (Object.keys(TITLES) as StepId[]).filter((s) => asked[s]?.length !== 0)
  const index = Math.max(0, steps.indexOf(at))
  const go = (i: number) => setAt(steps[Math.min(Math.max(i, 0), steps.length - 1)]!)
  const starter = useMemo(() => buildStarter(devices, answers, BUTTON_RULES), [devices, answers])
  const ready = answers.name.trim() !== '' && devices.length > 0
  const step = steps[index]!

  const save = async () => {
    setSaving(true)
    setSaveError(null)
    try {
      const a = await api.createActivity(starter.activity)
      setQueryData(`activity/${a.id}`, a)
      invalidate('activities', 'status')
      navigate(`/activities/${a.id}`)
    } catch (e) {
      setSaveError(errorMessage(e, { details: true }))
      setSaving(false)
    }
  }

  if (error) return <ErrorState error={error} retry={() => invalidate('devices', 'caps', 'integrations', 'activities')} />
  if (details.error) return <ErrorState error={details.error} retry={details.refetch} />
  return (
    <div className="mx-auto grid max-w-2xl gap-6">
      <PageHeader title="New activity" back={{ href: '/', label: 'Activities' }} />

      <ol className="flex gap-1.5" aria-label="Progress">
        {steps.map((s, i) => (
          <li key={s} className="flex-1" aria-current={i === index ? 'step' : undefined}>
            <button type="button" className="block w-full text-left" disabled={i >= index} onClick={() => go(i)}>
              <span className={cn('block h-1.5 rounded-full bg-border transition-colors', i <= index && 'bg-primary')} />
              <span className={cn('mt-1.5 hidden text-xs sm:block', i === index ? 'font-medium text-foreground' : 'text-muted-foreground')}>
                {i < index && <Check className="mr-1 inline size-3 text-success" />}
                {TITLES[s]}
              </span>
            </button>
          </li>
        ))}
      </ol>

      <section className="rounded-2xl border bg-card p-5 shadow-sm sm:p-7" aria-labelledby="step-title">
        {waiting ? (
          <Skeleton className="h-60" />
        ) : (
          {
            start: <StartStep answers={answers} set={set} catalog={catalog} />,
            watch: (
              <Choose title="What are you watching with?" blurb="Arrows, OK, play/pause, numbers and channels go to this device." options={watchers} value={answers.watch} onChange={(watch) => set({ watch })} />
            ),
            volume: <Choose title="What controls volume?" blurb="Volume up, down and mute go to this device." options={volumes} value={answers.volume} onChange={(volume) => set({ volume })} />,
            inputs: <InputsStep devices={withInputs} answers={answers} set={set} />,
            power: <PowerStep devices={powered} answers={answers} set={set} />,
            review: <Review devices={devices} answers={answers} starter={starter} catalog={catalog} visible={steps} edit={setAt} />,
          }[step]
        )}
        {saveError && <Alert variant="error" className="mt-4" title="Couldn’t create the activity">{saveError}</Alert>}
      </section>

      <div className="flex items-center justify-between gap-2">
        {index > 0 && (
          <Button variant="ghost" onClick={() => go(index - 1)}>
            <ChevronLeft /> Back
          </Button>
        )}
        {step === 'review' ? (
          <Button className="ml-auto" onClick={save} disabled={saving || lock.locked} title={lock.title}>
            {saving && <Loader2 className="animate-spin" />} Create activity
          </Button>
        ) : (
          <Button className="ml-auto" onClick={() => go(index + 1)} disabled={!ready}>
            Continue <ChevronRight />
          </Button>
        )}
      </div>
    </div>
  )
}

function Heading({ title, blurb }: { title: string; blurb?: string }) {
  return (
    <h2 id="step-title" className="mb-5 text-lg font-semibold">
      {title}
      {blurb && <span className="mt-0.5 block text-sm font-normal text-muted-foreground">{blurb}</span>}
    </h2>
  )
}

function StartStep({ answers, set, catalog }: { answers: WizardAnswers; set: (p: Partial<WizardAnswers>) => void; catalog: Catalog }) {
  const all = [...catalog.devices.values()]
  const toggle = (id: string) => {
    const devices = answers.devices.includes(id) ? answers.devices.filter((x) => x !== id) : [...answers.devices, id]
    // A role given to a device that's no longer picked is dropped (its input and power are ignored).
    const keep = (role: string | null) => (role && devices.includes(role) ? role : null)
    set({ devices, watch: keep(answers.watch), volume: keep(answers.volume) })
  }
  return (
    <div className="grid gap-5">
      <Field label="Name" htmlFor="wiz-name">
        <Input id="wiz-name" value={answers.name} maxLength={64} placeholder="Watch TV" autoFocus onChange={(e) => set({ name: e.target.value })} />
      </Field>
      <div className="grid gap-2" role="group" aria-labelledby="step-title">
        <h2 id="step-title" className="mb-1 text-base font-semibold">
          Included Devices
        </h2>
        {all.length === 0 && <p className="text-sm text-muted-foreground">No devices yet. <Link href="/devices" className="underline">Add one</Link> first.</p>}
        {all.map((d) => (
          <label key={d.id} className={OPTION_ROW}>
            <input type="checkbox" className="size-4 accent-[var(--color-primary)]" checked={answers.devices.includes(d.id)} onChange={() => toggle(d.id)} />
            <span className="min-w-0">
              <span className="block font-medium">{d.name}</span>
              <DeviceModel device={d} /> {/* not an HA entity id: noise here */}
            </span>
          </label>
        ))}
      </div>
    </div>
  )
}

function Choose({ title, blurb, options, value, onChange }: { title: string; blurb: string; options: WizardDevice[]; value: string | null; onChange: (id: string | null) => void }) {
  const choices: [string | null, string][] = [...options.map((d): [string, string] => [d.id, d.name]), [null, 'None of these']]
  return (
    <div>
      <Heading title={title} blurb={blurb} />
      <div className="grid gap-2" role="radiogroup" aria-labelledby="step-title">
        {choices.map(([id, name]) => (
          <label key={id ?? 'none'} className={OPTION_ROW}>
            <input type="radio" name={title} className="size-4 accent-[var(--color-primary)]" checked={value === id} onChange={() => onChange(id)} />
            <span className={cn(id ? 'font-medium' : 'text-muted-foreground')}>{name}</span>
          </label>
        ))}
      </div>
    </div>
  )
}

function InputsStep({ devices, answers, set }: { devices: WizardDevice[]; answers: WizardAnswers; set: (p: Partial<WizardAnswers>) => void }) {
  return (
    <div className="grid gap-5">
      <Heading title="Inputs" blurb="The input each device switches to at start." />
      {devices.map((d) => (
        <Field key={d.id} label={d.name} htmlFor={`in-${d.id}`}>
          <Select id={`in-${d.id}`} value={answers.inputs[d.id] ?? ''} onChange={(e) => set({ inputs: { ...answers.inputs, [d.id]: e.target.value || null } })}>
            <option value="">Don’t switch</option>
            {d.inputs.map((i) => (
              <option key={i.name}>{i.name}</option>
            ))}
          </Select>
        </Field>
      ))}
    </div>
  )
}

function PowerStep({ devices, answers, set }: { devices: WizardDevice[]; answers: WizardAnswers; set: (p: Partial<WizardAnswers>) => void }) {
  const change = (id: string, k: 'on' | 'off', v: boolean) => set({ power: { ...answers.power, [id]: { ...powerOf(answers, id), [k]: v } } })
  return (
    <div className="grid gap-4">
      <Heading title="Power" />
      {devices.map((d) => {
        const p = powerOf(answers, d.id)
        return (
          <div key={d.id} className="grid gap-2 rounded-lg border px-4 py-3 text-sm">
            <p className="font-medium">{d.name}</p>
            {d.power.type === 'toggle' ? (
              <p className="text-muted-foreground">Toggle only, not added</p>
            ) : (
              (['on', 'off'] as const).map((k) => (
                <label key={k} className="flex items-center justify-between gap-3">
                  {k === 'on' ? 'Turn on at start' : 'Turn off at end'}
                  <Switch checked={p[k]} onCheckedChange={(v) => change(d.id, k, v)} aria-label={`${k === 'on' ? 'Turn on' : 'Turn off'} ${d.name}`} />
                </label>
              ))
            )}
          </div>
        )
      })}
    </div>
  )
}

function Review({ devices, answers, starter, catalog, visible, edit }: { devices: WizardDevice[]; answers: WizardAnswers; starter: Starter; catalog: Catalog; visible: StepId[]; edit: (s: StepId) => void }) {
  // "Change" goes to the first of these steps the wizard showed; none shown, no link.
  const changeAt = (...want: StepId[]) => {
    const s = want.find((x) => visible.includes(x))
    return s ? () => edit(s) : undefined
  }
  const { activity } = starter
  const nameOf = (id: string | null) => devices.find((d) => d.id === id)?.name ?? 'None'
  const switching = devices.filter((d) => answers.inputs[d.id]).map((d) => `${d.name} → ${answers.inputs[d.id]}`)
  const toggleOnly = devices.filter((d) => d.power.type === 'toggle')
  const rows = [
    { label: 'Watching with', value: nameOf(answers.watch), onEdit: changeAt('watch') },
    { label: 'Volume', value: nameOf(answers.volume), onEdit: changeAt('volume') },
  ]
  if (visible.includes('inputs')) rows.push({ label: 'Inputs', value: switching.join(', ') || 'None', onEdit: changeAt('inputs') })
  const stepList = (title: string, list: Action[]) =>
    list.length === 0 ? (
      <p className="text-muted-foreground">Nothing</p>
    ) : (
      <ol className="grid gap-1" aria-label={`${title} steps`}>
        {list.map((a, i) => {
          const params = paramSummary(a, catalog)
          return (
            <li key={i} className="flex gap-2">
              <span className="w-5 shrink-0 text-right text-muted-foreground tabular-nums">{i + 1}.</span>
              <span>
                <span className="text-muted-foreground">{deviceName(a, catalog)}:</span> {commandLabel(a, catalog)}
                {params && <span className="text-muted-foreground"> · {params}</span>}
              </span>
            </li>
          )
        })}
      </ol>
    )
  return (
    <div className="grid gap-6 text-sm">
      <Heading title={activity.name || 'Review'} blurb="You can change anything after it’s created." />
      <ul className="grid gap-2" aria-label="Answers">
        {rows.map((r) => (
          <li key={r.label} className="flex items-baseline gap-3">
            <span className="w-28 shrink-0 text-muted-foreground">{r.label}</span>
            <span className="min-w-0 flex-1 font-medium">{r.value}</span>
            {r.onEdit && <ChangeLink label={r.label} onClick={r.onEdit} />}
          </li>
        ))}
      </ul>
      <ReviewSection title="Start" onEdit={changeAt('inputs', 'power')}>
        {stepList('Start', activity.startup)}
        {toggleOnly.map((d) => (
          <p key={d.id} className="text-muted-foreground">
            {d.name}: toggle only, not added
          </p>
        ))}
      </ReviewSection>
      <ReviewSection title="End" onEdit={changeAt('power')}>
        {stepList('End', activity.shutdown)}
      </ReviewSection>
    </div>
  )
}

function ChangeLink({ label, onClick }: { label: string; onClick: () => void }) {
  return (
    <Button variant="link" size="sm" className="h-auto p-0" onClick={onClick} aria-label={`Change ${label.toLowerCase()}`}>
      Change
    </Button>
  )
}

function ReviewSection({ title, onEdit, children }: { title: string; onEdit?: () => void; children: ReactNode }) {
  return (
    <section className="grid gap-2">
      <div className="flex items-center justify-between">
        <h3 className="font-medium">{title}</h3>
        {onEdit && <ChangeLink label={title} onClick={onEdit} />}
      </div>
      {children}
    </section>
  )
}
