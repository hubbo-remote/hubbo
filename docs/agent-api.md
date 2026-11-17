# Using the Hubbo API from an agent

Hubbo is a universal-remote hub. A handheld remote's buttons start activities ("Watch Apple
TV"), which turn on and switch the TV, receiver, Bluetooth streamers and Home Assistant devices.
While an activity runs, each button sends a command to one of its devices. This guide is for
agents and scripts with the hub's address and an API token.

**This guide is enough for everyday tasks; the spec is for exact schemas.** The spec,
`GET /api/v1/openapi.json` (OpenAPI 3.1), is large: look up only the piece you need, by the
pointers given here, e.g. `#/components/schemas/Activity`. Neither needs a token.

## Ground rules

- **Only registered devices.** You act on the devices an admin has added, with the commands
  listed in each device's capabilities. You can't call Home Assistant services, name HA entities
  or send raw payloads. Requests that try (`entity_id`, `target`, `area_id`, `service`, or nested
  objects in `params`) get `422`.
- **IDs, not names.** Use the `dev_…` and `act_…` IDs from the API. Names are for people and are
  never matched.
- **Read, don't guess.** Read a device's capabilities before building actions, and take ranges,
  steps and options from there. Validate before you save.
- **`accepted` is not "it worked".** It means the integration took the command. Hubbo doesn't
  track device state, so it can't tell you the TV is on.

## Connecting

```sh
export HUBBO=http://hubbo-3f2a.local/api/v1     # the hub's address, as you were given it
export HUBBO_TOKEN=hbt_k3x9q2mf_...
curl -s -H "Authorization: Bearer $HUBBO_TOKEN" $HUBBO/status
```

Send `Content-Type: application/json` with every body. A token needs no cookies. The admin
creates tokens in Settings → API tokens, and each is shown once. A `401` means the token is
missing, wrong or revoked; its error carries `"docs": "/api/v1/agent.md"`.

A token can read everything below, run commands, and manage activities, global bindings and
smart buttons. It can't add or change devices, integrations, pairing, tokens, backups or firmware
(`403 forbidden`): ask the admin for those.

## Devices and their commands

```sh
H="Authorization: Bearer $HUBBO_TOKEN"
curl -s -H "$H" $HUBBO/devices                          # every device, short form
curl -s -H "$H" $HUBBO/devices/dev_apple_tv             # one device, with its profile
curl -s -H "$H" $HUBBO/devices/dev_denon/capabilities   # the commands you can send it
```

A device's capabilities list its commands and their params
(`#/components/schemas/DeviceCapabilities`):

```json
{"command_id": "media_player.volume_set", "label": "Set volume", "effect": "momentary",
 "params": [{"name": "level", "type": "number", "required": true, "minimum": 0, "maximum": 1}]}
```

- **Param types:** `bool`, `int`, `number`, `enum` (one of `options`, exact match) and
  `activity_ref` (an activity ID). `minimum`, `maximum` and `step` are the real limits: a fan's
  `percentage` steps by its speed (e.g. `step: 33`). Use them.
- **Optional params** can be left out, which isn't the same as sending a value: `light.turn_on`
  without `brightness` keeps the last brightness. A `default` is what the hub uses when you leave
  the param out.
- **`label`, `unit`, `description`** say what a param means.
- **`effect`:** `momentary` (a one-shot), `hold` (leaves a key held: `press_down`, `key_down`),
  or `wait` (pauses a startup or shutdown).

**Home Assistant devices** (`integration_id: "ha_main"`) offer their domain's commands
(`media_player.turn_on`, `media_player.select_source {source}`, `light.turn_on`,
`cover.set_cover_position {position}`, `scene.turn_on`...). The domains are media_player, light,
switch, fan, scene, script, cover, button, input_boolean, humidifier, select and remote (`remote.turn_on`,
`.turn_off`, `.toggle`). Media players, lights, switches, fans and remotes often add their
integration's own services (`sonos.set_sleep_timer`),
typed from HA's metadata (labels, units, defaults, descriptions). Response-only services and
`play_media` aren't offered. Services Hubbo can't type (free text, objects: `webostv.button`)
are offered through one command, `call_service` ("Home Assistant action", see Actions). A remote's
`call_service` also lists HA's `remote` services (`remote.send_command`, `remote.learn_command`,
`remote.delete_command`, and `remote.turn_on` for its `activity` when the remote has activities). A device whose entity disappeared
shows `availability: stale`; its commands fail until an admin re-links it.

**Bluetooth devices** (`integration_id: "ble"`, an Apple TV, a Fire TV...) were added from a
profile. `GET /devices/{id}` (`#/components/schemas/DeviceDetail`) shows it: `commands` (names
such as `Select`, `Home`, `PlayPause`), `buttons` (which remote button usually sends which
command, e.g. `{"menu": "Home"}`), `power`, `timing` and `inputs`. Their capabilities come in
three groups:

| Group | Commands | Param |
|---|---|---|
| `remote` | `press`, `press_down`, `press_up` | `command`: a profile command name |
| `keys` | `key_tap`, `key_down`, `key_up` | `key`: `KEY_A`, `ARROW_UP`, `F1`... |
| `custom` | `usage_tap`, `usage_down`, `usage_up` | `usage`: a raw consumer code, 1-1023 |

Prefer `remote`: the same name sends the right code for that device. `press` holds the key as long
as the profile says; a profile command with its own `hold_ms` (Apple TV's `ControlCenter` is held
1 s) is a timed press. `press {command, hold_ms}` (1-10000) holds any command that long.
`wait_connected {timeout_ms}` waits for the device's Bluetooth link (see Steps).

Bluetooth commands work only while the device is `bluetooth.state: connected`; otherwise they
fail and nothing is sent.

**Hubbo's own commands** come from the `system` integration (`GET /integrations`, its
`commands`), with `"device_id": null`: `wait {duration_ms}`, `activate {activity_id}` and
`off {}`.

## Actions

Every action has the same four fields (`#/components/schemas/Action`):

```json
{"integration_id": "ha_main", "device_id": "dev_denon", "command_id": "media_player.volume_set", "params": {"level": 0.35}}
{"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "press", "params": {"command": "PlayPause"}}
{"integration_id": "system", "device_id": null, "command_id": "wait", "params": {"duration_ms": 1500}}
```

`integration_id` must be the device's own. `params` is always present (`{}` when there are none)
and holds at most 4 scalar values.

**A Home Assistant action** sends one of a device's untyped integration services with JSON data.
The device's capabilities list them in the `call_service` command's `services`, each with field
hints (`required`, `example`, `description`; `#/components/schemas/HaCustomService`). The action
adds `call` (`#/components/schemas/HaServiceCall`):

```json
{"integration_id": "ha_main", "device_id": "dev_lg_tv", "command_id": "call_service", "params": {},
 "call": {"service": "webostv.button", "data": {"button": "INFO"}}}
```

`data` goes to Home Assistant as is, aimed at the device's entity: at most 1024 bytes, 4 levels
deep, no target keys (`entity_id`, `device_id`, ...: `forbidden_param`), no templates (`{{`, `{%`:
`template_not_supported`). Use a service from the list. Its domain must be the device's own
integration (`webostv.*` for this TV), or on a remote also `remote`
(`{"service": "remote.send_command", "data": {"command": "DPAD_UP"}}`): any other, such as
`homeassistant.stop`, is `forbidden_param` at `/call/service`. HA answers `failed` for one the entity doesn't have. A config holds at most 16 of them.

**Try one** before you put it in an activity:

```sh
curl -s -X POST -H "$H" -H 'Content-Type: application/json' -d '{"params":{"level":0.35}}' \
  $HUBBO/devices/dev_denon/commands/media_player.volume_set/execute
# {"status":"accepted","device_id":"dev_denon","command_id":"media_player.volume_set","elapsed_ms":84}
```

A Home Assistant action takes its `call` in the body:
`POST /devices/dev_lg_tv/commands/call_service/execute` with
`{"params": {}, "call": {"service": "webostv.button", "data": {"button": "INFO"}}}`.

`200` comes back with `status` `accepted`, `failed` or `timeout`; all three mean it was sent. A
`4xx` means nothing was sent: read `error.details`. Only `momentary` commands can be executed:
`press_down` would leave a key held, so it gets `422 hold_not_allowed` (send `press`), and
`wait_connected` gets `422 sequence_only`.

## Activities

```json
{"name": "Watch Apple TV",
 "startup":  [ ...actions... ],
 "shutdown": [ ...actions... ],
 "bindings": {"<button>:<event>": [ ...actions... ]},
 "macros":   ["<button>:<event>", ...]}
```

`GET /activities` lists them, with `active_activity_id`. `GET /activities/{id}` adds `id`,
`schema_version` and `revision` (`#/components/schemas/Activity`).

### Steps: startup and shutdown

- **Startup** runs when the activity starts. **Shutdown** runs only on Off (the remote's Off,
  or `POST /activities/off`).
- **Switching from A to B runs only B's startup**, never A's shutdown. So a startup must leave
  every device the way the activity needs it, whatever was on before: power each device on with
  a discrete on command (`media_player.turn_on`, a profile's `power.on`), never a toggle, and set
  every input explicitly (the TV's source and the receiver's).
- **`wait {duration_ms}`** (0-60000) pauses the sequence. Devices need time after power-on
  (a profile's `power.on_delay_ms`) and after an input change (`timing.input_delay_ms`). A TV
  often ignores an input change while it's waking: put a wait of about 3 s between its
  `turn_on` and its `select_source`.
- **`wait_connected {timeout_ms}`** (1000-60000, default 15000) waits until a Bluetooth device is
  connected, then the sequence goes on. If the time runs out, it goes on anyway. Put it before a
  Bluetooth device's first command in a startup. Only startup and shutdown can use it.
- **Inputs:** HA devices use `select_source {source}`, picking from its `options`. A profile's
  `inputs` list named steps.
- **Power from a profile:** `power.type` is `discrete` (`on`/`off` steps), `toggle` (one
  `toggle` step: leave it out of startup, since repeating it turns the device off) or `none` (no
  steps; an Apple TV wakes on any key and sleeps on its own). Each profile step is
  `{"command": X}` (send `press {command: X}`) or `{"delay_ms": N}` (send `wait`).
- `activate` and `off` can't be used in startup or shutdown.

A typical startup:

```json
[{"integration_id": "ha_main", "device_id": "dev_lg_tv", "command_id": "media_player.turn_on", "params": {}},
 {"integration_id": "ha_main", "device_id": "dev_denon", "command_id": "media_player.turn_on", "params": {}},
 {"integration_id": "system", "device_id": null, "command_id": "wait", "params": {"duration_ms": 3000}},
 {"integration_id": "ha_main", "device_id": "dev_lg_tv", "command_id": "media_player.select_source", "params": {"source": "HDMI 1"}},
 {"integration_id": "ha_main", "device_id": "dev_denon", "command_id": "media_player.select_source", "params": {"source": "TV Audio"}},
 {"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "wait_connected", "params": {"timeout_ms": 15000}},
 {"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "press", "params": {"command": "Home"}}]
```

### Buttons

The remote's button ids come from `GET /remote/buttons`: `up`, `down`, `left`, `right`,
`select`, `menu`, `back`, `volume_up`, `volume_down`, `mute`, `play`, `pause`, `number_1`... It
also gives `hold_ms`, the long-press time (800).

**Triggers.** `bindings` maps `"<button>:<event>"` to a list of actions, run in order:

| Event | Fires |
|---|---|
| `down` | as soon as the button goes down |
| `up` | when it's released |
| `tap` | on release, if it was held less than 800 ms |
| `hold` | once, when it has been held 800 ms (then `tap` doesn't fire) |

A trigger that isn't in `bindings` does nothing. A binding's list has at least one action:
there are no empty lists.

**How to write a button.** Think of each button as a **Press** and, optionally, a **Long
press**. Write them the way the web admin does, so the admin sees them as you meant:

1. **Press, with no Long press:** the button passes straight through. A Bluetooth command goes
   on `down` as its hold form and on `up` as its release, with the same params, so holding the
   remote's button holds the key (the d-pad scrolls, fast-forward keeps going):

   ```json
   "up:down": [{"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "press_down", "params": {"command": "DirectionUp"}}],
   "up:up":   [{"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "press_up",   "params": {"command": "DirectionUp"}}]
   ```

   The same goes for `key_down`/`key_up` (`{"key": ...}`) and `usage_down`/`usage_up`
   (`{"usage": ...}`).

2. **Press, with a Long press:** Press becomes a `tap` (send `press`, the one-shot form), and
   Long press goes on `hold`. A `down` would fire on every press before the hub knows whether
   it's long, so don't use `down`/`up` on a button that has a `hold`:

   ```json
   "menu:tap":  [{"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "press", "params": {"command": "Home"}}],
   "menu:hold": [{"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "press", "params": {"command": "ControlCenter"}}]
   ```

3. **Exceptions to rule 1:**
   - **A timed profile command** (one with its own `hold_ms` in `GET /devices/{id}` `commands`)
     always goes on `tap` as `press`: the hub holds it for exactly that long.
   - **Home Assistant commands** have no down/up form. Press goes on `down` (sent on press) or
     `tap` (sent on release); both are valid, prefer `down`, which is what the web admin writes.
     With a Long press, Press goes on `tap` as in rule 2.
   - **Hubbo's own commands** (`activate`, `off`) always go on `tap`, and must be the last
     action in their list.

4. **A macro:** a Press or Long press listed in `macros` (next to `bindings`, in an activity and in
   the global bindings; left out, it means none), written like a startup: one-shot commands
   (`press`, not `press_down`/`press_up`, which get `hold_not_allowed`) and `wait`s between them, 1
   to 32. A list of more than one action must be a macro. It goes where a Press or Long press
   would: `down` with no Long press, `tap` with one, `hold` for the Long press. Macros run one
   after another on their own queue, so a macro's waits never hold up other buttons; a press while
   too many macros wait (4, or too many macro steps) is dropped (and not listed in the runs). A
   macro of only waits is refused (`sequence_only`). A switch of activity cancels the rest. A Press
   that isn't a macro passes through as in rule 1.

   ```json
   "guide:down": [
     {"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "press", "params": {"command": "Home"}},
     {"integration_id": "system", "device_id": null, "command_id": "wait", "params": {"duration_ms": 500}},
     {"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "press", "params": {"command": "DirectionDown"}}],
   ...
   "macros": ["guide:down"]
   ```

   To try a macro without saving it, `POST /macros/run` with `{actions, trigger, activity_id}`
   (`activity_id` null for a global one): `202 {run_id}` once it's queued, then read the run in
   `GET /activity-runs`. The same rules apply (`422`), and `503 busy` means too many macros are
   waiting (nothing queued; retrying is safe).

**Holding never repeats.** Each trigger fires once per press: holding Volume up sends one HA
`volume_up`, not a stream. Only a Bluetooth `*_down`/`*_up` pair is held as a key for as long as
the button is.

**Long press on an HA device.** HA devices have no `buttons` map: pick commands from their
capabilities. E.g. Mute on a receiver, press to mute and long-press to unmute:

```json
"mute:tap":  [{"integration_id": "ha_main", "device_id": "dev_denon", "command_id": "media_player.volume_mute", "params": {"muted": true}}],
"mute:hold": [{"integration_id": "ha_main", "device_id": "dev_denon", "command_id": "media_player.volume_mute", "params": {"muted": false}}]
```

**Bind only devices the startup prepares.** If a button drives a device the startup doesn't turn
on (a receiver, say), add that device's power-on and input to the startup, or the button may do
nothing.

**Enforced vs advice.** Validate and save refuse (`422`): unknown buttons or events, a `*_down` off
`down` or without its matching `*_up` (`hold_not_allowed`, `unpaired_hold`), `activate`/`off` not
last or in a sequence, a list of more than one action not in `macros`, a wait outside a macro, a
`*_down`/`*_up` in a macro, a `macros` entry that isn't bound or isn't `down`/`tap`/`hold`, and
every param check. The rest of this section is how the web admin writes buttons, so it shows
them as Press and Long press: the hub accepts `down` with `tap` on one button (the web then lists
that button's triggers raw).

**Global bindings** (`GET /global-bindings`, `PUT /global-bindings` with `{bindings, macros,
revision}`) hold the buttons that work in every activity: activity buttons (`"tv:tap"` →
`activate act_watch_apple_tv`), Off (`"off:tap"` → `off`), lights. They apply **per button**: if the active
activity binds any trigger of a button (`down`, `up`, `tap` or `hold`, with at least one action),
none of that button's globals apply. A button the activity doesn't bind at all gets its globals.
To see what an activity inherits, read both and take the global buttons the activity doesn't
mention:

```sh
curl -s -H "$H" $HUBBO/global-bindings > g.json
curl -s -H "$H" $HUBBO/activities/act_watch_apple_tv > a.json
jq -n --slurpfile g g.json --slurpfile a a.json '
  ($a[0].bindings | keys | map(split(":")[0]) | unique) as $bound
  | $g[0].bindings | with_entries(select((.key | split(":")[0]) as $b | $bound | index($b) | not))'
```

So binding just `tv:hold` in an activity also hides the global `tv:tap`.

### Recipes

Each recipe edits `a.json` (from `GET /activities/{id}`) with `jq`, for brevity (any JSON edit
does), then validates and saves it as in "Saving" below. `+=` keeps a button's other triggers:
when you change how a button works, remove its old triggers first (see "Remove a button").

**Bind the d-pad and Select to an Apple TV** (pass-through, so holding scrolls):

```sh
jq '.bindings += ([["up","DirectionUp"],["down","DirectionDown"],["left","DirectionLeft"],
                   ["right","DirectionRight"],["select","Select"]]
  | map(. as [$b,$c] | {
      ($b+":down"): [{integration_id:"ble",device_id:"dev_apple_tv",command_id:"press_down",params:{command:$c}}],
      ($b+":up"):   [{integration_id:"ble",device_id:"dev_apple_tv",command_id:"press_up",params:{command:$c}}]})
  | add)' a.json > b.json
```

**Send volume to the receiver in this activity** (HA: once per press, on `down`):

```sh
jq '.bindings += {
  "volume_up:down":   [{"integration_id":"ha_main","device_id":"dev_denon","command_id":"media_player.volume_up","params":{}}],
  "volume_down:down": [{"integration_id":"ha_main","device_id":"dev_denon","command_id":"media_player.volume_down","params":{}}],
  "mute:down":        [{"integration_id":"ha_main","device_id":"dev_denon","command_id":"media_player.volume_mute","params":{"muted":true}}]}' a.json > b.json
```

**Put a Long press on Menu** (Press moves from `down`/`up` to `tap`):

```sh
jq 'del(.bindings["menu:down"], .bindings["menu:up"]) | .bindings += {
  "menu:tap":  [{"integration_id":"ble","device_id":"dev_apple_tv","command_id":"press","params":{"command":"Home"}}],
  "menu:hold": [{"integration_id":"ble","device_id":"dev_apple_tv","command_id":"press","params":{"command":"ControlCenter"}}]}' a.json > b.json
```

To take the Long press off, delete `menu:hold` and turn `menu:tap` back into the
`press_down`/`press_up` pair.

**Remove a button** from the activity (its globals, if any, then apply again):

```sh
jq '.bindings |= with_entries(select(.key | startswith("menu:") | not))' a.json > b.json
```

### Saving

**Validate** (a dry run, always `200`). `path` is a JSON Pointer into what you sent:

```sh
curl -s -X POST -H "$H" -H 'Content-Type: application/json' -d @b.json $HUBBO/activities/validate
# {"valid":false,"errors":[{"path":"/bindings/volume_up:down/0/params/level","code":"param_out_of_range","message":"..."}]}
```

Fix and repeat until `"valid": true`. Then:

- **Create:** `POST /activities` with exactly `{name, startup, shutdown, bindings}` → `201`, the
  stored activity with its `id` and `revision`.
- **Replace:** `PUT /activities/{id}` with the activity as you read it, changed. `revision` is
  required (the one you read); `id` and `schema_version` may be echoed back and are ignored. It
  replaces the whole activity at once. `validate` takes the same body and ignores `id` and
  `revision`, so it can't catch a stale revision. If someone changed it since your GET, you
  get `409 revision_conflict` with `current_revision`: GET again and redo your change; don't just
  bump the number.
- **Delete:** `DELETE /activities/{id}?revision=N`. An activity another binding activates gets
  `409 activity_in_use` (`details` says where): remove that binding first.

```sh
curl -s -X PUT -H "$H" -H 'Content-Type: application/json' -d @b.json $HUBBO/activities/act_watch_apple_tv
```

`PUT /global-bindings` takes `{bindings, macros, revision}` the same way. It has no dry run: a mistake
gets `422` with the same `details`, and nothing is saved.

### Smart buttons

Light 1, Light 2, Switch 1 and Switch 2 are **slots** (`GET /smart-buttons`,
`#/components/schemas/SmartButtons`). Each holds a device id or `null`, and that's all you set. The
hub works out what the slot's button does, and what +/- do while it's the **selected** slot
(`GET /status` `selected_slot`: the slot whose button was pressed last; `light_1` after a reboot),
from the device's kind and the commands it offers. A light gets on/off and ±10% brightness, a fan
on/off and speed, a cover open/close and ±10 position, and a switch on/off. Other devices get
nothing. `actions` in the answer shows the result, read-only, as `{button, plus, minus}`, each
`{tap, hold, release}`: `release` runs when the button comes up after a hold (it stops a moving
cover). A hold fires once. Quick taps on + or − whose action has a `step` merge into one call
(`step` × the taps, clamped), sent about 300 ms after the last tap.
`GET /smart-buttons/actions?device_id=dev_lamp` previews what a slot would do with a device,
without saving (`#/components/schemas/SmartSlotActions`).

For those six buttons the active activity wins (per button, as above), then the slot, then the
global bindings. An empty slot leaves its button to your bindings. A global binding on + or −
still saves, but it only runs while the selected slot has no +/- actions. To set a slot, send back
`slots` with the `revision` (not `actions`):

```sh
curl -s -H "$H" $HUBBO/smart-buttons \
  | jq '{slots: (.slots | .light_2 = "dev_lamp"), revision}' > sb.json
curl -s -X PUT -H "$H" -H 'Content-Type: application/json' -d @sb.json $HUBBO/smart-buttons
```

A stale `revision` gets `409 revision_conflict`; an unknown device gets `422 unknown_device` at
`/slots/light_2`. A slot's device can't be deleted while the slot holds it (`409 device_in_use`).

### Start and stop

```sh
curl -s -X POST -H "$H" $HUBBO/activities/act_watch_apple_tv/activate
curl -s -X POST -H "$H" $HUBBO/activities/off
```

Both return `200` once the hub has switched; the sequences run in the background
(`#/components/schemas/TransitionResult`):

```json
{"switched": true, "active_activity_id": "act_watch_apple_tv", "previous_activity_id": null,
 "generation": 7, "startup": {"status": "queued", "queued_actions": 5},
 "shutdown": {"status": "none", "queued_actions": 0}}
```

- `switched: true` means the hub switched. **Never retry it**, even when `startup.status` is
  `rejected` (`queue_full`): a retry runs a second full switch.
- Activating the active activity runs its startup again. Off with nothing active does nothing
  (`switched: false`).
- `503 busy` means nothing changed; retrying is safe.

A step that fails or times out doesn't stop its sequence: the hub records it and goes on to the
next step (a `wait` still waits). Switching activity mid-run cancels the rest of the old run. To
see how a startup, shutdown or macro went, `GET /activity-runs`
(`#/components/schemas/ActivityRuns`) lists the last 8 (at most 5 of them macros), newest first,
each with its `kind` (`startup`, `shutdown`, `macro`; a macro also has its `button` and `trigger`,
`press` or `long_press`, and a global one a null `activity_id`), its `outcome` (`completed`,
`completed_with_errors`, `cancelled`, `rejected`, or `running`) and every step's `result` and
`error`. A restart clears them.

In a startup or shutdown, a Home Assistant step is checked against HA's current state first: if
the device is already on (or off), or already has the value the step sets, it's `skipped`
(`already_set`) and nothing is sent. An HA step that gets no answer within 1 s is `no_reply`: the
device may well have acted, and the sequence goes on. Neither counts as a failure, and a `wait`
always waits.

## The hub's state

`GET /status` shows the hub's name (`hubbo.name`), the active activity, the newest activity run
(`last_run`: its `kind`, `button`, `outcome` and `failed_steps`, or null), and `config.state`:

- `ok` (or `fresh`, nothing saved yet): normal.
- `blocked`: the saved configuration can't be used. Nothing is active and every edit gets
  `409 config_blocked`. Only the admin can fix it (restore a backup, or reset); don't retry.
- `storage_unavailable`: the hub can't save; edits get `503 storage_unavailable`.

After the admin restores or resets, every `revision` you hold is stale: GET again.

Firmware updates are the admin's: checking for a release and installing it are admin-only
(`/admin/firmware/check`, `/admin/firmware/install`), and nothing installs by itself. An update
ends in a reboot, and a token works again once the hub is back.

## Errors

```json
{"error": {"code": "validation_failed", "message": "Activity is invalid",
           "details": [{"path": "/startup/0/params/entity_id", "code": "forbidden_param", "message": "..."}]}}
```

- `400 malformed_json`, `415`: fix the JSON or send `Content-Type: application/json`.
- `401`: token missing, wrong or revoked. `403 forbidden`: admin-only. Ask the admin.
- `404 not_found`: a wrong ID (list again), or a path the API doesn't have (see `docs`).
- `409 revision_conflict`: GET, redo the change, PUT. `409 activity_in_use`: another binding
  activates it (`details` says where). `409 config_blocked`: ask the admin.
- `422 validation_failed`: read `details[].path` and `code`; the codes are listed at
  `#/components/schemas/ErrorDetail`.
- `503 busy`: retry shortly. `503 storage_unavailable`: nothing was saved.
  `503 integration_unavailable`: HA or Bluetooth is down (`GET /integrations`).

An HA command's `timeout` may still have acted: don't repeat volume steps blindly. A command
missing from capabilities isn't offered on purpose: ask the admin. `GET /status` `diagnostics`
lists recent errors.

## Limits

16 activities; 64 devices, at most 4 of them Bluetooth; 32 actions per sequence or trigger;
4 params per action; names up to 64 characters; enum strings up to 64; `wait` up to 60 s;
16 Home Assistant actions (`call`), each with at most 1024 bytes of `data`.
