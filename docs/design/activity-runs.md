# Activity runs: keep going, and show what happened

Approved by the user on 2026-11-15, after a hub in daily use silently failed to switch the receiver's
input between activities: a step before the input change failed, the sequence stopped there, and
nothing showed it.

## What the user decided

- **Keep going.** A step that fails or times out is reported, and the sequence goes on to the next
  step. It never stops a sequence. This replaces "on action error, report and stop the sequence".
- **Show it.** The hub keeps the last few activity runs, step by step, and the web admin shows them:
  which steps ran, which failed, and why.
- **Later, not now:** a "wait until on" step to replace fixed waits.

## Keep going

- Applies to every sequence: startup, shutdown, button bindings, smart buttons, API runs. One rule.
- A failed or timed-out step is recorded with its error, and the next step starts at once. A `wait`
  still waits.
- **Cancelled is unchanged:** switching activity (or Off) mid-run cancels the rest of the old run, as
  today. The steps that didn't run are recorded as cancelled.
- **Outcome** of a run: `completed` (every step ok), `completed_with_errors` (it reached the end, and
  one or more steps failed), `cancelled`, `rejected` (couldn't be queued).

## The run history

The hub keeps the **last 8 activity runs** in RAM (lost on reboot, nothing written to flash): a
startup, or a shutdown (Off). Button presses aren't kept; they're too frequent to be useful here.

A run:

| Field | Meaning |
|---|---|
| `activity_id` | the activity |
| `kind` | `startup` or `shutdown` |
| `started_ago_ms` | how long ago it started |
| `duration_ms` | start to the last step's result, or null while running |
| `outcome` | as above, or `running` |
| `steps` | one per action, in order |

A step: `index`, `device_id` (null for system steps), `command_id`, `result` (`ok`, `failed`,
`timed_out`, `cancelled`, or `pending` while running), `error` (the error code, or null),
`duration_ms` (null if it didn't run).

- `GET /activity-runs` returns them newest first. Read with the token, like `/status`.
- `/status` gains `last_run`: the newest run's `activity_id`, `kind`, `outcome` and failed step count,
  so the dashboard can flag a problem without fetching the history.

## Web admin

- **Dashboard:** a "Recent activity" card. Each run is one line, e.g. "Watch TV started · 2 min ago ·
  1 step failed". A run with failures is highlighted. Opening it lists the steps: the device and
  command, a tick or a cross, the reason in plain words ("TV · Turn on · timed out
  after 5 s"), and how long each took.
- **Activity page:** if the newest run of this activity had failures, the failing steps in its
  startup or shutdown list carry a small warning with the reason.
- Copy stays light.

## Owners

- **core:** keep going in the executor; the outcome; the run history (fixed size, no allocation on
  the RF path, readable from the API's task safely); hub-sim; tests.
- **api:** `GET /activity-runs`, `last_run` in `/status`, the spec, fixtures, contract, docs.
- **frontend:** the dashboard card, the activity page's warnings, the mock, e2e.
- **firmware:** any wiring the history needs to be read from httpd (the snapshot pattern the engine
  already uses).
- **lead:** this doc, ARCHITECTURE.md (the "report and stop" rule), IMPLEMENTATION_PLAN.md.
