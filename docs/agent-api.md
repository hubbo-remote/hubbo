# Using the Hubbo API from an agent

This is for LLM agents and scripts that configure or drive a Hubbo hub. The full contract is
`api/openapi.yaml`; realistic request and response bodies are in `api/fixtures/`.

## Ground rules

- **Only registered devices.** You can act on devices an admin has registered, using the
  commands listed in each device's capabilities. You cannot call Home Assistant services, name
  HA entities, or send raw payloads. Requests that try (`entity_id`, `target`, `area_id`,
  `service`, nested objects in `params`) are rejected with `422`.
- **IDs, not names.** Use `dev_…` and `act_…` IDs from the API. Names are for people and are
  never matched.
- **`accepted` is not "it worked".** A command result of `accepted` means the integration took
  the command. Hubbo doesn't track device state, so it can't tell you the TV is on.
- **Don't guess.** Read capabilities before building actions, and validate before saving.

## Connecting

Base URL: `http://<hub>/api/v1` (or `https://` when TLS is set up). The admin creates a token in
Settings → API tokens (`POST /admin/tokens`) and gives it to you once:

```sh
export HUBBO=http://hubbo.local/api/v1
export HUBBO_TOKEN=hbt_k3x9q2mf_...
curl -s -H "Authorization: Bearer $HUBBO_TOKEN" $HUBBO/status
```

Send `Content-Type: application/json` with every body. With a token you need no cookies and no
CSRF header.

**Local simulator:** it runs in `SIMULATOR_NO_AUTH` mode by default. No token is needed, every
request acts as admin, and `/status` shows `"auth_mode": "simulator_no_auth"`. Everything goes to
fake adapters.

## What a token can and can't do

Can: read status, remote buttons, devices and capabilities; run registered commands; create,
replace, delete, validate and activate activities; turn the current activity off; edit global
bindings; list integrations.

Can't (`403 forbidden`): register, edit or delete devices; configure or discover integrations;
set the remote's endpoint or pair the remote or Bluetooth; manage tokens or the password; back
up, restore or update firmware. Ask the admin for those.

## Discover

```sh
curl -s -H "Authorization: Bearer $HUBBO_TOKEN" $HUBBO/devices
curl -s -H "Authorization: Bearer $HUBBO_TOKEN" $HUBBO/devices/dev_denon/capabilities
curl -s -H "Authorization: Bearer $HUBBO_TOKEN" $HUBBO/remote/buttons
curl -s -H "Authorization: Bearer $HUBBO_TOKEN" $HUBBO/integrations
```

A capability lists each command and its parameters:

```json
{"command_id": "media_player.volume_set", "label": "Set volume",
 "params": [{"name": "level", "type": "number", "required": true, "minimum": 0, "maximum": 1}]}
```

Param types: `bool`, `int`, `number`, `enum` (pick from `options`, exact match), and
`activity_ref` (an activity ID). Optional params can be left out. Leaving them out is different
from sending a default; for example `light.turn_on` without `brightness` keeps the last brightness.

Home Assistant devices can also have their integration's own services (e.g.
`webostv.button {button: enum}`), typed from HA's metadata when the device was registered. Their
labels are HA's. A device whose entity is missing from the latest discovery shows
`availability: stale`, and its commands fail (`status: failed`, `message: entity_not_found`)
until an admin re-links it. Until a discovery runs, a renamed or removed entity's commands are
reported `accepted` (HA itself says success).

Deviceless commands come from the `system` integration in `GET /integrations` (`commands`):
`wait {duration_ms}`, `activate {activity_id}`, `off {}`. Use them with `"device_id": null`.

## Actions

Every action has the same four fields:

```json
{"integration_id": "ha_main", "device_id": "dev_denon", "command_id": "media_player.volume_set", "params": {"level": 0.35}}
{"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "key_tap", "params": {"key": "PLAY_PAUSE"}}
{"integration_id": "system", "device_id": null, "command_id": "wait", "params": {"duration_ms": 1500}}
```

`integration_id` must be the device's own `integration_id`. `params` is always present (`{}` if
there are none), holds at most 4 scalar values, and nothing else.

## Try a command

```sh
curl -s -X POST -H "Authorization: Bearer $HUBBO_TOKEN" -H 'Content-Type: application/json' \
  -d '{"params":{"level":0.35}}' \
  $HUBBO/devices/dev_denon/commands/media_player.volume_set/execute
# {"status":"accepted","device_id":"dev_denon","command_id":"media_player.volume_set","elapsed_ms":84}
```

`200` comes back with `status` `accepted`, `failed` or `timeout`, all meaning it was sent. A
`4xx` means nothing was sent: read `error.details`.

Only `momentary` commands can be executed (each command's `effect` in capabilities says which).
A `hold` command such as `key_down` would leave a key held with nothing to release it, so execute
answers `422` with detail code `hold_not_allowed`: send `key_tap` instead. Holds belong in
bindings, as a `down`/`up` pair (`up:down` → `key_down`, `up:up` → `key_up`).

## Activities

An activity is a startup sequence, a shutdown sequence, and bindings from remote triggers to
actions:

- **Startup** runs when the activity is activated. Switching from A to B runs only B's startup,
  never A's shutdown. So startup should set every input it relies on explicitly (e.g. TV source
  and receiver source), and use explicit power-on commands, never toggles.
- **Shutdown** runs only on Off (the remote's Off, or `POST /activities/off`).
- **Bindings** map `"<button>:<event>"` to actions. Events are `down`, `up`, `tap` (released
  before 800 ms) and `hold` (still held at 800 ms, fires once). Use `down` + `up` for key-like
  behaviour (Bluetooth `key_down`/`key_up`, so holding a remote arrow holds the key). Use `tap` +
  `hold` for click-or-hold. Unbound triggers do nothing.
- `system/activate` and `system/off` may appear only as the last action of a binding, never in
  startup or shutdown.

Global bindings (`GET/PUT /global-bindings`) apply when the active activity doesn't bind the
trigger. That's where activity buttons go, e.g. `"tv:tap"` → `activate act_watch_apple_tv`, and
`"off:tap"` → `off`.

### 1. Validate (dry run)

```sh
curl -s -X POST -H "Authorization: Bearer $HUBBO_TOKEN" -H 'Content-Type: application/json' \
  -d @activity.json $HUBBO/activities/validate
# {"valid":false,"errors":[{"path":"/bindings/volume_up:down/0/params/level","code":"param_out_of_range","message":"..."}]}
```

It always returns `200`. `path` is a JSON Pointer into what you sent. Fix and repeat until
`"valid": true`.

### 2. Create in one request

```sh
curl -s -X POST -H "Authorization: Bearer $HUBBO_TOKEN" -H 'Content-Type: application/json' \
  -d @activity.json $HUBBO/activities
```

Body: `{name, startup, shutdown, bindings}`. Response `201` is the stored activity with its `id`
and `revision: 1`. See `api/fixtures/activities/create.request.json` and
`act_watch_apple_tv.json`.

### 3. Edit: GET, change, PUT

```sh
curl -s -H "Authorization: Bearer $HUBBO_TOKEN" $HUBBO/activities/act_watch_apple_tv > a.json
# edit a.json, keep "revision" as it was
curl -s -X PUT -H "Authorization: Bearer $HUBBO_TOKEN" -H 'Content-Type: application/json' \
  -d @a.json $HUBBO/activities/act_watch_apple_tv
```

PUT replaces the whole activity atomically, so either all of it is saved or none of it. If someone
changed it since your GET you get `409 revision_conflict` with `current_revision`. GET again and
reapply your change; don't just bump the number. To delete, send
`DELETE /activities/{id}?revision=N`.

### 4. Activate and turn off

```sh
curl -s -X POST -H "Authorization: Bearer $HUBBO_TOKEN" $HUBBO/activities/act_watch_apple_tv/activate
curl -s -X POST -H "Authorization: Bearer $HUBBO_TOKEN" $HUBBO/activities/off
```

Both return `200` once the engine has handled the request; the sequences run in the
background. The body says what happened, phase by phase:

```json
{"switched": true, "active_activity_id": "act_watch_apple_tv", "previous_activity_id": null,
 "generation": 7, "startup": {"status": "queued", "queued_actions": 5},
 "shutdown": {"status": "none", "queued_actions": 0}}
```

- `switched: true` means the hub changed activity. **Never retry it**, even when
  `startup.status` is `rejected` (the sequence queue was full, `reason: "queue_full"`): a retry
  would run a second full transition. Send the missing commands individually if needed.
- Off with nothing active is a no-op: `switched: false`, `shutdown.status: none`.
- `503 busy` means the request never reached the engine; nothing changed and a retry is safe.

## Configuration state

`GET /status` has `config: {state, generation, reason}`:

- `ok`: running the saved configuration. `fresh`: nothing saved yet (first boot), defaults
  running.
- `blocked`: the saved configuration can't be used (from newer firmware, a failed migration,
  it doesn't compile, or every saved copy is damaged; `reason` says which). Nothing is active
  and every edit gets `409 config_blocked`. Only an admin can fix it: `POST /admin/restore` with a backup, or
  `POST /admin/reset` (factory defaults). Don't keep retrying edits. A backup taken while
  blocked is the saved record's raw bytes, marked `X-Hubbo-Backup: blocked-raw`
  (`application/octet-stream`, `hubbo-backup-BLOCKED-<date>.json`), not a `Backup` document.
  Keep it before a reset; restore accepts it once the firmware can read it.
- `storage_unavailable`: the store can't be written; edits get `503 storage_unavailable`.
  `GET /admin/backup` still exports the running config if it loaded fine before the store
  failed; it's 503 only when the hub booted without being able to read it.

Every change bumps `generation`, and each object's `revision` is the generation of its last
change. A restore or reset stamps everything with a new generation, so any revision you read
before it gets `409 revision_conflict`: GET again. A restore or reset never runs startup or
shutdown commands: the active activity stays active if its id still exists, otherwise nothing
is active.

## Errors

```json
{"error": {"code": "validation_failed", "message": "Activity is invalid",
           "details": [{"path": "/startup/0/params/entity_id", "code": "forbidden_param", "message": "..."}]}}
```

| HTTP | `code` | What to do |
|---|---|---|
| 400 | `malformed_json` | Fix the JSON. |
| 401 | `unauthenticated` | Token missing, wrong or revoked. Ask the admin. |
| 403 | `forbidden` | Admin-only operation. Ask the admin. |
| 404 | `not_found` | Wrong ID. List again. |
| 409 | `revision_conflict` | GET, reapply, PUT. |
| 409 | `device_in_use`, `limit_reached`, `busy` | Read the message; `busy` can be retried shortly. |
| 413 | `payload_too_large` | Activity bodies max 32 KiB. |
| 415 | `unsupported_media_type` | Send `Content-Type: application/json`. |
| 409 | `activity_in_use` | Another binding activates it; remove that first (see `details`). |
| 409 | `config_blocked` | The saved config can't be used; ask the admin to restore or reset. |
| 503 | `storage_unavailable` | The hub can't save right now; nothing changed. |
| 421 | `host_not_allowed` | Use the hub's own address in the URL. |
| 501 | `not_implemented` | Not available yet. |
| 422 | `validation_failed` | Read `details[].path` and `code`. |
| 429 | `rate_limited` | Wait `Retry-After` seconds. |
| 503 | `integration_unavailable` | HA or Bluetooth is down. Check `/integrations`. |

Detail codes include `unknown_device`, `integration_mismatch`, `unknown_command`,
`unknown_param`, `missing_param`, `invalid_param_type`, `param_out_of_range`,
`invalid_enum_value`, `invalid_trigger`, `misplaced_transition` and `forbidden_param`. The full
list is in `ErrorDetail` in the spec.

## Walkthrough on the simulator (the Phase 2 gate)

Create, edit, activate and turn off an activity with curl against hub-sim, then restart it and
see the config survive. Start the simulator with a state directory (in another terminal):

```sh
bin/hubbo sim serve --port 8780 --data build/sim-state
```

No-auth mode needs no token:

```sh
H=http://127.0.0.1:8780/api/v1
J='Content-Type: application/json'

curl -s $H/devices | jq '.devices[].id'
curl -s $H/devices/dev_denon/capabilities | jq '.commands[].command_id'

# Validate, then create in one request.
cat > /tmp/act.json <<'JSON'
{"name": "Movie Night",
 "startup": [{"integration_id": "ha_main", "device_id": "dev_lg_tv", "command_id": "media_player.turn_on", "params": {}},
             {"integration_id": "system", "device_id": null, "command_id": "wait", "params": {"duration_ms": 1000}}],
 "shutdown": [{"integration_id": "ha_main", "device_id": "dev_lg_tv", "command_id": "media_player.turn_off", "params": {}}],
 "bindings": {"volume_up:down": [{"integration_id": "ha_main", "device_id": "dev_denon",
                                  "command_id": "media_player.volume_up", "params": {}}]}}
JSON
curl -s -X POST -H "$J" -d @/tmp/act.json $H/activities/validate     # {"valid":true,"errors":[]}
curl -s -X POST -H "$J" -d @/tmp/act.json $H/activities | tee /tmp/created.json | jq '{id, revision}'
ID=$(jq -r .id /tmp/created.json)                                     # act_movie_night

# Edit: GET, change, PUT with the revision you read.
curl -s $H/activities/$ID | jq '.name = "Movie Night (late)"' > /tmp/edit.json
curl -s -X PUT -H "$J" -d @/tmp/edit.json $H/activities/$ID | jq .revision     # 2
curl -s -X PUT -H "$J" -d @/tmp/edit.json $H/activities/$ID | jq .error       # revision_conflict, current_revision 2

# Activate, check, turn off.
curl -s -X POST $H/activities/$ID/activate        # 200 {"switched":true,"active_activity_id":"act_movie_night",...}
curl -s $H/status | jq .active_activity_id
curl -s -X POST $H/activities/off                 # 200, shutdown.queued_actions = shutdown length
```

Stop hub-sim (Ctrl-C) and start it again with the same `--data`: `GET $H/activities/$ID`
returns the edited activity at revision 2.

With `--auth` the same calls need credentials. First run:

```sh
curl -s -X POST -H "$J" -d '{"username":"admin","password":"correct horse battery"}' $H/auth/setup
# later: POST /auth/login with the same body. Both set the hubbo_session cookie and return csrf_token.
curl -s -c /tmp/jar -X POST -H "$J" -d '{"username":"admin","password":"correct horse battery"}' \
  $H/auth/login | jq -r .csrf_token > /tmp/csrf
curl -s -b /tmp/jar -X POST -H "$J" -H "X-Hubbo-CSRF: $(cat /tmp/csrf)" -d '{"name":"agent"}' \
  $H/admin/tokens | jq -r .token          # hbt_..., shown once
```

Then use `Authorization: Bearer hbt_...` as above.

## Troubleshooting

- **A command is missing from capabilities.** Hubbo only offers commands the entity supports
  (HA `supported_features`) and that are on its allowlist (docs/ha-capabilities.md). Toggles,
  `play_media` are deliberately absent, and an integration service appears only if HA describes
  every field in a way Hubbo can type (discovery lists the rest as `skipped_services`).
- **A device shows `availability: stale`.** Its HA entity wasn't in the latest discovery
  (removed or renamed). Only the admin can re-link it.
- **`timeout` on HA commands.** Hubbo waited and gave up; the device may still have acted. Don't
  retry volume steps blindly.
- **Volume buttons step once per press.** Repeat-while-held isn't implemented yet.
- **`/status` diagnostics** show recent errors and RF dispatch latency.
- **`421 host_not_allowed`.** The `Host` header must be one of the hub's names (its IP, mDNS
  name or hostname; `localhost`/`127.0.0.1` for hub-sim). This blocks DNS-rebinding attacks.
- **`501 not_implemented`.** Firmware update, integration settings and discovery, and
  remote/Bluetooth pairing aren't implemented yet. Nothing happened.

## Limits

16 activities, 64 devices, 32 actions per sequence or binding, 4 params per action, names up to
64 characters, enum strings up to 64, `wait` up to 60 s.
