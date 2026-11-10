import { describe, expect, it } from 'vitest'
import { fixture } from '../../mock/fixtures.ts'
import type { TransitionResult } from '../api/types'
import { describeTransition } from './transition'

const names = (id: string) => ({ act_play_xbox: 'Play Xbox' })[id] ?? id

describe('describeTransition', () => {
  it('reports a normal switch', () => {
    expect(describeTransition(fixture<TransitionResult>('activities/activate.json'), names)).toEqual({ variant: 'success', title: 'Play Xbox is on' })
  })

  it('says the switch happened even when startup was rejected (api/fixtures/activities/activate.queue_full.json)', () => {
    const m = describeTransition(fixture<TransitionResult>('activities/activate.queue_full.json'), names)
    expect(m.variant).toBe('warning')
    expect(m.title).toBe('Play Xbox is on, but its startup didn’t run')
    expect(m.description).toBe('Hubbo was busy. Try again.')
  })

  it('reports off with a rejected shutdown', () => {
    const r: TransitionResult = { switched: true, active_activity_id: null, previous_activity_id: 'act_play_xbox', generation: 3, startup: { status: 'none', queued_actions: 0 }, shutdown: { status: 'rejected', queued_actions: 0, reason: 'queue_full' } }
    expect(describeTransition(r, names).title).toBe('Turned off, but the shutdown didn’t run')
  })
})
