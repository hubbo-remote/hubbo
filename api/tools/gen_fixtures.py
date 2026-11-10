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
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fixtures")

# ---------------------------------------------------------------- commands

# These build exactly what the hub serves (core ParamSpec/CommandSpec):
# no param labels, units or command descriptions; int params have step 1.
def p_enum(name, options, required=True):
    return {"name": name, "type": "enum", "required": required, "options": options}


def p_num(name, typ, lo, hi, required=True):
    d = {"name": name, "type": typ, "required": required, "minimum": lo, "maximum": hi}
    if typ == "int":
        d["step"] = 1
    return d


def p_bool(name, required=True):
    return {"name": name, "type": "bool", "required": required}


def cmd(command_id, label, params=(), effect="momentary"):
    """effect: "momentary", or "hold" for commands that leave state held (key_down)."""
    return {"command_id": command_id, "label": label, "effect": effect, "params": list(params)}


# BLE HID keys offered for the Apple TV: the core BLE adapter's key catalog
# (core/src/integrations/ble_hid.cpp), as hub-sim serves it.
HID_KEYS = [
    "ARROW_UP", "ARROW_DOWN", "ARROW_LEFT", "ARROW_RIGHT", "SELECT", "MENU", "AC_HOME",
    "PLAY_PAUSE", "SCAN_NEXT", "SCAN_PREVIOUS", "RECORD", "VOLUME_UP", "VOLUME_DOWN", "MUTE",
    "KEY_1", "KEY_2", "KEY_3", "KEY_4", "KEY_5", "KEY_6", "KEY_7", "KEY_8", "KEY_9", "KEY_0",
    "ENTER", "F1", "F2", "F3", "F4", "PAGE_UP", "PAGE_DOWN", "KEY_A", "KEY_B", "KEY_C", "KEY_D",
    "KEY_E", "KEY_F", "KEY_G", "KEY_H", "KEY_I", "KEY_J", "KEY_K", "KEY_L", "KEY_M", "KEY_N",
    "KEY_O", "KEY_P", "KEY_Q", "KEY_R", "KEY_S", "KEY_T", "KEY_U", "KEY_V", "KEY_W", "KEY_X",
    "KEY_Y", "KEY_Z",
]


def ble_commands():
    key = lambda: p_enum("key", HID_KEYS)
    return [
        cmd("key_down", "Key down", [key()], effect="hold"),
        cmd("key_up", "Key up", [key()]),
        cmd("key_tap", "Key tap", [key()]),
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


# ---------------------------------------------------------------- devices

HA_ENTITIES = {
    "dev_denon": {
        "entity_id": "media_player.family_room_denon", "unique_id": "c5a1e0b2f3d44c1aa1b2c3d4e5f60718",
        "name": "Family Room Denon", "kind": "receiver", "domain": "media_player", "state": "off",
        "supported_features": 69004,  # denonavr SUPPORT_DENON | SELECT_SOUND_MODE
        "attributes": {
            "source_list": ["Blu-ray", "CBL/SAT", "DVD", "Game", "Media Player", "TV Audio", "Tuner", "Bluetooth"],
            "sound_mode_list": ["MUSIC", "MOVIE", "GAME", "AUTO", "STEREO", "DIRECT"],
        },
    },
    "dev_sonos": {
        "entity_id": "media_player.family_room_sonos", "unique_id": "RINCON_48A6B8000000001400",
        "name": "Family Room Sonos", "kind": "speaker", "domain": "media_player", "state": "idle",
        "revision": 3,  # renamed twice
        "supported_features": 8321599,  # sonos _attr_supported_features | SELECT_SOURCE (Arc)
        "attributes": {"source_list": ["TV"]},
    },
    "dev_lg_tv": {
        "entity_id": "media_player.family_room_lg_tv", "unique_id": "1b2e6c3a-9f4d-4a7e-b1c2-d3e4f5a6b7c8",
        "name": "Family Room LG TV", "kind": "tv", "domain": "media_player", "state": "off",
        # webostv SUPPORT_WEBOSTV | MUTE | VOLUME_STEP (external speaker) | TURN_ON (turn-on trigger)
        "supported_features": 24505,
        "attributes": {"source_list": ["HDMI 1", "HDMI 2", "HDMI 3", "Live TV", "Netflix", "YouTube"]},
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


def ha_provider(entity_id, unique_id=None):
    p = {"type": "home_assistant", "entity_id": entity_id}
    if unique_id:
        p["unique_id"] = unique_id
    return p


def ha_device(dev_id, e):
    return {
        "id": dev_id, "name": e["name"], "integration_id": "ha_main", "kind": e["kind"],
        "provider": ha_provider(e["entity_id"], e["unique_id"]),
        "availability": "available", "schema_version": 1, "revision": e.get("revision", 1),
    }


def ha_snapshot(e):
    """The stored `ha_capabilities` (backups only; never in a Device response)."""
    snap = {"platform": e.get("platform", "demo"), "supported_features": e["supported_features"]}
    for key in ("source_list", "sound_mode_list", "supported_color_modes",
                "min_color_temp_kelvin", "max_color_temp_kelvin"):
        if key in e["attributes"]:
            snap[key] = e["attributes"][key]
    snap["services"] = []
    return snap


def stored_device(d):
    """A Device as a backup stores it: no availability; HA devices carry their snapshot."""
    out = {k: x for k, x in d.items() if k != "availability"}
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
    return []


DEVICES = [
    {"id": "dev_apple_tv", "name": "Family Room Apple TV", "integration_id": "ble", "kind": "streamer",
     "provider": {"type": "ble_hid", "peer_name": "Family Room"}, "availability": "available",
     "schema_version": 1, "revision": 1},
] + [ha_device(k, v) for k, v in HA_ENTITIES.items()]

CAPABILITIES = {"dev_apple_tv": {"device_id": "dev_apple_tv", "integration_id": "ble", "commands": ble_commands()}}
for k, v in HA_ENTITIES.items():
    CAPABILITIES[k] = {"device_id": k, "integration_id": "ha_main", "commands": ha_commands(v)}

# ---------------------------------------------------------------- activities

def a(device, command, **params):
    integ = "system" if device is None else next(d["integration_id"] for d in DEVICES if d["id"] == device)
    return {"integration_id": integ, "device_id": device, "command_id": command, "params": params}


def wait(ms):
    return a(None, "wait", duration_ms=ms)


def key_bindings(button, key):
    return {f"{button}:down": [a("dev_apple_tv", "key_down", key=key)],
            f"{button}:up": [a("dev_apple_tv", "key_up", key=key)]}


watch_bindings = {}
for b, k in [("up", "ARROW_UP"), ("down", "ARROW_DOWN"), ("left", "ARROW_LEFT"),
             ("right", "ARROW_RIGHT"), ("select", "SELECT"), ("menu", "MENU"),
             ("back", "AC_HOME"), ("exit", "AC_HOME"), ("skip_forward", "SCAN_NEXT"),
             ("skip_backward", "SCAN_PREVIOUS")]:
    watch_bindings.update(key_bindings(b, k))
watch_bindings.update({
    "play:tap": [a("dev_apple_tv", "key_tap", key="PLAY_PAUSE")],
    "pause:tap": [a("dev_apple_tv", "key_tap", key="PLAY_PAUSE")],
    "info:hold": [a("dev_apple_tv", "key_tap", key="KEY_I")],
    "volume_up:down": [a("dev_sonos", "media_player.volume_up")],
    "volume_down:down": [a("dev_sonos", "media_player.volume_down")],
    "mute:tap": [a("dev_sonos", "media_player.volume_mute", muted=True)],
    "plus:tap": [a("dev_family_room_lights", "light.brightness_step", step=32)],
    "minus:tap": [a("dev_family_room_lights", "light.brightness_step", step=-32)],
    "light_1:tap": [a("dev_family_room_lights", "light.turn_on", brightness=255, transition=1)],
    "light_2:tap": [a("dev_family_room_lights", "light.turn_off", transition=1)],
    "blue:tap": [a("dev_apple_tv", "key_tap", key="F1")],
})

WATCH = {
    "id": "act_watch_apple_tv", "name": "Watch Apple TV",
    "startup": [
        a("dev_lg_tv", "media_player.turn_on"),
        a("dev_sonos", "media_player.select_source", source="TV"),
        wait(1500),
        a("dev_lg_tv", "media_player.select_source", source="HDMI 1"),
        a("dev_family_room_lights", "light.turn_on", brightness=64, color_temp_kelvin=2700, transition=2),
    ],
    "shutdown": [
        a("dev_lg_tv", "media_player.turn_off"),
        a("dev_sonos", "media_player.media_pause"),
    ],
    "bindings": watch_bindings, "schema_version": 1, "revision": 4,
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
        "mute:tap": [a("dev_denon", "media_player.volume_mute", muted=True)],
        "mute:hold": [a("dev_denon", "media_player.volume_mute", muted=False)],
        "play:tap": [a("dev_xbox", "media_player.media_play_pause")],
        "pause:tap": [a("dev_xbox", "media_player.media_play_pause")],
        "skip_forward:tap": [a("dev_xbox", "media_player.media_next_track")],
        "skip_backward:tap": [a("dev_xbox", "media_player.media_previous_track")],
        "light_1:tap": [a("dev_family_room_lights", "light.turn_on", brightness=255)],
        "light_2:tap": [a("dev_family_room_lights", "light.turn_off")],
        "red:tap": [a("dev_denon", "media_player.volume_set", level=0.35)],
    },
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
    "revision": 6,
}


def input_of(act):
    return {k: copy.deepcopy(act[k]) for k in ("name", "startup", "shutdown", "bindings")}


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
     "detail": "Connected to Family Room", "device_count": 1},
    {"id": "ha_main", "type": "home_assistant", "name": "Home Assistant", "enabled": True,
     "state": "connected", "detail": "Home Assistant 2026.11.1", "device_count": 5},
]

STATUS = {
    "hubbo": {"version": "0.1.0", "build": "0.1.0+g1a2b3c4", "board": "waveshare_esp32_s3_eth"},
    "uptime_s": 86400, "time": "2026-11-08T20:15:00Z", "auth_mode": "enforced",
    "auth_state": "ok", "config": {"state": "ok", "generation": 42},
    "active_activity_id": "act_watch_apple_tv",
    "network": {"link": "up", "ip": "192.0.2.56", "hostname": "hubbo-d4afab"},
    "remote": REMOTE_STATUS,
    "integrations": [{"id": i["id"], "type": i["type"], "state": i["state"]} for i in INTEGRATIONS],
    "diagnostics": {
        "heap_free": 182340, "heap_min_free": 141020, "psram_free": 7912448, "queue_overflows": 0,
        "dispatch_latency": {"count": 1532, "p50_ms": 1.2, "p95_ms": 3.8, "p99_ms": 7.5},
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

# (file, operation, kind, content). kind: HTTP status as string, or "request".
FIXTURES = []


def fx(name, op, kind, content):
    FIXTURES.append((name, op, kind, content))


fx("status.json", "getStatus", "200", STATUS)
fx("status.blocked.json", "getStatus", "200",
   dict(copy.deepcopy(STATUS), active_activity_id=None,
        config={"state": "blocked", "generation": 57,
                "reason": "config.json schema_version 2 is newer than this firmware (1)"}))
fx("status.simulator.json", "getStatus", "200",
   dict(copy.deepcopy(STATUS), auth_mode="simulator_no_auth",
        hubbo={"version": "0.1.0", "build": "sim", "board": "hub-sim"}))
fx("remote/status.json", "getRemoteStatus", "200", REMOTE_STATUS)
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

fx("devices/list.json", "listDevices", "200", {"devices": DEVICES})
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
    "status": "failed", "device_id": "dev_apple_tv", "command_id": "key_tap", "elapsed_ms": 1,
    "message": "Bluetooth host not connected"})

fx("activities/list.json", "listActivities", "200",
   {"activities": ACTIVITIES, "active_activity_id": "act_watch_apple_tv"})
for act in ACTIVITIES:
    fx(f"activities/{act['id']}.json", "getActivity", "200", act)
create = input_of(PLAY_XBOX)
create["name"] = "Play Xbox (copy)"
fx("activities/create.request.json", "createActivity", "request", create)
replace = copy.deepcopy(WATCH)
replace["bindings"]["green:tap"] = [a("dev_apple_tv", "key_tap", key="F3")]
fx("activities/replace.request.json", "replaceActivity", "request", replace)
fx("activities/global_bindings.json", "getGlobalBindings", "200", GLOBAL_BINDINGS)
fx("activities/validate.valid.json", "validateActivity", "200", {"valid": True, "errors": []})
fx("activities/validate.invalid.json", "validateActivity", "200", {"valid": False, "errors": [
    {"path": "/bindings/volume_up:down/0/params/level", "code": "param_out_of_range",
     "message": "level must be between 0 and 1"},
    {"path": "/startup/2/device_id", "code": "unknown_device", "message": "dev_projector is not registered"},
    {"path": "/bindings/play:tap/0/integration_id", "code": "integration_mismatch",
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
    "settings": {"type": "ble_hid", "device_name": "Family Room Hubbo", "bonded": True,
                 "peer_name": "Family Room"},
    "revision": 1})
fx("integrations/ha_main.configure.request.json", "configureIntegration", "request", {
    "settings": {"type": "home_assistant", "url": "https://homeassistant.local:8123",
                 "verify_tls": True, "access_token": "eyJhbGciOiJIUzI1NiJ9.example-not-a-real-token"},
    "revision": 2})
# Discovery candidates: domain commands only (a preview), no unique_id; the
# details add the integration's services and the unique_id.
cands = []
for dev_id, e in HA_ENTITIES.items():
    cands.append({"provider": ha_provider(e["entity_id"]),
                  "name": e["name"], "domain": e["domain"], "platform": e.get("platform", "demo"),
                  "supported": True, "command_ids": [c["command_id"] for c in ha_commands(e)],
                  "dropped_options": 0, "registered_device_id": dev_id})
cands.append({"provider": ha_provider("switch.tv_backlight"),
              "name": "TV Backlight", "domain": "switch", "platform": "zha", "supported": True,
              "command_ids": [c["command_id"] for c in switch_commands()], "dropped_options": 0,
              "registered_device_id": None})
cands.append({"provider": ha_provider("switch.porch_light"),  # YAML-only: no registry entry, no platform
              "name": "Porch Light", "domain": "switch", "supported": True,
              "command_ids": [c["command_id"] for c in switch_commands()], "dropped_options": 0,
              "registered_device_id": None})
cands.append({"provider": ha_provider("media_player.kitchen_display"),
              "name": "Kitchen Display", "domain": "media_player", "platform": "cast", "supported": False,
              "command_ids": [], "dropped_options": 0, "registered_device_id": None})
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
    "skipped_services": ["zha.issue_zigbee_cluster_command"], "registered_device_id": None})
fx("integrations/ha_main.entity_details.failed.json", "getEntityDetails", "200",
   {"state": "failed", "entity_id": "switch.not_in_ha", "error": "switch.not_in_ha isn't in Home Assistant"})
fx("errors/details_required.json", "registerDevice", "409", err(
    "details_required", "Load this entity's details first "
    "(POST /api/v1/admin/integrations/ha_main/entities/switch.tv_backlight/details)"))
fx("integrations/ble.pair.json", "startIntegrationPairing", "202", {"state": "pairing", "window_s": 120})
fx("integrations/ble.pair.paired.json", "getIntegrationPairing", "200", {"state": "paired", "peer_name": "Living Room"})

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
    ],
    "devices": [stored_device(d) for d in DEVICES],
    "global_bindings": GLOBAL_BINDINGS, "activities": ACTIVITIES,
    "excluded": ["admin_password", "api_tokens", "integration_secrets", "ble_bonds"],
}
fx("admin/backup.json", "downloadBackup", "200", BACKUP)
# A board's first boot: nothing configured. hub-sim --seed uses it, and the
# contract run's persistence check starts from it.
fx("admin/backup.empty.json", "downloadBackup", "200", {
    "format": "hubbo-backup", "format_version": 1, "created_at": None,
    "firmware_version": "0.1.0", "board": "waveshare_esp32_s3_eth",
    "integrations": [], "devices": [], "global_bindings": {"bindings": {}, "revision": 1},
    "activities": [], "excluded": ["admin_password", "api_tokens", "integration_secrets", "ble_bonds"],
})
fx("admin/restore.request.json", "restoreBackup", "request", BACKUP)
# While the config is blocked, GET /admin/backup exports the saved record's
# raw bytes (X-Hubbo-Backup: blocked-raw); restore takes such a record back
# if this firmware can read it.
fx("admin/restore.blocked-raw.request.json", "restoreBackup", "request", {
    "format": "hubbo-config", "schema_version": 2, "revision": 41,
    "integrations": [], "devices": [], "global_bindings": {"bindings": {}, "revision": 1}, "activities": []})
fx("errors/restore_unsupported_schema.json", "restoreBackup", "422", err(
    "validation_failed", "This saved configuration is from newer firmware; update first", [
        ("/schema_version", "unsupported_schema", "This saved configuration is from newer firmware; update first"),
    ]))
fx("errors/backup_storage_unavailable.json", "downloadBackup", "503",
   err("storage_unavailable", "The configuration store can't be read right now; try again later"))
fx("errors/backup_blocked_damaged.json", "downloadBackup", "409",
   err("config_blocked", "The saved configuration is damaged; no intact copy to back up"))
fx("admin/restore.result.json", "restoreBackup", "200", {
    "dry_run": False, "devices": len(DEVICES), "activities": len(ACTIVITIES), "integrations": 3,
    "migrated_from": None, "requires": ["home_assistant_token", "ble_pairing", "api_tokens"], "warnings": []})
fx("admin/reset.json", "resetConfiguration", "200", {"state": "ok", "generation": 58})
fx("errors/config_blocked.json", "createActivity", "409",
   err("config_blocked", "The saved configuration can't be used; restore a backup or reset"))
fx("errors/hold_not_allowed.json", "executeCommand", "422", err(
    "validation_failed", "key_down holds a key; use key_tap", [
        ("/command_id", "hold_not_allowed", "key_down/key_up are for bindings; execute key_tap instead"),
    ]))
fx("admin/firmware.json", "getFirmwareStatus", "200", {
    "version": "v0.5.0", "board": "waveshare_esp32_s3_eth",
    "update": {"state": "idle", "progress": 0, "error": None}})
fx("admin/firmware.writing.json", "getFirmwareStatus", "200", {
    "version": "v0.5.0", "board": "waveshare_esp32_s3_eth",
    "update": {"state": "writing", "progress": 42, "error": None}})
fx("admin/firmware.rebooting.json", "getFirmwareStatus", "200", {
    "version": "v0.5.0", "board": "waveshare_esp32_s3_eth",
    "update": {"state": "rebooting", "progress": 100, "error": None}})
fx("admin/firmware.failed.json", "getFirmwareStatus", "200", {
    "version": "v0.5.0", "board": "waveshare_esp32_s3_eth",
    "update": {"state": "failed", "progress": 0, "error": "write_failed"}})
fx("admin/firmware.update.json", "updateFirmware", "202", {
    "version": "v0.5.0", "board": "waveshare_esp32_s3_eth",
    "update": {"state": "writing", "progress": 0, "error": None}})
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

# Errors
fx("errors/unauthenticated.json", "listDevices", "401",
   err("unauthenticated", "Log in or send an API token"))
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
        ("/bindings/light_1:tap/0/params/target", "forbidden_param", "target is never accepted"),
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
    "nested_target_object.json": dict(input_of(WATCH), bindings={"light_1:tap": [
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
