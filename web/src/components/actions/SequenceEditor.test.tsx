// @vitest-environment jsdom
import { fireEvent, render, screen, within } from '@testing-library/react'
import { useState } from 'react'
import { beforeAll, describe, expect, it } from 'vitest'
import type { Action, Command, Device } from '@/api/types'
import { waitAction, type Catalog } from '@/lib/activity'
import { SequenceEditor } from './SequenceEditor'

// jsdom has no modal dialogs.
beforeAll(() => {
  HTMLDialogElement.prototype.showModal ??= function (this: HTMLDialogElement) {
    this.open = true
  }
  HTMLDialogElement.prototype.close ??= function (this: HTMLDialogElement) {
    this.open = false
  }
})

// A hand-made catalog: the mock's fixtures load from disk, which jsdom can't.
const cat: Catalog = {
  devices: new Map([['dev_lg_tv', { id: 'dev_lg_tv', name: 'LG TV', integration_id: 'ha_main' } as Device]]),
  capabilities: new Map([['dev_lg_tv', { device_id: 'dev_lg_tv', integration_id: 'ha_main', commands: [{ command_id: 'media_player.turn_on', label: 'Power on', effect: 'momentary', params: [] }] }]]),
  systemCommands: [{ command_id: 'wait', effect: 'momentary', params: [{ name: 'duration_ms', type: 'int', required: true, minimum: 0, maximum: 60000, step: 1 }] }] as Command[],
  activities: [],
}
const lgOn: Action = { integration_id: 'ha_main', device_id: 'dev_lg_tv', command_id: 'media_player.turn_on', params: {} }

let latest: Action[] = []
function Harness({ initial }: { initial: Action[] }) {
  const [actions, set] = useState(initial)
  latest = actions
  return <SequenceEditor label="Startup" actions={actions} onChange={set} catalog={cat} issues={[]} pathPrefix="/startup" />
}

describe('SequenceEditor', () => {
  it('Duplicate puts a copy right after the step', () => {
    render(<Harness initial={[lgOn, waitAction()]} />)
    fireEvent.click(screen.getByRole('button', { name: 'Duplicate step 1' }))
    expect(latest).toEqual([lgOn, lgOn, waitAction()])
  })

  it('Add wait adds 500 ms; editing a wait changes only its time, in ms', () => {
    render(<Harness initial={[lgOn]} />)
    fireEvent.click(screen.getByRole('button', { name: 'Add wait' }))
    expect(latest[1]).toEqual(waitAction(500))
    fireEvent.click(screen.getByRole('button', { name: 'Edit step 2' }))
    const dialog = within(screen.getByRole('dialog'))
    expect(dialog.queryByLabelText('Device')).toBeNull()
    const field = dialog.getByLabelText(/Wait \(ms\)/) as HTMLInputElement
    expect(field.value).toBe('500')
    fireEvent.change(field, { target: { value: '70000' } })
    expect((dialog.getByRole('button', { name: 'Done' }) as HTMLButtonElement).disabled).toBe(true)
    fireEvent.change(field, { target: { value: '1500' } })
    fireEvent.click(dialog.getByRole('button', { name: 'Done' }))
    expect(latest).toEqual([lgOn, waitAction(1500)])
  })

  it('a stored wait out of range opens with Done off', () => {
    render(<Harness initial={[waitAction(90_000)]} />)
    fireEvent.click(screen.getByRole('button', { name: 'Edit step 1' }))
    const dialog = within(screen.getByRole('dialog'))
    expect((dialog.getByRole('button', { name: 'Done' }) as HTMLButtonElement).disabled).toBe(true)
  })
})
