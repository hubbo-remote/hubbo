# State checks: skip what's already true

Approved by the user on 2026-11-15. It builds on activity runs (docs/design/activity-runs.md), on the
same branch. The trigger: switching a room from one activity to another sent "turn on" to a
receiver that was already on. Home Assistant took 5 s to answer, the step timed out, and the input
change came late, or not at all on firmware that stopped at a failure.

## The idea

Harmony could only write, so it had to guess. Hubbo can read Home Assistant. A startup list should
read as the state the room needs to be in, and switching activities should do only what's
missing. Watch TV → Play games sees that the TV and receiver are already on and changes the input
straight away. From Off, every step runs as it does today.

## What the user decided

- **Each step stands alone.** A step is checked against HA's current state just before it runs,
  and skipped if that state is already true. No step depends on another step, and nothing is
  remembered between steps or between runs.
- **Waits stay.** HA reporting "on" doesn't mean a device is ready for its next command, so a
  `wait` always waits. A step's state is never used to time the steps after it.
- **Not now:** waiting for a state to become true, and an input step that implies power on.

## The check

- **Where:** startup and shutdown sequences only. Button presses, smart buttons and API runs never
  check, so a press never waits on a read.
- **Which steps:** any HA device, by a generic rule taken from what HA provides: the service's
  fields and the entity's attributes. There's no list of devices, domains or commands, and a step
  is checked only when the rule can say for certain what it would change.
  1. **Power.** `<domain>.turn_on` is already true when the entity's state is `on`, or for a
     media player `playing`, `paused`, `idle` or `buffering`. Any other state (a scene's
     timestamp, a climate mode) runs the step. `<domain>.turn_off` is already true when the state
     is `off` or `standby`.
  2. **Setters.** A step's field is already true when the entity has an attribute with the same
     name and that attribute already equals the step's value. Examples:
     - `media_player.select_source` (`source`) and `select_sound_mode` (`sound_mode`)
     - `fan.set_preset_mode` (`preset_mode`) and `set_percentage` (`percentage`)
     - `climate.set_fan_mode` (`fan_mode`) and `set_temperature` (`temperature`)
     - `media_player.volume_set` (`volume_level`) and `volume_mute` (`is_volume_muted`)

     Most of HA follows this naming. Where a domain doesn't, nothing matches and the step simply
     runs.
  3. **Together.** A step is skipped only if every part is already true: power (for a
     turn_on/turn_off) and every field it sends. So `light.turn_on` with `brightness: 128` skips
     only when the light is on at 128. A field with no matching attribute (`brightness_pct`,
     `transition`) makes the step run.
  4. **Values.** Strings and booleans must be equal. Numbers count as equal within a tiny tolerance
     (HA rounds some values). Lists and objects (`rgb_color`) are never compared, so a step with
     one always runs.
  5. **Never checked:** services with no target state, such as toggles, steps (`volume_up`,
     `brightness_step`), `media_play_pause` and `remote.send_command`. Rules 1 to 3 exclude them
     on their own, with no exception list.

- **How:** the executor flags startup and shutdown steps. The HA adapter, on the integration task,
  does a fresh read of that one entity just before sending, using the HA client's existing
  single-entity read, and applies the rule. The read isn't cached, and the engine task never waits
  on it.
- **Never less reliable:** if the read fails, takes longer than 300 ms, or the entity is
  `unavailable` or `unknown`, the step is sent as it would be today.

## Slow replies: 1 s, then move on

Some HA integrations act on a command at once but don't answer the service call until their own
timeout runs out. One receiver in use answers every command after 5.0 s, even when telnet is on.
HA learns the new state from its next poll.

- **An HA step waits for HA's reply for at most 1 s**, rather than the 5 s step timeout. The HA
  adapter owns this limit: it sends each call with a 1 s client timeout and finishes the step as
  `no_reply` when that runs out. The executor's 5 s deadline stays as a backstop. It applies to every HA
  step in every sequence. Steps for other integrations keep their timeouts.
- **A reply within 1 s** is recorded as usual. HA's own errors (unknown entity, bad value, not
  connected) come back in milliseconds, so they still show as failures.
- **No reply within 1 s:** the step's result is `no_reply`. That isn't a failure, since the device
  may well have acted. It doesn't count toward `failed_steps`, so a run can still end `completed`.
  The sequence goes on at once.
- **Nothing keeps listening.** A late reply is dropped, like a late reply to a cancelled step
  today. Nothing new runs in the background.
- **No refresh after no reply.** HA may show the old state until its next poll, so an activity
  started in that window can skip a step it needed. The user accepted this (2026-11-15): Hubbo
  doesn't ask HA to refresh.
- Timing between steps belongs to explicit waits. A fast device's reply still arrives before its
  next step starts, as a side effect.

## In the run history

- A step can have two new results:
  - `skipped`, with the reason `already_set`. It counts as ok.
  - `no_reply`, with no error. It also counts as ok.

  A run with only these and ok steps ends `completed`.
- The web admin shows them plainly: "Receiver · Power on · already on" (or "already off"), "Receiver ·
  Select source · already set" for a setter, and "sent, no reply" for no reply. A step on the wire
  has no params or values, so a run step stays small. No reply isn't shown as a warning. Copy stays light.

## Owners

- **core:**
  - The check in the executor: async, since a read is pending; bounded; transition lane only.
  - The generic rule: power, plus fields matched to attributes. No per-device table.
  - The 1 s HA reply limit, and the `skipped` and `no_reply` step results.
  - hub-sim support: `/sim/ha` states, plus a slow-reply mode for an entity.
  - Tests: skip, run, read timeout, unavailable, all-or-nothing fields, a switch that skips power
    and runs the input, no reply after 1 s with the run still `completed`, and a fast error still
    `failed`.
- **api:** `skipped`, `no_reply` and `already_set` in the spec, fixtures, contract and
  docs/agent-api.md.
- **frontend:** the step copy in "Recent activity" and the mock.
- **firmware:** nothing expected. Check the loop gap during a checked startup.
- **lead:** this doc, ARCHITECTURE.md, IMPLEMENTATION_PLAN.md.
