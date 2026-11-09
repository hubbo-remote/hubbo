import { describe, expect, it } from 'vitest'
import { fixture } from '../../mock/fixtures.ts'
import type { TransitionResult } from '../api/types'
import { describeTransition } from './transition'

const names = (id: string) => ({ act_play_xbox: 'Play Xbox' })[id] ?? id

describe('describeTransition', () => {
  it('reports a normal switch with its queued startup', () => {
    expect(describeTransition(fixture<TransitionResult>('activities/activate.json'), names)).toEqual({ variant: 'success', title: 'Switched to Play Xbox', description: '7 steps queued' })
  })

  it('says the switch happened even when startup was rejected (api/fixtures/activities/activate.queue_full.json)', () => {
    const m = describeTransition(fixture<TransitionResult>('activities/activate.queue_full.json'), names)
    expect(m.variant).toBe('warning')
    expect(m.title).toBe('Switched to Play Xbox, but its startup didn’t run')
    expect(m.description).toContain('the action queue was full')
  })

  it('reports off with a rejected shutdown', () => {
    const r: TransitionResult = { switched: true, active_activity_id: null, previous_activity_id: 'act_play_xbox', generation: 3, startup: { status: 'none', queued_actions: 0 }, shutdown: { status: 'rejected', queued_actions: 0, reason: 'queue_full' } }
    expect(describeTransition(r, names).title).toBe('Turned off, but the shutdown didn’t run')
  })
})
