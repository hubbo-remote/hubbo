import { describe, expect, it } from 'vitest'
import type { Action, ActivityRun, ActivityRunStep } from '@/api/types'
import { activityName, failuresFor, runKind, runSummary, stepAction, stepReason } from './runs'

const step = (over: Partial<ActivityRunStep>): ActivityRunStep => ({ index: 0, device_id: 'dev_tv', command_id: 'media_player.turn_on', result: 'ok', error: null, duration_ms: 40, ...over })
const run = (steps: ActivityRunStep[], over: Partial<ActivityRun> = {}): ActivityRun => ({ id: 1, activity_id: 'act_tv', kind: 'startup', started_ago_ms: 0, duration_ms: 100, outcome: 'completed', steps, ...over })

describe('a step in plain words', () => {
  it('says why it failed, and how long a timeout waited', () => {
    expect(stepReason(step({ result: 'timed_out', error: 'adapter_timeout', duration_ms: 5000 }))).toBe('timed out after 5 s')
    expect(stepReason(step({ result: 'failed', error: 'entity_not_found' }))).toBe('Home Assistant no longer has this device')
    expect(stepReason(step({ result: 'failed', error: 'some_new_code' }))).toBe('failed (some new code)')
    expect(stepReason(step({ result: 'cancelled', error: 'cancelled', duration_ms: null }))).toBe('skipped: another activity took over')
    expect(stepReason(step({ result: 'cancelled', error: 'queue_full', duration_ms: null }))).toBe('skipped: Hubbo was too busy')
    expect(stepReason(step({}))).toBeNull()
  })
  it('says a step was already set, or got no reply, without calling it a failure', () => {
    expect(stepReason(step({ result: 'skipped', error: 'already_set', command_id: 'media_player.turn_on' }))).toBe('already on')
    expect(stepReason(step({ result: 'skipped', error: 'already_set', command_id: 'switch.turn_off' }))).toBe('already off')
    expect(stepReason(step({ result: 'skipped', error: 'already_set', command_id: 'media_player.select_source' }))).toBe('already set')
    expect(stepReason(step({ result: 'no_reply', error: null }))).toBe('sent, no reply')
    const r = run([step({ result: 'skipped', error: 'already_set' }), step({ result: 'no_reply', error: null })])
    expect(runSummary(r)).toBe('completed')
    const tv: Action = { integration_id: 'ha_main', device_id: 'dev_tv', command_id: 'media_player.turn_on', params: {} }
    expect(failuresFor(r, 'startup', [tv, tv]).size).toBe(0)
  })
})

describe('a run in one line', () => {
  it('names it and counts what failed', () => {
    expect(runKind(run([]))).toBe('start')
    expect(runKind(run([], { kind: 'shutdown' }))).toBe('stop')
    expect(runSummary(run([step({}), step({ result: 'timed_out' })], { outcome: 'completed_with_errors' }))).toBe('1 failed')
    expect(runSummary(run([step({})]))).toBe('completed')
    expect(runSummary(run([step({ result: 'failed' }), step({ result: 'cancelled' })], { outcome: 'cancelled' }))).toBe('cancelled, 1 failed')
    expect(runSummary(run([], { outcome: 'running' }))).toBe('running…')
    expect(runSummary(run([], { outcome: 'rejected' }))).toBe('didn’t run')
  })
})

describe('failuresFor', () => {
  const tv: Action = { integration_id: 'ha_main', device_id: 'dev_tv', command_id: 'media_player.turn_on', params: {} }
  const r = run([step({ result: 'failed', error: 'entity_not_found' }), step({ index: 1, result: 'ok' })])
  it('marks a failing step that is still the same command there', () => {
    expect([...failuresFor(r, 'startup', [tv, tv])]).toEqual([[0, 'Home Assistant no longer has this device']])
  })
  it('drops it once the step there changed, and without a run', () => {
    expect(failuresFor(r, 'startup', [{ ...tv, command_id: 'media_player.turn_off' }]).size).toBe(0)
    expect(failuresFor(r, 'shutdown', [tv]).size).toBe(0)
    expect(failuresFor(undefined, 'startup', [tv]).size).toBe(0)
  })
})

describe('a step as an action', () => {
  it('is Hubbo’s own when it has no device, and keeps no params', () => {
    expect(stepAction(step({ device_id: null, command_id: 'wait' }))).toEqual({ integration_id: 'system', device_id: null, command_id: 'wait', params: {} })
    expect(stepAction(step({})).integration_id).toBe('')
  })
  it('names the activity, or a stand-in once it’s gone', () => {
    expect(activityName([{ id: 'act_tv', name: 'Watch TV' }], 'act_tv')).toBe('Watch TV')
    expect(activityName([], 'act_gone')).toBe('An activity')
  })
})
