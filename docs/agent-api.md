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
pair the remote or Bluetooth; manage tokens or the password; back up, restore or update
firmware. Ask the admin for those.

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

Both return `202` with the number of queued actions. They return immediately; the sequence runs
in the background.

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
| 422 | `validation_failed` | Read `details[].path` and `code`. |
| 429 | `rate_limited` | Wait `Retry-After` seconds. |
| 503 | `integration_unavailable` | HA or Bluetooth is down. Check `/integrations`. |

Detail codes include `unknown_device`, `integration_mismatch`, `unknown_command`,
`unknown_param`, `missing_param`, `invalid_param_type`, `param_out_of_range`,
`invalid_enum_value`, `invalid_trigger`, `misplaced_transition` and `forbidden_param`. The full
list is in `ErrorDetail` in the spec.

## Troubleshooting

- **A command is missing from capabilities.** Hubbo only offers commands the entity supports
  (HA `supported_features`) and that are on its allowlist (docs/ha-capabilities.md). Toggles,
  `play_media` and integration-specific services are deliberately absent.
- **A device shows `availability: stale`.** Its HA entity was removed or renamed. Only the admin
  can re-link it.
- **`timeout` on HA commands.** Hubbo waited and gave up; the device may still have acted. Don't
  retry volume steps blindly.
- **Volume buttons step once per press.** Repeat-while-held isn't implemented yet.
- **`/status` diagnostics** show recent errors and RF dispatch latency.

## Limits

16 activities, 64 devices, 32 actions per sequence or binding, 4 params per action, names up to
64 characters, enum strings up to 32, `wait` up to 60 s.
