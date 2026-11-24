#!/usr/bin/env python3
"""Generate api/fixtures/*.json from one set of Python definitions.

The fixtures are committed (the frontend mock server and simulator contract
tests read the JSON). Edit this file, not the JSON, then run `api/lint.sh gen`.
`api/lint.sh` fails if the JSON is out of date.

HA entity data is realistic: supported_features values are the masks the real
integrations set (home-assistant/core @ 5a20e53, see docs/ha-capabilities.md),
and the capability lists are what core/src/ha/capabilities.cpp produces for
them (core/tests/ha_capabilities_test.cpp checks the same inputs, and
api/contract.sh checks every capability fixture equals what hub-sim serves).
"""
import copy
import json
import os
import re
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures")
# The built-in profiles, as synced from hubbo-devices (the hub builds them in).
PROFILES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "profiles")


def load_json(name):
    with open(os.path.join(PROFILES_DIR, name), encoding="utf-8") as f:
        return json.load(f)


# ---------------------------------------------------------------- profiles

PROFILE_INDEX = load_json("index.json")
# As the hub keeps them: the spec's document without `meta`.
PROFILES = {}
for entry in PROFILE_INDEX:
    doc = load_json(entry["file"])
    doc.pop("meta", None)
    assert doc["id"] == entry["id"] and doc["revision"] == entry["revision"], entry
    PROFILES[entry["id"]] = doc
SHA256 = {e["id"]: e["sha256"] for e in PROFILE_INDEX}
KINDS = [{"id": k["id"], "label": k["label"]} for k in load_json("kinds.json")["kinds"]]
PROFILE_CONTENT = ("commands", "buttons", "power", "timing", "inputs", "channels")


def profile_ref(pid):
    return {"id": pid, "revision": PROFILES[pid]["revision"], "sha256": SHA256[pid]}


def profile_summary(pid):
    d = PROFILES[pid]
    return dict(profile_ref(pid), manufacturer=d["manufacturer"], model=d["model"], kind=d["kind"],
                platform=d["platform"])


def profile_device(dev_id, pid, name, kind=None, revision=1):
    """A device added from a built-in profile (the detail form, GET /devices/{id})."""
    d = PROFILES[pid]
    out = {"id": dev_id, "name": name, "integration_id": "ble", "kind": kind or d["kind"],
           "provider": {"type": "ble_hid"}, "platform": "ble_hid",
           "manufacturer": d["manufacturer"], "model": d["model"],
           "profile": profile_ref(pid),
           "availability": "available", "bluetooth": {"state": "connected"},
           "schema_version": 1, "revision": revision}
    out.update({k: copy.deepcopy(d[k]) for k in PROFILE_CONTENT if k in d})
    return out


def light(d):
    """The GET /devices form: no profile content."""
    return {k: v for k, v in d.items() if k not in PROFILE_CONTENT}

# ---------------------------------------------------------------- commands

# These build exactly what the hub serves (core ParamSpec/CommandSpec):
# no command descriptions; int params have step 1; a param's label, unit and
# default only where the integration gives them (HA services, Bluetooth).
def p_enum(name, options, required=True):
    return {"name": name, "type": "enum", "required": required, "options": options}


def p_num(name, typ, lo, hi, required=True):
    d = {"name": name, "type": typ, "required": required, "minimum": lo, "maximum": hi}
    if typ == "int":
        d["step"] = 1
    return d


def p_bool(name, required=True):
    return {"name": name, "type": "bool", "required": required}


def cmd(command_id, label, params=(), effect="momentary", group=None):
    """effect: "momentary", "hold" for commands that leave state held
    (key_down), or "wait" (sequences only). group: Bluetooth picker group."""
    c = {"command_id": command_id, "label": label, "effect": effect, "params": list(params)}
    if group:
        c["group"] = group
    return c


# The `key` options (the All keys table), read from core's
# HUBBO_BLE_HID_KEYS so the two can't drift.
def hid_keys():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                        "core", "src", "integrations", "ble_hid.cpp")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    block = text[text.index("#define HUBBO_BLE_HID_KEYS"):]
    block = block[:block.index("\n\n")]
    return re.findall(r'X\("([A-Z0-9_]+)"', block)


HID_KEYS = hid_keys()


def ble_commands(pid):
    """A Bluetooth device's catalog: its profile's commands (remote), every
    named key (keys) and a raw consumer usage (custom)."""
    command = lambda: dict(p_enum("command", list(PROFILES[pid]["commands"])), label="Remote command")
    key = lambda: p_enum("key", HID_KEYS)
    usage = lambda: p_num("usage", "int", 1, 1023)
    return [  # core's BleHidCommand order
        cmd("press", "Press", [command(), dict(p_num("hold_ms", "int", 1, 10000, required=False), unit="ms")],
            group="remote"),
        cmd("press_down", "Press down", [command()], effect="hold", group="remote"),
        cmd("press_up", "Press up", [command()], group="remote"),
        cmd("key_tap", "Key tap", [key()], group="keys"),
        cmd("key_down", "Key down", [key()], effect="hold", group="keys"),
        cmd("key_up", "Key up", [key()], group="keys"),
        cmd("usage_tap", "Usage tap", [usage()], group="custom"),
        cmd("usage_down", "Usage down", [usage()], effect="hold", group="custom"),
        cmd("usage_up", "Usage up", [usage()], group="custom"),
        cmd("wait_connected", "Wait until connected",
            [dict(p_num("timeout_ms", "int", 1000, 60000, required=False), unit="ms", default=15000)],
            effect="wait"),
    ]


SYSTEM_COMMANDS = [
    cmd("wait", "Wait", [p_num("duration_ms", "int", 0, 60000)]),
    cmd("activate", "Switch activity", [{"name": "activity_id", "type": "activity_ref", "required": True}]),
    cmd("off", "Off"),
]

# Mirrors core/src/ha/capabilities.cpp. Order matters (it is the output order).
F = {
    "PAUSE": 1, "VOLUME_SET": 4, "VOLUME_MUTE": 8, "PREVIOUS_TRACK": 16,
    "NEXT_TRACK": 32, "TURN_ON": 128, "TURN_OFF": 256, "VOLUME_STEP": 1024,
    "SELECT_SOURCE": 2048, "STOP": 4096, "PLAY": 16384, "SHUFFLE_SET": 32768,
    "SELECT_SOUND_MODE": 65536, "REPEAT_SET": 262144,
}
LIGHT_TRANSITION = 32


def media_player_commands(features, attrs):
    has = lambda name: features & F[name] == F[name]
    out = []
    if has("TURN_ON"):
        out.append(cmd("media_player.turn_on", "Power on"))
    if has("TURN_OFF"):
        out.append(cmd("media_player.turn_off", "Power off"))
    if has("VOLUME_SET") or has("VOLUME_STEP"):
        out.append(cmd("media_player.volume_up", "Volume up"))
        out.append(cmd("media_player.volume_down", "Volume down"))
    if has("VOLUME_SET"):
        out.append(cmd("media_player.volume_set", "Set volume",
                       [p_num("level", "number", 0, 1)]))
    if has("VOLUME_MUTE"):
        out.append(cmd("media_player.volume_mute", "Mute",
                       [p_bool("muted")]))
        out.append(cmd("media_player.mute_toggle", "Toggle mute"))
    if has("PLAY") and has("PAUSE"):
        out.append(cmd("media_player.media_play_pause", "Play/pause"))
    if has("PLAY"):
        out.append(cmd("media_player.media_play", "Play"))
    if has("PAUSE"):
        out.append(cmd("media_player.media_pause", "Pause"))
    if has("STOP"):
        out.append(cmd("media_player.media_stop", "Stop"))
    if has("NEXT_TRACK"):
        out.append(cmd("media_player.media_next_track", "Next track"))
    if has("PREVIOUS_TRACK"):
        out.append(cmd("media_player.media_previous_track", "Previous track"))
    if has("SELECT_SOURCE") and attrs.get("source_list"):
        out.append(cmd("media_player.select_source", "Select source",
                       [p_enum("source", attrs["source_list"])]))
    if has("SELECT_SOUND_MODE") and attrs.get("sound_mode_list"):
        out.append(cmd("media_player.select_sound_mode", "Sound mode",
                       [p_enum("sound_mode", attrs["sound_mode_list"])]))
    if has("SHUFFLE_SET"):
        out.append(cmd("media_player.shuffle_set", "Shuffle", [p_bool("shuffle")]))
    if has("REPEAT_SET"):
        out.append(cmd("media_player.repeat_set", "Repeat",
                       [p_enum("repeat", ["off", "all", "one"])]))
    return out


def light_commands(features, attrs):
    modes = set(attrs.get("supported_color_modes") or [])
    # HA's COLOR_MODES_BRIGHTNESS (an allowlist, as in capabilities.cpp).
    brightness = bool(modes & {"brightness", "color_temp", "hs", "xy", "rgb", "rgbw", "rgbww", "white"})
    color_temp = "color_temp" in modes
    transition = features & LIGHT_TRANSITION == LIGHT_TRANSITION
    tr = lambda: p_num("transition", "number", 0, 300, required=False)
    on = []
    if brightness:
        on.append(p_num("brightness", "int", 0, 255, required=False))
    if color_temp:
        lo = attrs.get("min_color_temp_kelvin", 0)
        hi = attrs.get("max_color_temp_kelvin", 0)
        if lo <= 0 or hi <= 0 or lo > hi:  # HA's defaults, as in capabilities.cpp
            lo, hi = 2000, 6535
        on.append(p_num("color_temp_kelvin", "int", lo, hi, required=False))
    if transition:
        on.append(tr())
    out = [cmd("light.turn_on", "Turn on", on)]
    out.append(cmd("light.turn_off", "Turn off", [tr()] if transition else []))
    if brightness:
        step = [p_num("step", "int", -255, 255)]
        if transition:
            step.append(tr())
        out.append(cmd("light.brightness_step", "Change brightness", step))
    return out


def switch_commands():
    return [cmd("switch.turn_on", "Turn on"), cmd("switch.turn_off", "Turn off")]


COVER = {"OPEN": 1, "CLOSE": 2, "SET_POSITION": 4, "STOP": 8,
         "OPEN_TILT": 16, "CLOSE_TILT": 32, "STOP_TILT": 64, "SET_TILT_POSITION": 128}
HUMIDIFIER_MODES = 1


def percent(name, lo=0, hi=100):
    return dict(p_num(name, "int", lo, hi), unit="%")


def cover_commands(features):
    has = lambda *names: all(features & COVER[n] for n in names)
    out = []
    if has("OPEN"):
        out.append(cmd("cover.open_cover", "Open"))
    if has("CLOSE"):
        out.append(cmd("cover.close_cover", "Close"))
    if has("STOP"):
        out.append(cmd("cover.stop_cover", "Stop"))
    if has("OPEN", "CLOSE"):
        out.append(cmd("cover.toggle", "Toggle"))
    if has("SET_POSITION"):
        out.append(cmd("cover.set_cover_position", "Set position", [percent("position")]))
        out.append(cmd("cover.position_step", "Change position", [percent("step", -100, 100)]))
    if has("OPEN_TILT"):
        out.append(cmd("cover.open_cover_tilt", "Open tilt"))
    if has("CLOSE_TILT"):
        out.append(cmd("cover.close_cover_tilt", "Close tilt"))
    if has("STOP_TILT"):
        out.append(cmd("cover.stop_cover_tilt", "Stop tilt"))
    if has("SET_TILT_POSITION"):
        out.append(cmd("cover.set_cover_tilt_position", "Set tilt", [percent("tilt_position")]))
    return out


def humidifier_commands(features, attrs):
    lo, hi = attrs.get("min_humidity", 0), attrs.get("max_humidity", 100)
    out = [cmd("humidifier.turn_on", "Turn on"), cmd("humidifier.turn_off", "Turn off"),
           cmd("humidifier.toggle", "Toggle"), cmd("humidifier.set_humidity", "Set humidity", [percent("humidity", lo, hi)])]
    if features & HUMIDIFIER_MODES and attrs.get("available_modes"):
        out.append(cmd("humidifier.set_mode", "Set mode", [p_enum("mode", attrs["available_modes"])]))
    return out


def select_commands(attrs):
    if not attrs.get("options"):
        return []
    return [cmd("select.select_option", "Choose", [p_enum("option", attrs["options"])]),
            cmd("select.select_next", "Next"), cmd("select.select_previous", "Previous")]


def suggested_kind(domain, platform, device_class):
    """Mirrors ha::suggestedKind (core/src/ha/capabilities.cpp)."""
    fixed = {"light": "light", "switch": "switch", "fan": "fan", "scene": "scene", "script": "script",
             "cover": "cover", "humidifier": "climate"}
    if domain != "media_player":
        return fixed.get(domain, "other")
    if device_class == "receiver":
        return "receiver"
    if device_class == "speaker" or platform in ("alexa_media", "group"):
        return "speaker"
    return "streamer" if platform == "apple_tv" else "tv"


# ---------------------------------------------------------------- devices

HA_ENTITIES = {
    "dev_denon": {
        "entity_id": "media_player.family_room_denon", "unique_id": "c5a1e0b2f3d44c1aa1b2c3d4e5f60718",
        "name": "Family Room Denon", "kind": "receiver", "domain": "media_player", "state": "off",
        "device_class": "receiver",
        "supported_features": 69004,  # denonavr SUPPORT_DENON | SELECT_SOUND_MODE
        "attributes": {
            "source_list": ["Blu-ray", "CBL/SAT", "DVD", "Game", "Media Player", "TV Audio", "Tuner", "Bluetooth"],
            "sound_mode_list": ["MUSIC", "MOVIE", "GAME", "AUTO", "STEREO", "DIRECT"],
        },
    },
    "dev_sonos": {
        "entity_id": "media_player.family_room_sonos", "unique_id": "RINCON_48A6B8000000001400",
        "name": "Family Room Sonos", "kind": "speaker", "domain": "media_player", "state": "idle",
        "device_class": "speaker",
        "revision": 3,  # renamed twice
        "supported_features": 8321599,  # sonos _attr_supported_features | SELECT_SOURCE (Arc)
        "attributes": {"source_list": ["TV"]},
    },
    "dev_lg_tv": {
        "entity_id": "media_player.family_room_lg_tv", "unique_id": "1b2e6c3a-9f4d-4a7e-b1c2-d3e4f5a6b7c8",
        "name": "Family Room LG TV", "kind": "tv", "domain": "media_player", "state": "off",
        "device_class": "tv",
        # webostv SUPPORT_WEBOSTV | MUTE | VOLUME_STEP (external speaker) | TURN_ON (turn-on trigger)
        "supported_features": 24505,
        "attributes": {"source_list": ["HDMI 1", "HDMI 2", "HDMI 3", "Live TV", "Netflix", "YouTube"]},
        # webostv's services take free text, so they're custom services (a
        # Home Assistant action), with HA's own hints (webostv services.yaml
        # and strings.json). hub-sim's fake HA reports the same.
        "platform": "webostv",
        "custom_services": [
            {"service": "webostv.button", "label": "Button", "fields": [
                {"name": "button", "required": True, "example": "LEFT",
                 # HA's text, cut to 160 bytes the hub's way (a word boundary, then "…")
                 "description": "Name of the button to press. Known possible values are LEFT, RIGHT, DOWN, UP, "
                                "HOME, MENU, BACK, ENTER, DASH, INFO, ASTERISK, CC, EXIT, MUTE, RED, …"}]},
            {"service": "webostv.command", "label": "Command", "fields": [
                {"name": "command", "required": True, "example": "system.launcher/open",
                 "description": "Endpoint for the command."},
                {"name": "payload", "required": False, "example": "target: https://www.google.com\n",
                 "description": "An optional payload to provide to the endpoint in the format of key value pair(s)."}]},
            {"service": "webostv.select_sound_output", "label": "Select sound output", "fields": [
                {"name": "sound_output", "required": True, "example": "external_speaker",
                 "description": "Name of the sound output to switch to."}]},
        ],
    },
    "dev_xbox": {
        "entity_id": "media_player.xbox", "unique_id": "FD00112233445566",
        "name": "Xbox", "kind": "console", "domain": "media_player", "state": "off",
        "supported_features": 149433,  # xbox SUPPORT_XBOX
        "attributes": {},
    },
    "dev_family_room_lights": {
        "entity_id": "light.family_room", "unique_id": "7d3e2a1b-4c5d-4e6f-8a9b-0c1d2e3f4a5b",
        "name": "Family Room Lights", "kind": "light", "domain": "light", "state": "on",
        "supported_features": 44,  # hue v2: EFFECT | FLASH | TRANSITION
        "attributes": {"supported_color_modes": ["color_temp", "xy"],
                       "min_color_temp_kelvin": 2000, "max_color_temp_kelvin": 6535},
    },
}


# HA's remote services (remote/services.yaml, labels and descriptions from
# remote/strings.json, cut to 160 bytes the hub's way) as a remote's custom
# services: send_command first, then by id. hub-sim's fake HA serves the same get_services.
REMOTE_CUSTOM_SERVICES = [
    {"service": "remote.send_command", "label": "Send remote command", "fields": [
        {"name": "device", "required": False, "example": "32756745", "description": "Device ID to send command to."},
        {"name": "command", "required": True, "example": "Play",
         "description": "A single command or a list of commands to send."},
        {"name": "num_repeats", "required": False,
         "description": "The number of times you want to repeat the commands."},
        {"name": "delay_secs", "required": False,
         "description": "The time you want to wait in between repeated commands."},
        {"name": "hold_secs", "required": False,
         "description": "The time you want to have it held before the release is send."}]},
    {"service": "remote.delete_command", "label": "Delete remote command", "fields": [
        {"name": "device", "required": False, "example": "television",
         "description": "Device from which commands will be deleted."},
        {"name": "command", "required": True, "example": "Mute",
         "description": "The single command or the list of commands to be deleted."}]},
    {"service": "remote.learn_command", "label": "Learn remote command", "fields": [
        {"name": "device", "required": False, "example": "television",
         "description": "Device ID to learn command from."},
        {"name": "command", "required": False, "example": "Turn on",
         "description": "A single command or a list of commands to learn."},
        {"name": "command_type", "required": False, "description": "The type of command to be learned."},
        {"name": "alternative", "required": False,
         "description": "If code must be stored as an alternative. This is useful for discrete codes. Discrete "
                        "codes are used for toggles that only perform one function. For example…"},
        {"name": "timeout", "required": False, "description": "Timeout for the command to be learned."}]},
]


# Made-up entities of the newer domains (never a real home's names): discovery
# candidates, not registered, each with the capabilities registering it
# gives (integrations/ha_main.entities/<entity_id>.capabilities.json, for
# the web mock). Platform "demo"; humidifier MODES = 1, cover 15 = OPEN |
# CLOSE | SET_POSITION | STOP.
NEW_DOMAIN_ENTITIES = [
    {"entity_id": "scene.movie_night", "name": "Movie Night", "domain": "scene", "state": "unknown",
     "supported_features": 0, "attributes": {}},
    {"entity_id": "script.goodnight", "name": "Goodnight", "domain": "script", "state": "off",
     "supported_features": 0, "attributes": {}},
    {"entity_id": "cover.office_blinds", "name": "Office Blinds", "domain": "cover", "state": "open",
     "device_class": "blind", "supported_features": 15, "attributes": {}},
    {"entity_id": "button.restart_router", "name": "Restart Router", "domain": "button", "state": "unknown",
     "device_class": "restart", "supported_features": 0, "attributes": {}},
    {"entity_id": "input_boolean.guest_mode", "name": "Guest Mode", "domain": "input_boolean", "state": "off",
     "supported_features": 0, "attributes": {}},
    {"entity_id": "humidifier.bedroom_humidifier", "name": "Bedroom Humidifier", "domain": "humidifier",
     "state": "on", "device_class": "humidifier", "supported_features": 1,
     "attributes": {"available_modes": ["normal", "eco", "sleep"], "min_humidity": 30, "max_humidity": 80}},
    {"entity_id": "select.dryer_cycle", "name": "Dryer Cycle", "domain": "select", "state": "normal",
     "supported_features": 0, "attributes": {"options": ["normal", "delicate", "towels"]}},
    # A remote: HA's own remote services beyond the allowlist are custom
    # services (a Home Assistant action); "demo" adds none of its own.
    {"entity_id": "remote.living_room_tv", "name": "Living Room TV", "domain": "remote", "state": "on",
     "supported_features": 0, "attributes": {}, "custom_services": REMOTE_CUSTOM_SERVICES},
]


def ha_provider(entity_id, unique_id=None):
    p = {"type": "home_assistant", "entity_id": entity_id}
    if unique_id:
        p["unique_id"] = unique_id
    return p


def ha_device(dev_id, e):
    return {
        "id": dev_id, "name": e["name"], "integration_id": "ha_main", "kind": e["kind"],
        "provider": ha_provider(e["entity_id"], e["unique_id"]), "platform": "home_assistant",
        "availability": "available", "schema_version": 1, "revision": e.get("revision", 1),
    }


def ha_snapshot(e):
    """The stored `ha_capabilities` (backups only; never in a Device response)."""
    snap = {"platform": e.get("platform", "demo"), "supported_features": e["supported_features"]}
    for key in ("source_list", "sound_mode_list", "supported_color_modes",
                "min_color_temp_kelvin", "max_color_temp_kelvin", "min_humidity", "max_humidity",
                "available_modes", "options"):
        if key in e["attributes"]:
            snap[key] = e["attributes"][key]
    snap["services"] = []
    if e.get("custom_services"):
        snap["custom_services"] = e["custom_services"]
    return snap


def stored_device(d):
    """A Device as a backup stores it: no runtime state; HA devices carry their snapshot."""
    out = {k: x for k, x in d.items() if k not in ("availability", "bluetooth", "platform")}
    entity = HA_ENTITIES.get(d["id"])
    if entity is not None:
        out["ha_capabilities"] = ha_snapshot(entity)
    return out


def ha_commands(e):
    if e["domain"] == "media_player":
        return media_player_commands(e["supported_features"], e["attributes"])
    if e["domain"] == "light":
        return light_commands(e["supported_features"], e["attributes"])
    if e["domain"] == "switch":
        return switch_commands()
    simple = {"scene": [cmd("scene.turn_on", "Activate")],
              "script": [cmd("script.turn_on", "Run"), cmd("script.turn_off", "Stop")],
              "button": [cmd("button.press", "Press")],
              "input_boolean": [cmd("input_boolean.turn_on", "Turn on"), cmd("input_boolean.turn_off", "Turn off"),
                                cmd("input_boolean.toggle", "Toggle")]}
    if e["domain"] in simple:
        return simple[e["domain"]]
    if e["domain"] == "cover":
        return cover_commands(e["supported_features"])
    if e["domain"] == "humidifier":
        return humidifier_commands(e["supported_features"], e["attributes"])
    if e["domain"] == "select":
        return select_commands(e["attributes"])
    if e["domain"] == "remote":  # turn_on's activity is text: untyped, so no param
        return [cmd("remote.turn_on", "Turn on"), cmd("remote.turn_off", "Turn off"), cmd("remote.toggle", "Toggle")]
    return []


def device_commands(e):
    """A registered entity's commands: its own, then a Home Assistant action
    if it has custom services."""
    out = ha_commands(e)
    if e.get("custom_services"):
        out.append(dict(cmd("call_service", "Home Assistant action", group="custom"), services=e["custom_services"]))
    return out


# Detail form (GET /devices/{id}); `light()` gives the list form.
DEVICES = [
    profile_device("dev_apple_tv", "hubbo:apple-tv", "Family Room Apple TV"),
] + [ha_device(k, v) for k, v in HA_ENTITIES.items()]

CAPABILITIES = {"dev_apple_tv": {"device_id": "dev_apple_tv", "integration_id": "ble",
                                 "commands": ble_commands("hubbo:apple-tv")}}
for k, v in HA_ENTITIES.items():
    CAPABILITIES[k] = {"device_id": k, "integration_id": "ha_main", "commands": device_commands(v)}

# ---------------------------------------------------------------- activities

def a(device, command_id, /, **params):
    integ = "system" if device is None else next(d["integration_id"] for d in DEVICES if d["id"] == device)
    return {"integration_id": integ, "device_id": device, "command_id": command_id, "params": params}


def ha_action(device, service, **data):
    """A Home Assistant action: `service` with `data`, sent verbatim to the device's entity."""
    return dict(a(device, "call_service"), call={"service": service, "data": data})


def wait(ms):
    return a(None, "wait", duration_ms=ms)


# Buttons as the web admin writes them (docs/agent-api.md "Buttons"): a Press passes through
# (Bluetooth as a down/up pair, an HA command on down); with a Long press, Press is the tap;
# Hubbo's own (activate, off) on tap.
def passthrough(button, family, device="dev_apple_tv", **params):
    return {f"{button}:down": [a(device, f"{family}_down", **params)],
            f"{button}:up": [a(device, f"{family}_up", **params)]}


watch_bindings = {}
for b, k in [("up", "DirectionUp"), ("down", "DirectionDown"), ("left", "DirectionLeft"),
             ("right", "DirectionRight"), ("select", "Select"), ("back", "Back"), ("exit", "Back"),
             ("skip_forward", "NextTrack"), ("skip_backward", "PreviousTrack"), ("play", "PlayPause"),
             ("pause", "PlayPause")]:
    watch_bindings.update(passthrough(b, "press", command=k))
watch_bindings.update(passthrough("record", "usage", usage=0xB2))
watch_bindings.update(passthrough("blue", "key", key="F1"))
watch_bindings.update({
    "menu:tap": [a("dev_apple_tv", "press", command="Home")],
    "menu:hold": [a("dev_apple_tv", "press", command="ControlCenter")],
    "info:hold": [a("dev_apple_tv", "key_tap", key="KEY_I")],
    "volume_up:down": [a("dev_sonos", "media_player.volume_up")],
    "volume_down:down": [a("dev_sonos", "media_player.volume_down")],
    "mute:down": [a("dev_sonos", "media_player.mute_toggle")],
    "plus:down": [a("dev_family_room_lights", "light.brightness_step", step=32)],
    "minus:down": [a("dev_family_room_lights", "light.brightness_step", step=-32)],
    "light_1:down": [a("dev_family_room_lights", "light.turn_on", brightness=255, transition=1)],
    "light_2:down": [a("dev_family_room_lights", "light.turn_off", transition=1)],
    # A macro (listed in `macros`): a Press with a wait (no Long press, so on down).
    "guide:down": [a("dev_apple_tv", "press", command="Home"), wait(500),
                   a("dev_apple_tv", "press", command="DirectionDown")],
})

WATCH = {
    "id": "act_watch_apple_tv", "name": "Watch Apple TV",
    "startup": [
        a("dev_lg_tv", "media_player.turn_on"),
        a("dev_sonos", "media_player.select_source", source="TV"),
        a("dev_apple_tv", "wait_connected", timeout_ms=15000),
        a("dev_apple_tv", "press", command="Home"),
        wait(1500),
        a("dev_lg_tv", "media_player.select_source", source="HDMI 1"),
        a("dev_family_room_lights", "light.turn_on", brightness=64, color_temp_kelvin=2700, transition=2),
    ],
    "shutdown": [
        a("dev_apple_tv", "press", command="Home", hold_ms=2000),
        a("dev_apple_tv", "press", command="Select"),
        a("dev_lg_tv", "media_player.turn_off"),
        a("dev_sonos", "media_player.media_pause"),
    ],
    "bindings": watch_bindings, "macros": ["guide:down"], "schema_version": 1, "revision": 4,
}

PLAY_XBOX = {
    "id": "act_play_xbox", "name": "Play Xbox",
    "startup": [
        a("dev_xbox", "media_player.turn_on"),
        a("dev_lg_tv", "media_player.turn_on"),
        a("dev_denon", "media_player.turn_on"),
        wait(2000),
        a("dev_lg_tv", "media_player.select_source", source="HDMI 2"),
        a("dev_denon", "media_player.select_source", source="Game"),
        a("dev_denon", "media_player.select_sound_mode", sound_mode="GAME"),
    ],
    "shutdown": [
        a("dev_xbox", "media_player.turn_off"),
        a("dev_lg_tv", "media_player.turn_off"),
        a("dev_denon", "media_player.turn_off"),
    ],
    "bindings": {
        "volume_up:down": [a("dev_denon", "media_player.volume_up")],
        "volume_down:down": [a("dev_denon", "media_player.volume_down")],
        "mute:down": [a("dev_denon", "media_player.mute_toggle")],
        "play:down": [a("dev_xbox", "media_player.media_play_pause")],
        "pause:down": [a("dev_xbox", "media_player.media_play_pause")],
        "skip_forward:down": [a("dev_xbox", "media_player.media_next_track")],
        "skip_backward:down": [a("dev_xbox", "media_player.media_previous_track")],
        "light_1:down": [a("dev_family_room_lights", "light.turn_on", brightness=255)],
        "light_2:down": [a("dev_family_room_lights", "light.turn_off")],
        "red:down": [a("dev_denon", "media_player.volume_set", level=0.35)],
        "info:down": [ha_action("dev_lg_tv", "webostv.button", button="INFO")],
    },
    "macros": [],
    "schema_version": 1, "revision": 2,
}

ACTIVITIES = [WATCH, PLAY_XBOX]

GLOBAL_BINDINGS = {
    "bindings": {
        "tv:tap": [a(None, "activate", activity_id="act_watch_apple_tv")],
        "movie:tap": [a(None, "activate", activity_id="act_watch_apple_tv")],
        "music:tap": [a(None, "activate", activity_id="act_play_xbox")],
        "off:tap": [a(None, "off")],
        "light_1:hold": [a("dev_family_room_lights", "light.turn_off")],
    },
    "macros": [],
    "revision": 6,
}


# ---------------------------------------------------------------- smart buttons

SLOTS = ("light_1", "light_2", "switch_1", "switch_2")


def press(tap=(), hold=(), release=()):
    return {"tap": list(tap), "hold": list(hold), "release": list(release)}


def no_actions():
    return {"button": press(), "plus": press(), "minus": press()}


def light_actions(dev):
    """A dimmable light's slot actions (core's smartSlotActions, the light
    row of docs/design/smart-buttons.md): on/off, and +/- step 10% of 255."""
    return {"button": press([a(dev, "light.turn_on")], [a(dev, "light.turn_off")]),
            "plus": press([a(dev, "light.brightness_step", step=26)]),
            "minus": press([a(dev, "light.brightness_step", step=-26)])}


EMPTY_SLOTS = {slot: None for slot in SLOTS}
# The only fixture devices put in a slot: the lights, and the LG TV (a kind
# with no actions). The contract checks the other kinds on hub-sim.
SLOT_ACTIONS = {None: no_actions(), "dev_family_room_lights": light_actions("dev_family_room_lights"),
                "dev_lg_tv": no_actions()}


def smart_buttons(slots, revision):
    """GET /smart-buttons for `slots`."""
    return {"slots": dict(slots), "actions": {slot: SLOT_ACTIONS[dev] for slot, dev in slots.items()},
            "revision": revision}


# Light 1 holds the lights; the others empty.
SMART_SLOTS = dict(EMPTY_SLOTS, light_1="dev_family_room_lights")
SMART_BUTTONS = smart_buttons(SMART_SLOTS, 7)
BACKUP_SMART_BUTTONS = {"slots": SMART_SLOTS, "revision": 7}


def input_of(act):
    return {k: copy.deepcopy(act[k]) for k in ("name", "startup", "shutdown", "bindings", "macros")}


# ---------------------------------------------------------------- misc

BUTTONS = [
    ("up", "keypad", "Up"), ("down", "keypad", "Down"), ("left", "keypad", "Left"),
    ("right", "keypad", "Right"), ("select", "keypad", "OK"), ("menu", "keypad", "Menu"),
] + [(f"number_{n}", "keypad", str(n)) for n in [1, 2, 3, 4, 5, 6, 7, 8, 9, 0]] + [
    ("number_dot_dash", "keypad", "-/."), ("number_enter", "keypad", "Enter"),
    ("off", "media_home", "Off"), ("music", "media_home", "Music"), ("tv", "media_home", "TV"),
    ("movie", "media_home", "Movie"), ("light_1", "media_home", "Light 1"),
    ("light_2", "media_home", "Light 2"), ("plus", "media_home", "+"), ("minus", "media_home", "−"),
    ("switch_1", "media_home", "Switch 1"), ("switch_2", "media_home", "Switch 2"),
    ("red", "media_home", "Red"), ("green", "media_home", "Green"), ("yellow", "media_home", "Yellow"),
    ("blue", "media_home", "Blue"), ("dvr", "media_home", "DVR"), ("guide", "media_home", "Guide"),
    ("info", "media_home", "Info"), ("back", "media_home", "Back"), ("exit", "media_home", "Exit"),
    ("volume_up", "media_home", "Vol +"), ("volume_down", "media_home", "Vol −"),
    ("mute", "media_home", "Mute"), ("channel_up", "media_home", "Ch +"),
    ("channel_down", "media_home", "Ch −"), ("skip_backward", "media_home", "Skip back"),
    ("skip_forward", "media_home", "Skip fwd"), ("play", "media_home", "Play"),
    ("pause", "media_home", "Pause"), ("record", "media_home", "Record"), ("stop", "media_home", "Stop"),
]

REMOTE_STATUS = {
    "paired": True, "channel": 74, "last_packet_age_ms": 5230, "held": [],
    "last_event": {"button": "volume_up", "kind": "up", "age_ms": 5230, "timed_out": False},
    "counters": {"bad_checksum": 2, "unknown_code": 0},
}

INTEGRATIONS = [
    {"id": "system", "type": "system", "name": "System", "enabled": True, "state": "connected",
     "device_count": 0, "commands": SYSTEM_COMMANDS},
    {"id": "ble", "type": "ble_hid", "name": "Bluetooth", "enabled": True, "state": "connected",
     "device_count": 1},
    {"id": "ha_main", "type": "home_assistant", "name": "Home Assistant", "enabled": True,
     "state": "connected", "detail": "Home Assistant 2026.11.1", "device_count": 5},
    {"id": "mqtt_main", "type": "mqtt", "name": "MQTT", "enabled": True, "state": "connected", "device_count": 0},
]

# ---------------------------------------------------------------- activity runs


def run(run_id, act, kind, outcome, started_ago_ms, results, button=None, trigger=None):
    """One run of `act`'s `kind` sequence (a macro: `button`'s `trigger`
    binding): `results` per action, in order, as (result, error, duration_ms)."""
    key = {None: None, "press": "down", "long_press": "hold"}[trigger]
    actions = act["bindings"][f"{button}:{key}"] if kind == "macro" else act[kind]
    assert len(results) == len(actions)
    steps = [{"index": i, "device_id": x["device_id"], "command_id": x["command_id"],
              "result": r, "error": e, "duration_ms": d} for i, (x, (r, e, d)) in enumerate(zip(actions, results))]
    done = [d for _, _, d in results if d is not None]
    return {"id": run_id, "activity_id": act["id"], "kind": kind, "button": button, "trigger": trigger,
            "started_ago_ms": started_ago_ms,
            "duration_ms": None if outcome == "running" else sum(done), "outcome": outcome, "steps": steps}


OK = ("ok", None, 40)
# Newest first: a Guide macro in Watch Apple TV (a wait between two presses);
# Watch Apple TV's startup skipped the TV (already on), got no reply from the
# Sonos, and went on past the lights failing; before it, Play Xbox's
# shutdown, and its startup, cut short by the switch.
ACTIVITY_RUNS = [
    run(13, WATCH, "macro", "completed", 30000, [("ok", None, 45), ("ok", None, 500), ("ok", None, 40)],
        button="guide", trigger="press"),
    run(12, WATCH, "startup", "completed_with_errors", 120000,
        [("skipped", "already_set", 25), ("no_reply", None, 1000), ("ok", None, 2100), OK, ("ok", None, 1500), OK,
         ("failed", "entity_not_found", 30)]),
    run(11, PLAY_XBOX, "shutdown", "completed", 125000, [OK, OK, OK]),
    run(10, PLAY_XBOX, "startup", "cancelled", 3600000,
        [OK, ("failed", "entity_not_found", 30), OK, ("ok", None, 2000), OK,
         ("cancelled", "cancelled", None), ("cancelled", "cancelled", None)]),
]


def last_run(r):
    return {"id": r["id"], "activity_id": r["activity_id"], "kind": r["kind"], "button": r["button"],
            "trigger": r["trigger"], "outcome": r["outcome"],
            "failed_steps": sum(s["result"] in ("failed", "timed_out") for s in r["steps"])}  # not skipped/no_reply


HUB = {"name": "Living Room", "hostname": "living-room", "hostname_conflict": False, "check_updates": True,
       "radio_power": "max"}
STATUS = {
    "hubbo": {"name": HUB["name"], "version": "0.1.0", "build": "0.1.0+g1a2b3c4",
              "board": "waveshare_esp32_s3_eth"},
    "uptime_s": 86400, "time": "2026-11-08T20:15:00Z", "auth_mode": "enforced",
    "auth_state": "ok", "config": {"state": "ok", "generation": 42},
    "active_activity_id": "act_watch_apple_tv",
    "selected_slot": "light_1",
    "last_run": last_run(ACTIVITY_RUNS[0]),
    "network": {"link": "up", "ip": "192.0.2.56", "hostname": HUB["hostname"]},
    "remote": REMOTE_STATUS,
    "integrations": [{k: i[k] for k in ("id", "type", "state", "detail") if k in i} for i in INTEGRATIONS],
    "diagnostics": {
        "heap_free": 182340, "heap_min_free": 141020, "psram_free": 7912448, "queue_overflows": 0,
        "dispatch_latency": {"count": 1532, "p50_ms": 0.5, "p95_ms": 0.65, "p99_ms": 0.8},
        "last_errors": [{"at": "2026-11-08T19:02:11Z", "source": "ha_main", "code": "timeout",
                         "message": "media_player.volume_up on dev_sonos timed out after 2000 ms"}],
    },
}


def err(code, message, details=None, **extra):
    e = {"code": code, "message": message}
    if details:
        e["details"] = [{"path": p, "code": c, "message": m} for p, c, m in details]
    e.update(extra)
    return {"error": e}


TOKEN_SECRET = "hbt_k3x9q2mf_" + "Zq7Lr2Vb9Xc4Nt6Hs8Jd1Kf3Gm5Pw0Ya7Ue2Ri4To9A"
assert len(TOKEN_SECRET.split("_", 2)[2]) == 43

GUIDE = "/api/v1/agent.md"  # the agent guide, linked from the index and error hints

# (file, operation, kind, content). kind: HTTP status as string, or "request".
FIXTURES = []


def fx(name, op, kind, content):
    FIXTURES.append((name, op, kind, content))


fx("discovery/index.json", "getApiIndex", "200", {
    "name": "Hubbo API", "version": STATUS["hubbo"]["version"],
    "about": "Read docs (the agent guide) first: it covers everyday tasks. openapi is the full reference "
             "(large): use it to look up an exact schema or error.",
    "docs": GUIDE,
    "openapi": "/api/v1/openapi.json",
    "auth": "Authorization: Bearer <token>. An admin creates tokens in Settings → API tokens."})
fx("status.json", "getStatus", "200", STATUS)
fx("status.blocked.json", "getStatus", "200",
   dict(copy.deepcopy(STATUS), active_activity_id=None, last_run=None,
        config={"state": "blocked", "generation": 57,
                "reason": "config.json schema_version 2 is newer than this firmware (1)"}))
fx("status.simulator.json", "getStatus", "200",
   dict(copy.deepcopy(STATUS), auth_mode="simulator_no_auth",
        hubbo={"name": "Hubbo 534D", "version": "0.1.0", "build": "sim", "board": "hub-sim"},
        network=dict(STATUS["network"], hostname="hubbo-534d")))
fx("activity_runs/list.json", "listActivityRuns", "200", {"runs": ACTIVITY_RUNS})
fx("remote/status.json", "getRemoteStatus", "200", REMOTE_STATUS)
fx("remote/log.json", "getRemoteLog", "200", {"span_ms": 300000, "dropped": 0, "entries": [
    {"type": "packet", "age_ms": 4210, "button": "volume_up", "kind": "down", "signal": "strong"},
    {"type": "event", "age_ms": 4209, "button": "volume_up", "kind": "down", "timed_out": False},
    {"type": "action", "age_ms": 4209, "trigger": "volume_up:down", "binding": "command", "integration_id": "ble",
     "device_id": "dev_apple_tv", "command_id": "press", "result": "ok", "error": None, "duration_ms": 12},
    {"type": "packet", "age_ms": 4100, "button": "volume_up", "kind": "up", "signal": "weak"},
    {"type": "event", "age_ms": 4100, "button": "volume_up", "kind": "up", "timed_out": False},
    {"type": "anomaly", "age_ms": 2050, "anomaly": "rf_stall", "stall_ms": 9},
    {"type": "packet", "age_ms": 1500, "button": "guide", "kind": "down", "signal": "strong"},
    {"type": "event", "age_ms": 1500, "button": "guide", "kind": "down", "timed_out": False},
    {"type": "event", "age_ms": 1000, "button": "guide", "kind": "up", "timed_out": True},
    {"type": "event", "age_ms": 1000, "button": "guide", "kind": "tap", "timed_out": False},
    {"type": "action", "age_ms": 1000, "trigger": "guide:tap", "binding": "macro", "integration_id": "system",
     "device_id": None, "command_id": "wait", "result": "ok", "error": None, "duration_ms": 300},
]})
fx("remote/buttons.json", "listRemoteButtons", "200", {
    "buttons": [{"name": n, "group": g, "label": l} for n, g, l in BUTTONS],
    "event_kinds": ["down", "up", "tap", "hold"], "hold_ms": 800})

fx("auth/login.request.json", "login", "request", {"username": "hubbo-owner", "password": "correct horse battery"})
fx("auth/setup.request.json", "setupAdmin", "request", {"username": "hubbo-owner", "password": "correct horse battery"})
fx("auth/session.admin.json", "getSession", "200", {
    "authenticated": True, "role": "admin", "username": "hubbo-owner",
    "expires_in_s": 604800,
    "setup_required": False, "auth_state": "ok", "config_state": "ok", "auth_mode": "enforced"})
fx("auth/session.anonymous.json", "getSession", "200", {
    "authenticated": False, "role": None, "setup_required": False, "auth_state": "ok", "config_state": "ok", "auth_mode": "enforced"})
fx("auth/session.setup_required.json", "getSession", "200", {
    "authenticated": False, "role": None, "setup_required": True, "auth_state": "fresh", "auth_mode": "enforced"})
fx("auth/session.storage_unavailable.json", "getSession", "200", {
    "authenticated": False, "role": None, "setup_required": False, "auth_state": "unavailable",
    "config_state": "storage_unavailable", "auth_mode": "enforced"})
fx("auth/session.token.json", "getSession", "200", {
    "authenticated": True, "role": "token", "setup_required": False, "auth_state": "ok", "config_state": "ok", "auth_mode": "enforced"})
fx("auth/session.simulator.json", "getSession", "200", {
    "authenticated": True, "role": "admin", "username": "simulator",
    "setup_required": False, "auth_state": "ok", "config_state": "ok", "auth_mode": "simulator_no_auth"})

fx("devices/list.json", "listDevices", "200", {"devices": [light(d) for d in DEVICES]})
for d in DEVICES:
    fx(f"devices/{d['id']}.json", "getDevice", "200", d)
    fx(f"devices/{d['id']}.capabilities.json", "getDeviceCapabilities", "200", CAPABILITIES[d["id"]])
fx("devices/execute.volume_set.request.json", "executeCommand", "request", {"params": {"level": 0.35}})
fx("devices/execute.accepted.json", "executeCommand", "200", {
    "status": "accepted", "device_id": "dev_denon", "command_id": "media_player.volume_set", "elapsed_ms": 84})
fx("devices/execute.timeout.json", "executeCommand", "200", {
    "status": "timeout", "device_id": "dev_sonos", "command_id": "media_player.volume_up", "elapsed_ms": 2000,
    "message": "Home Assistant did not answer within 2000 ms"})
fx("devices/execute.failed.json", "executeCommand", "200", {
    "status": "failed", "device_id": "dev_apple_tv", "command_id": "press", "elapsed_ms": 1,
    "message": "adapter_unavailable"})
fx("devices/execute.press.request.json", "executeCommand", "request", {"params": {"command": "Select"}})
fx("errors/sequence_only.json", "executeCommand", "422", err(
    "validation_failed", "wait_connected only works in a startup, shutdown or macro", [
        ("/command_id", "sequence_only", "wait_connected only works in a startup, shutdown or macro"),
    ]))

fx("profiles/list.json", "listProfiles", "200", {"profiles": [profile_summary(p) for p in PROFILES]})
for pid, doc in PROFILES.items():
    fx(f"profiles/{pid.split(':', 1)[1]}.json", "getProfile", "200", doc)
fx("profiles/kinds.json", "listKinds", "200", {"kinds": KINDS})

fx("activities/list.json", "listActivities", "200",
   {"activities": ACTIVITIES, "active_activity_id": "act_watch_apple_tv"})
for act in ACTIVITIES:
    fx(f"activities/{act['id']}.json", "getActivity", "200", act)
create = input_of(PLAY_XBOX)
create["name"] = "Play Xbox (copy)"
fx("activities/create.request.json", "createActivity", "request", create)
replace = copy.deepcopy(WATCH)
replace["bindings"].update(passthrough("green", "key", key="F3"))
fx("activities/replace.request.json", "replaceActivity", "request", replace)
fx("activities/global_bindings.json", "getGlobalBindings", "200", GLOBAL_BINDINGS)
# Trying Watch Apple TV's Guide macro from the editor (as on screen, saved or not).
fx("activities/macro_run.request.json", "runMacro", "request",
   {"actions": WATCH["bindings"]["guide:down"], "trigger": "guide:down", "activity_id": WATCH["id"]})
fx("activities/macro_run.json", "runMacro", "202", {"run_id": 14})
fx("smart_buttons/get.json", "getSmartButtons", "200", SMART_BUTTONS)
# Switch 2 gets the LG TV: a kind with no smart actions.
REPLACED_SLOTS = dict(SMART_SLOTS, switch_2="dev_lg_tv")
fx("smart_buttons/replace.request.json", "replaceSmartButtons", "request",
   {"slots": REPLACED_SLOTS, "revision": SMART_BUTTONS["revision"]})
fx("smart_buttons/replace.json", "replaceSmartButtons", "200", smart_buttons(REPLACED_SLOTS, 8))
fx("smart_buttons/actions.dev_family_room_lights.json", "getSmartButtonActions", "200",
   SLOT_ACTIONS["dev_family_room_lights"])
fx("smart_buttons/actions.dev_lg_tv.json", "getSmartButtonActions", "200", SLOT_ACTIONS["dev_lg_tv"])
fx("errors/smart_buttons_revision_conflict.json", "replaceSmartButtons", "409",
   err("revision_conflict", "Changed since the revision you sent; GET it again and reapply", current_revision=8))
fx("errors/smart_buttons_unknown_device.json", "replaceSmartButtons", "422", err(
    "validation_failed", "The smart buttons are invalid", [
        ("/slots/light_2", "unknown_device", "No device with that id")]))
fx("activities/validate.valid.json", "validateActivity", "200", {"valid": True, "errors": []})
fx("activities/validate.invalid.json", "validateActivity", "200", {"valid": False, "errors": [
    {"path": "/bindings/volume_up:down/0/params/level", "code": "param_out_of_range",
     "message": "level must be between 0 and 1"},
    {"path": "/startup/2/device_id", "code": "unknown_device", "message": "dev_projector is not registered"},
    {"path": "/bindings/play:down/0/integration_id", "code": "integration_mismatch",
     "message": "dev_apple_tv belongs to integration ble, not ha_main"},
]})
NO_PHASE = {"status": "none", "queued_actions": 0}
fx("activities/activate.json", "activateActivity", "200", {
    "switched": True, "active_activity_id": "act_play_xbox", "previous_activity_id": "act_watch_apple_tv",
    "generation": 17, "startup": {"status": "queued", "queued_actions": len(PLAY_XBOX["startup"])},
    "shutdown": NO_PHASE})
fx("activities/activate.queue_full.json", "activateActivity", "200", {
    "switched": True, "active_activity_id": "act_play_xbox", "previous_activity_id": "act_watch_apple_tv",
    "generation": 19, "startup": {"status": "rejected", "queued_actions": 0, "reason": "queue_full"},
    "shutdown": NO_PHASE})
fx("activities/off.json", "activitiesOff", "200", {
    "switched": True, "active_activity_id": None, "previous_activity_id": "act_watch_apple_tv",
    "generation": 18, "startup": NO_PHASE,
    "shutdown": {"status": "queued", "queued_actions": len(WATCH["shutdown"])}})

fx("integrations/list.json", "listIntegrations", "200", {"integrations": INTEGRATIONS})
fx("integrations/ha_main.config.json", "getIntegrationConfig", "200", {
    "id": "ha_main", "type": "home_assistant", "name": "Home Assistant", "enabled": True,
    "settings": {"type": "home_assistant", "url": "https://homeassistant.local:8123",
                 "verify_tls": True, "has_access_token": True},
    "revision": 2})
fx("integrations/ble.config.json", "getIntegrationConfig", "200", {
    "id": "ble", "type": "ble_hid", "name": "Bluetooth", "enabled": True,
    "settings": {"type": "ble_hid", "device_name": "Family Room Hubbo"},
    "revision": 1})
fx("integrations/mqtt_main.config.json", "getIntegrationConfig", "200", {
    "id": "mqtt_main", "type": "mqtt", "name": "MQTT", "enabled": True,
    "settings": {"type": "mqtt", "host": "192.0.2.10", "port": 1883, "username": "hubbo",
                 "has_password": True},
    "revision": 3})
fx("integrations/mqtt_main.configure.request.json", "configureIntegration", "request", {
    "enabled": True,
    "settings": {"type": "mqtt", "host": "192.0.2.10", "port": 1883, "username": "hubbo",
                 "password": "example-not-a-real-password"},
    "revision": 3})
fx("integrations/ha_main.configure.request.json", "configureIntegration", "request", {
    "settings": {"type": "home_assistant", "url": "https://homeassistant.local:8123",
                 "verify_tls": True, "access_token": "eyJhbGciOiJIUzI1NiJ9.example-not-a-real-token"},
    "revision": 2})
# Discovery candidates: domain commands only (a preview), no unique_id; the
# details add the integration's services and the unique_id.
def candidate(entity_id, name, domain, platform, commands, registered=None, state=None, device_class=""):
    """A discovery candidate; platform None for a YAML-only entity (no registry entry)."""
    c = {"provider": ha_provider(entity_id), "name": name, "domain": domain}
    if platform is not None:
        c["platform"] = platform
    if state is not None:
        c["state"] = state
    c.update({"supported": bool(commands), "command_ids": [x["command_id"] for x in commands],
              "dropped_options": 0, "registered_device_id": registered,
              "suggested_kind": suggested_kind(domain, platform or "", device_class)})
    return c


cands = [candidate(e["entity_id"], e["name"], e["domain"], e.get("platform", "demo"), ha_commands(e), dev_id,
                   device_class=e.get("device_class", "")) for dev_id, e in HA_ENTITIES.items()]
for e in NEW_DOMAIN_ENTITIES:
    cands.append(candidate(e["entity_id"], e["name"], e["domain"], "demo", ha_commands(e), state=e["state"],
                           device_class=e.get("device_class", "")))
    fx(f"integrations/ha_main.entities/{e['entity_id']}.capabilities.json", "getDeviceCapabilities", "200",
       {"device_id": "dev_" + e["entity_id"].split(".", 1)[1], "integration_id": "ha_main",
        "commands": device_commands(e)})
cands.append(candidate("switch.tv_backlight", "TV Backlight", "switch", "zha", switch_commands()))
cands.append(candidate("switch.porch_light", "Porch Light", "switch", None, switch_commands()))  # YAML-only
cands.append(candidate("media_player.kitchen_display", "Kitchen Display", "media_player", "cast", []))
fx("integrations/ha_main.discover.json", "discoverIntegration", "202", {"state": "running"})
fx("integrations/ha_main.discovery.json", "getDiscovery", "200",
   {"state": "done", "total": len(cands), "truncated": 0, "elapsed_ms": 1840, "candidates": cands, "next_cursor": None})
fx("integrations/ha_main.discovery_failed.json", "getDiscovery", "200",
   {"state": "failed", "error": "Home Assistant didn't answer in time"})
fx("integrations/ha_main.entity_details.running.json", "requestEntityDetails", "202",
   {"state": "running", "entity_id": "switch.tv_backlight"})
fx("integrations/ha_main.entity_details.done.json", "getEntityDetails", "200", {
    "state": "done", "entity_id": "switch.tv_backlight",
    "provider": ha_provider("switch.tv_backlight", "0x00158d0001a2b3c4-switch"),
    "name": "TV Backlight", "domain": "switch", "platform": "zha", "supported": True,
    "command_ids": [c["command_id"] for c in switch_commands()], "dropped_options": 0,
    "skipped_services": [{"service": "zha.issue_zigbee_cluster_command", "reason": "unsupported_field"}],
    "registered_device_id": None})
fx("integrations/ha_main.entity_details.failed.json", "getEntityDetails", "200",
   {"state": "failed", "entity_id": "switch.not_in_ha", "error": "switch.not_in_ha isn't in Home Assistant"})
fx("errors/details_required.json", "registerDevice", "409", err(
    "details_required", "Load this entity's details first "
    "(POST /api/v1/admin/integrations/ha_main/entities/switch.tv_backlight/details)"))
fx("admin/device_pair.json", "startDevicePairing", "202", {"state": "pairing", "window_s": 120})
fx("admin/device_pair.connected.json", "getDevicePairing", "200", {"state": "connected"})
fx("admin/device_pair.not_paired.json", "getDevicePairing", "200", {"state": "not_paired"})
fx("errors/pairing_busy.json", "startDevicePairing", "409", err("busy", "Another device is pairing"))
fx("errors/not_bluetooth.json", "startDevicePairing", "404",
   err("not_found", "Family Room Denon isn't a Bluetooth device"))
fx("errors/bluetooth_unavailable.json", "startDevicePairing", "503",
   err("integration_unavailable", "Bluetooth isn't working"))
fx("errors/bluetooth_busy.json", "startDevicePairing", "503", err("busy", "Bluetooth is busy. Try again."))
fx("errors/too_many_bluetooth_devices.json", "registerDevice", "422", err(
    "validation_failed", "You can add up to 4 Bluetooth devices", [
        ("/integration_id", "too_many_devices", "You can add up to 4 Bluetooth devices"),
    ]))

fx("admin/register_profile.request.json", "registerDevice", "request",
   {"profile_id": "hubbo:fire-tv", "name": "Bedroom Fire TV"})
fx("admin/register_profile.json", "registerDevice", "201",
   dict(profile_device("dev_bedroom_fire_tv", "hubbo:fire-tv", "Bedroom Fire TV"),
        availability="unavailable", bluetooth={"state": "not_paired"}))
fx("errors/unknown_profile.json", "registerDevice", "422", err(
    "validation_failed", "There's no profile hubbo:roku", [
        ("/profile_id", "unknown_profile", "There's no profile hubbo:roku"),
    ]))
fx("admin/register_device.request.json", "registerDevice", "request", {
    "name": "TV Backlight", "integration_id": "ha_main", "kind": "switch",
    "provider": ha_provider("switch.tv_backlight", "0x00158d0001a2b3c4-switch")})
fx("admin/update_device.request.json", "updateDevice", "request", {"name": "Living Room Sonos", "revision": 3})
fx("admin/remote_pair.scanning.json", "startRemotePairing", "202",
   {"state": "scanning", "paired": True, "remaining_s": 60,
    "message": "Press the pair button on the Harmony Hub"})
fx("admin/remote_pair.found.json", "getRemotePairing", "200",
   {"state": "found", "paired": True, "channel": 74})
fx("admin/remote.json", "getRemoteConfig", "200",
   {"radio": "listening", "endpoint": {"channel": 74, "address": "FC5C97550F"},
    "pairing": {"state": "found", "paired": True, "channel": 74}})
fx("admin/remote_endpoint.request.json", "setRemoteEndpoint", "request",
   {"channel": 74, "address": "0xFC5C97550F"})
fx("admin/tokens.json", "listTokens", "200", {"tokens": [
    {"id": "tok_k3x9q2mf", "name": "Claude agent", "hint": "hbt_k3x9q2mf_",
     "created_at": "2026-11-08T18:00:00Z", "last_used_at": "2026-11-08T20:01:13Z"},
    {"id": "tok_a7p0w4zt", "name": "HA automation", "hint": "hbt_a7p0w4zt_",
     "created_at": "2026-11-02T09:30:00Z", "last_used_at": None},
]})
fx("admin/token_create.request.json", "createToken", "request", {"name": "Claude agent"})
fx("admin/token_created.json", "createToken", "201", {
    "id": "tok_k3x9q2mf", "name": "Claude agent", "hint": "hbt_k3x9q2mf_",
    "created_at": "2026-11-08T18:00:00Z", "last_used_at": None, "token": TOKEN_SECRET})
fx("admin/password_change.request.json", "changeAdminPassword", "request",
   {"current_password": "correct horse battery", "new_password": "staple battery horse correct"})

BACKUP = {
    "format": "hubbo-backup", "format_version": 1, "created_at": "2026-11-08T20:20:00Z",
    "firmware_version": "0.1.0", "board": "waveshare_esp32_s3_eth",
    "remote": {"address": "FC5C97550F", "channel": 74},
    "integrations": [
        {"id": "system", "type": "system", "name": "System", "enabled": True},
        {"id": "ble", "type": "ble_hid", "name": "Bluetooth", "enabled": True,
         "settings": {"device_name": "Family Room Hubbo"}},
        {"id": "ha_main", "type": "home_assistant", "name": "Home Assistant", "enabled": True,
         "settings": {"url": "https://homeassistant.local:8123", "verify_tls": True}},
        {"id": "mqtt_main", "type": "mqtt", "name": "MQTT", "enabled": True,
         "settings": {"host": "192.0.2.10", "port": 1883, "username": "hubbo"}},
    ],
    "devices": [stored_device(d) for d in DEVICES],
    "global_bindings": GLOBAL_BINDINGS, "smart_buttons": BACKUP_SMART_BUTTONS, "activities": ACTIVITIES,
    "excluded": ["admin_password", "api_tokens", "integration_secrets", "ble_bonds"],
}
fx("admin/backup.json", "downloadBackup", "200", BACKUP)
# A board's first boot: nothing configured but the global Off binding (core's
# factoryDefaultConfig()). hub-sim --seed uses it, and the contract run's
# persistence check starts from it.
fx("admin/backup.empty.json", "downloadBackup", "200", {
    "format": "hubbo-backup", "format_version": 1, "created_at": None,
    "firmware_version": "0.1.0", "board": "waveshare_esp32_s3_eth",
    "integrations": [], "devices": [],
    "global_bindings": {"bindings": {"off:tap": [a(None, "off")]}, "macros": [], "revision": 1},
    "smart_buttons": {"slots": EMPTY_SLOTS, "revision": 1},
    "activities": [], "excluded": ["admin_password", "api_tokens", "integration_secrets", "ble_bonds"],
})
fx("admin/restore.request.json", "restoreBackup", "request", BACKUP)
# While the config is blocked, GET /admin/backup exports the saved record's
# raw bytes (X-Hubbo-Backup: blocked-raw); restore takes such a record back
# if this firmware can read it.
fx("admin/restore.blocked-raw.request.json", "restoreBackup", "request", {
    "format": "hubbo-config", "schema_version": 2, "revision": 41,
    "integrations": [], "devices": [], "global_bindings": {"bindings": {}, "macros": [], "revision": 1}, "activities": []})
fx("errors/restore_unsupported_schema.json", "restoreBackup", "422", err(
    "validation_failed", "This saved configuration is from newer firmware; update first", [
        ("/schema_version", "unsupported_schema", "This saved configuration is from newer firmware; update first"),
    ]))
fx("errors/backup_storage_unavailable.json", "downloadBackup", "503",
   err("storage_unavailable", "The configuration store can't be read right now; try again later"))
fx("errors/backup_blocked_damaged.json", "downloadBackup", "409",
   err("config_blocked", "The saved configuration is damaged; no intact copy to back up"))
fx("admin/restore.result.json", "restoreBackup", "200", {
    "dry_run": False, "devices": len(DEVICES), "activities": len(ACTIVITIES), "integrations": len(BACKUP["integrations"]),
    "migrated_from": None, "requires": ["home_assistant_token", "mqtt_password", "ble_pairing", "api_tokens"], "warnings": []})
fx("admin/reset.json", "resetConfiguration", "200", {"state": "ok", "generation": 58})
fx("errors/config_blocked.json", "createActivity", "409",
   err("config_blocked", "The saved configuration can't be used; restore a backup or reset"))
fx("errors/hold_not_allowed.json", "executeCommand", "422", err(
    "validation_failed", "key_down holds a key; use key_tap", [
        ("/command_id", "hold_not_allowed", "key_down/key_up are for bindings; execute key_tap instead"),
    ]))


def firmware(state="idle", progress=0, error=None, version="0.5.0", **release):
    """GET /admin/firmware: the update, then the release fields (checked an hour ago, nothing newer)."""
    out = {"version": version, "board": "waveshare_esp32_s3_eth",
           "update": {"state": state, "progress": progress, "error": error},
           "available": None, "checking": False, "checked_ago_ms": 3600000, "check_error": None,
           "rolled_back_from": None}
    out.update(release)
    return out


AVAILABLE = {"version": "1.4.0"}
fx("admin/firmware.json", "getFirmwareStatus", "200", firmware())
fx("admin/firmware.writing.json", "getFirmwareStatus", "200", firmware("writing", 42))
fx("admin/firmware.rebooting.json", "getFirmwareStatus", "200", firmware("rebooting", 100))
fx("admin/firmware.failed.json", "getFirmwareStatus", "200", firmware("failed", error="write_failed"))
fx("admin/firmware.update.json", "updateFirmware", "202", firmware("writing"))
# Releases: a release build (plain X.Y.Z; a development build's git describe
# is never offered one) with 1.4.0 available, its download, a failed check
# that keeps what the last good one found, and a rollback.
fx("admin/firmware.available.json", "getFirmwareStatus", "200", firmware(version="1.3.0", available=AVAILABLE))
fx("admin/firmware.downloading.json", "getFirmwareStatus", "200",
   firmware("downloading", 35, version="1.3.0", available=AVAILABLE))
fx("admin/firmware.check_error.json", "getFirmwareStatus", "200",
   firmware(version="1.3.0", available=AVAILABLE, checked_ago_ms=12000, check_error="unreachable"))
fx("admin/firmware.rolled_back.json", "getFirmwareStatus", "200",
   firmware(version="1.4.0", rolled_back_from="1.5.0"))
fx("admin/firmware.never_checked.json", "getFirmwareStatus", "200", firmware(checked_ago_ms=None))
fx("admin/firmware.check.json", "checkFirmwareRelease", "202",
   firmware(version="1.3.0", checking=True))
fx("errors/check_busy.json", "checkFirmwareRelease", "409", err("busy", "A check is already running"))
fx("admin/firmware_install.request.json", "installFirmwareRelease", "request", {"version": "1.4.0"})
fx("admin/firmware.install.json", "installFirmwareRelease", "202",
   firmware("downloading", version="1.3.0", available=AVAILABLE))
fx("errors/not_available.json", "installFirmwareRelease", "409",
   err("not_available", "That version isn't available; check again"))
fx("errors/update_in_progress.json", "updateFirmware", "409",
   err("update_in_progress", "An update is already running"))
fx("errors/wrong_board.json", "updateFirmware", "422",
   err("wrong_board", "This update is for a different device"))
fx("errors/invalid_image.json", "updateFirmware", "422",
   err("invalid_image", "This isn't a Hubbo update file"))
fx("errors/length_required.json", "updateFirmware", "411",
   err("length_required", "Send the image whole, with Content-Length"))
fx("errors/factory_reset_during_update.json", "factoryReset", "409",
   err("update_in_progress", "A firmware update is running"))

fx("admin/hub.json", "getHub", "200", HUB)
fx("admin/hub.conflict.json", "getHub", "200",
   dict(HUB, hostname=HUB["hostname"] + "-2", hostname_conflict=True))
fx("admin/hub_update.request.json", "updateHub", "request", {"name": HUB["name"]})
fx("admin/hub_settings.request.json", "updateHub", "request", {"check_updates": False, "radio_power": "low"})
fx("errors/hub_name_invalid.json", "updateHub", "422", err(
    "validation_failed", "Name must be at most 32 characters", [
        ("/name", "too_long", "Name must be at most 32 characters")]))
fx("admin/restart.json", "restartHub", "202", {})
fx("errors/restart_during_update.json", "restartHub", "409",
   err("update_in_progress", "A firmware update is running"))

# Errors
fx("errors/unauthenticated.json", "listDevices", "401",
   err("unauthenticated", "Log in or send an API token", docs=GUIDE))
fx("errors/invalid_credentials.json", "login", "401",
   err("invalid_credentials", "Wrong username or password"))
fx("errors/forbidden_token_admin.json", "registerDevice", "403",
   err("forbidden", "API tokens cannot register devices"))
fx("errors/csrf_failed.json", "createActivity", "403",
   err("csrf_failed", "Origin does not match this hub"))
fx("errors/not_found.json", "getDevice", "404", err("not_found", "No device dev_projector"))
fx("errors/revision_conflict.json", "replaceActivity", "409",
   err("revision_conflict", "Activity changed since revision 3", current_revision=4))
fx("errors/device_in_use.json", "deleteDevice", "409", err("device_in_use", "dev_sonos is used by 2 activities", [
    ("/activities/act_watch_apple_tv/startup/1", "referenced", "Watch Apple TV startup step 2"),
    ("/activities/act_watch_apple_tv/bindings/volume_up:down/0", "referenced", "Watch Apple TV volume_up:down"),
]))
fx("errors/payload_too_large.json", "createActivity", "413",
   err("payload_too_large", "Body exceeds 32768 bytes"))
fx("errors/validation_nested_target.json", "createActivity", "422", err(
    "validation_failed", "Activity is invalid", [
        ("/startup/0/params/entity_id", "forbidden_param", "entity_id is never accepted; target the registered device"),
        ("/bindings/volume_up:down/0/params/level", "invalid_param_type", "level must be a number"),
        ("/bindings/light_1:down/0/params/target", "forbidden_param", "target is never accepted"),
    ]))
fx("errors/validation_execute.json", "executeCommand", "422", err(
    "validation_failed", "Invalid params for media_player.select_source", [
        ("/params/source", "invalid_enum_value", "source must be one of the device's sources"),
    ]))
fx("errors/setup_required.json", "login", "409", err("setup_required", "Create the admin account first"))
fx("errors/integration_unavailable.json", "executeCommand", "503",
   err("integration_unavailable", "Home Assistant is not connected"))

# Malicious request bodies the server must reject. Kept as data for the
# simulator's security tests; not schema-validated as valid (the lint expects
# them to FAIL request validation).
MALICIOUS = {
    "nested_entity_id.json": dict(input_of(WATCH), startup=[
        {"integration_id": "ha_main", "device_id": "dev_lg_tv", "command_id": "media_player.turn_on",
         "params": {"entity_id": "all"}}]),
    "nested_target_object.json": dict(input_of(WATCH), bindings={"light_1:down": [
        {"integration_id": "ha_main", "device_id": "dev_family_room_lights", "command_id": "light.turn_on",
         "params": {"brightness": {"target": {"area_id": "house"}}}}]}),
    "raw_service_call.json": dict(input_of(WATCH), startup=[
        {"integration_id": "ha_main", "device_id": "dev_lg_tv", "command_id": "media_player.turn_on",
         "params": {}, "service": "homeassistant.turn_off", "target": {"entity_id": "all"}}]),
    "array_param.json": dict(input_of(WATCH), startup=[
        {"integration_id": "ha_main", "device_id": "dev_family_room_lights", "command_id": "light.turn_on",
         "params": {"brightness": [255, {"entity_id": "light.all"}]}}]),
    "raw_entity_device_id.json": dict(input_of(WATCH), startup=[
        {"integration_id": "ha_main", "device_id": "media_player.family_room_lg_tv",
         "command_id": "media_player.turn_on", "params": {}}]),
}
MALICIOUS_OP = "createActivity"


def main():
    check = "--check" in sys.argv
    files = {}
    index = []
    for name, op, kind, content in FIXTURES:
        files[name] = content
        index.append({"file": name, "operation": op, "kind": kind})
    for name, content in MALICIOUS.items():
        files["malicious/" + name] = content
        index.append({"file": "malicious/" + name, "operation": MALICIOUS_OP, "kind": "request-invalid"})
    files["index.json"] = {"$comment": "Generated by api/tools/gen_fixtures.py; do not edit.",
                           "fixtures": index}
    stale = []
    for name, content in files.items():
        path = os.path.join(OUT, name)
        text = json.dumps(content, indent=2, ensure_ascii=False) + "\n"
        try:
            with open(path, encoding="utf-8") as f:
                if f.read() == text:
                    continue
        except FileNotFoundError:
            pass
        stale.append(name)
        if not check:  # write only what changed, so mtimes mean something
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
    if check:
        known = set(files)
        for root, _, names in os.walk(OUT):
            for n in names:
                rel = os.path.relpath(os.path.join(root, n), OUT)
                if rel.endswith(".json") and rel not in known:
                    stale.append(rel + " (not generated)")
        if stale:
            print("fixtures out of date; run `api/lint.sh gen`:", *stale, sep="\n  ")
            return 1
        print(f"fixtures up to date ({len(files)} files)")
    else:
        print(f"{len(stale)} of {len(files)} fixture files changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
