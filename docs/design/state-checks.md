# State checks: skip what's already true

Harmony could only write, so it had to guess. Hubbo can read Home Assistant. A startup list should
read as the state the room needs to be in, and switching activities should do only what's
missing. Watch TV → Play games sees that the TV and receiver are already on and changes the input
straight away. From Off, every step runs.

The case that prompted it: switching activities sent "turn on" to a receiver that was already on,
HA took 5 s to answer, the step timed out, and the input change came late.

## Rules

- **Each step stands alone.** A step is checked against HA's current state just before it runs,
  and skipped if that state is already true. No step depends on another step, and nothing is
  remembered between steps or between runs. Hubbo reads state when it needs it; it never tracks it.
- **Waits stay.** HA reporting "on" doesn't mean a device is ready for its next command, so a
  `wait` always waits. A step's state is never used to time the steps after it.
- **Not yet:** waiting for a state to become true, and an input step that implies power on.

## The check

- **Where:** startup and shutdown sequences only. Button presses, smart buttons, macros and API
  runs never check, so a press never waits on a read.
- **Which steps:** any HA device, by a generic rule taken from what HA provides: the service's
  fields and the entity's attributes. There's no list of devices, domains or commands, and a step
  is checked only when the rule can say for certain what it would change.
  1. **Power.** `<domain>.turn_on` is already true when the entity's state is `on`, or for a
     media player `playing`, `paused`, `idle` or `buffering`. Any other state (a scene's
     timestamp) runs the step. `<domain>.turn_off` is already true when the state is `off` or
     `standby`.
  2. **Setters.** A step's field is already true when the entity has an attribute with the same
     name and that attribute already equals the step's value. Examples:
     - `media_player.select_source` (`source`) and `select_sound_mode` (`sound_mode`)
     - `fan.set_preset_mode` (`preset_mode`) and `set_percentage` (`percentage`)
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
  does a fresh read of that one entity just before sending, using the HA client's single-entity
  read, and applies the rule. The read isn't cached, and the engine task never waits on it.
- **Never less reliable:** if the read fails, takes longer than 300 ms, or the entity is
  `unavailable` or `unknown`, the step is sent anyway.

## Slow replies: 1 s, then move on

Some HA integrations act on a command at once but don't answer the service call until their own
timeout runs out (some receivers answer every command after 5 s). HA learns the new state from its
next poll.

- **An HA step waits for HA's reply for at most 1 s**, rather than the 5 s step timeout. The HA
  adapter owns this limit: it sends each call with a 1 s client timeout and finishes the step as
  `no_reply` when that runs out. The executor's 5 s deadline stays as a backstop. It applies to
  every HA step in every sequence. Steps for other integrations keep their timeouts.
- **A reply within 1 s** is recorded as usual. HA's own errors (unknown entity, bad value, not
  connected) come back in milliseconds, so they still show as failures.
- **No reply within 1 s:** the step's result is `no_reply`. That isn't a failure, since the device
  may well have acted. It doesn't count toward `failed_steps`, so a run can still end `completed`.
  The sequence goes on at once.
- **Nothing keeps listening.** A late reply is dropped, like a late reply to a cancelled step.
- **No refresh after no reply.** HA may show the old state until its next poll, so an activity
  started in that window can skip a step it needed. That's accepted: Hubbo doesn't ask HA to
  refresh.

## In the run history

- `skipped`, with the reason `already_set`, and `no_reply` (no error) both count as ok. A run with
  only these and ok steps ends `completed` ([activity-runs.md](activity-runs.md)).
- The web admin shows them plainly: "Receiver · Power on · already on" (or "already off"),
  "Receiver · Select source · already set" for a setter, and "sent, no reply". A run step carries
  no params or values, so it stays small.
