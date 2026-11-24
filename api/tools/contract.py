#!/usr/bin/env python3
"""Contract tests: drive a running hub-sim over HTTP and check every response
against api/openapi.yaml (status declared for the operation, body valid,
no secrets).

    contract.py --base http://127.0.0.1:18080 --mode noauth
    contract.py --base http://127.0.0.1:18081 --mode auth

noauth: SIMULATOR_NO_AUTH, every request is a simulated admin. Walks the
        Phase 2 gate flow (registry, activity CRUD/validate/activate/off,
        execute, tokens, firmware update) with the request fixtures.
auth:   production auth policy. First-run setup with the user's own
        username, login, cookies, cross-site Origin, tokens, the full role
        matrix over HTTP (a token can't update firmware).

mock:   the web admin's mock API (web/mock, signed in as the dev admin) gets
        the noauth flows; mock-auth gets the auth flows from first run.
        Checks the mock can't support are skipped and listed as
        "SKIPPED (<mode>)" at the end (see skip()).

Both hub modes then restart hub-sim (same --data, i.e. a reboot) after a first
boot, the default seed, every kind of edit and restores, and require the
whole observable state (activities, devices, capabilities, global bindings,
integrations, active activity, backup) to be identical across the restart.
Every `revision` in every response must be >= 1. Run via api/contract.sh.
"""
import argparse
import contextlib
import copy
import gzip
import http.client
import http.cookies
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import validate as v  # noqa: E402

FIXTURES = v.FIXTURES
PASSWORD = "correct horse battery"
# hub-sim's fake Home Assistant accepts this token (sim/src/fake_ha.h). Like
# every secret-named field the contract sends (remember_secrets), it may then
# appear in no response (any operation, any status, the backup included).
HA_TOKEN = "fake-ha-token"
# hub-sim's fake broker takes any login (sim/src/fake_mqtt.h); the same
# no-echo rule applies.
MQTT_PASSWORD = "fake-broker-password"
GUIDE_PATH = os.path.join(v.ROOT, "docs", "agent-api.md")  # served as GET /api/v1/agent.md
LLMS_PATH = os.path.join(v.ROOT, "docs", "llms.txt")  # served as GET /llms.txt
GUIDE = SPEC_LINK = None  # the guide's and the spec's paths, read from the spec in main()
TRANSPORT_ERRORS = (405,)  # any request may get these (api/openapi.yaml conventions)
ADMIN = "hubbo-owner"  # setup takes the user's own username (there's no default)

failures = []
skips = []  # (what, why): checks this target can't support, printed at the end
checked = 0
MOCK = False  # the target is web/mock, not hub-sim (--mode mock / mock-auth)
sent_secrets = set()  # write-only values sent so far (see HA_TOKEN)


def remember_secrets(value):
    """Adds the values of secret-named request fields (validate.SECRET_KEYS) to sent_secrets."""
    if isinstance(value, dict):
        for k, x in value.items():
            if k in v.SECRET_KEYS and isinstance(x, str) and len(x) >= 8:
                sent_secrets.add(x)
            remember_secrets(x)
    elif isinstance(value, list):
        for x in value:
            remember_secrets(x)


def skip(what, why):
    """A check this target can't run: recorded and printed, never silently dropped."""
    skips.append((what, why))


def fixture(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as f:
        return json.load(f)


class Client:
    def __init__(self, base, checker):
        self.base = base.rstrip("/")
        self.checker = checker
        self.cookie = None
        self.token = None

    def _send(self, method, path, data=None, headers=None, json_body=False):
        """One request with this client's credentials: (status, headers, raw bytes, error).
        status is 0 (and error says why) when nothing answered."""
        h = {"Host": "127.0.0.1"}
        if json_body:
            h["Content-Type"] = "application/json"
        if self.token:
            h["Authorization"] = "Bearer " + self.token
        elif self.cookie:
            h["Cookie"] = "hubbo_session=" + self.cookie
        h.update(headers or {})
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status, r.headers, r.read(), None
        except urllib.error.HTTPError as e:
            return e.code, e.headers, e.read(), None
        except (urllib.error.URLError, OSError) as e:
            return 0, http.client.HTTPMessage(), b"", e

    def call(self, op_id, method, path, body=None, expect=None, headers=None, raw=None, label=None):
        """One request. Validates the response against op_id's spec; checks the status if given."""
        global checked
        remember_secrets(body)
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        status, resp_headers, raw_body, error = self._send(method, "/api/v1" + path, data, headers,
                                                           json_body=data is not None and raw is None)
        if error:  # no answer at all: a failure, not a crash
            checked += 1
            failures.append(f"{label or op_id} {method} {path}: no response ({error})")
            return 0, None, resp_headers
        text = raw_body.decode()
        where = f"{label or op_id} {method} {path} -> {status}"
        checked += 1
        for secret in sent_secrets:
            if secret in text:
                failures.append(f"{where}: a write-only secret appears in the response")
        if expect is not None and status != expect:
            failures.append(f"{where}: expected {expect}: {text[:300]}")
        op = self.checker.ops[op_id]
        declared = op["responses"]
        content_type = (resp_headers.get("Content-Type") or "").split(";")[0].strip()
        if text and content_type and content_type != "application/json":
            # A declared non-JSON body (the blocked-raw backup): its media
            # type must be declared for this status; the bytes are opaque.
            resp = declared.get(str(status), {})
            if "$ref" in resp:
                resp = v.resolve(self.checker.spec, resp["$ref"])
            if content_type not in resp.get("content", {}):
                failures.append(f"{where}: Content-Type {content_type} not declared for {op_id} {status}")
            return status, text, resp_headers
        body_json = None
        if text:
            try:
                body_json = json.loads(text)
            except ValueError:
                failures.append(f"{where}: body is not JSON: {text[:120]}")
        if status in TRANSPORT_ERRORS:
            for p in self.checker.ref_errors("#/components/schemas/Error", body_json):
                failures.append(f"{where}: {p}")
        elif str(status) not in declared:
            failures.append(f"{where}: status {status} not declared for {op_id}")
        elif "content" not in declared[str(status)] and "$ref" not in declared[str(status)]:
            if text:
                failures.append(f"{where}: {op_id} {status} declares no body, got {text[:120]}")
        else:
            if body_json is None:
                failures.append(f"{where}: empty body")
            else:
                for p in self.checker.errors(op_id, str(status), body_json)[:5]:
                    failures.append(f"{where}: {p}")
        describes_api = op.get("x-hubbo-describes-api", False)
        for path_, value in [] if describes_api else revisions(body_json):
            if not isinstance(value, int) or isinstance(value, bool) or value < 1:
                failures.append(f"{where}: {path_} = {value!r}; revisions start at 1")
        scan_secrets = not (describes_api or op.get("x-hubbo-returns-secret", False))
        if body_json is not None and status < 300 and scan_secrets:
            for path_ in v.find_secrets(body_json):
                failures.append(f"{where}: secret field {path_} in response")
        cookie = resp_headers.get("Set-Cookie")
        if cookie:
            morsel = http.cookies.SimpleCookie(cookie).get("hubbo_session")
            if morsel is not None:
                if "httponly" not in cookie.lower() or "samesite=strict" not in cookie.lower():
                    failures.append(f"{where}: session cookie must be HttpOnly and SameSite=Strict")
        return status, body_json, resp_headers

    def host_call(self, method, path, body=None):
        """A route outside the spec (e.g. /sim/storage), by its full path; returns (status, json)."""
        data = json.dumps(body).encode() if body is not None else None
        status, _, raw, error = self._send(method, path, data, json_body=data is not None)
        if error:
            return 0, {"error": str(error)}
        try:
            return status, json.loads(raw) if raw else None
        except ValueError:
            return status, None

    def fetch(self, path, compressed=False):
        """A GET by full path, undecoded: (status, headers, wire bytes, body bytes). `compressed`
        asks for gzip and requires it; otherwise the answer must not be encoded."""
        global checked
        checked += 1
        status, headers, raw, error = self._send("GET", path, headers={"Accept-Encoding": "gzip"} if compressed else None)
        if error:
            failures.append(f"GET {path}: no response ({error})")
            return 0, headers, b"", b""
        encoding = (headers.get("Content-Encoding") or "").lower()
        if compressed:
            expect(encoding == "gzip", f"GET {path} with Accept-Encoding gzip: Content-Encoding {encoding!r}")
            expect("accept-encoding" in (headers.get("Vary") or "").lower(), f"GET {path}: no Vary: Accept-Encoding")
        else:
            expect(encoding in ("", "identity"), f"GET {path} without Accept-Encoding: Content-Encoding {encoding!r}")
        try:
            return status, headers, raw, gzip.decompress(raw) if encoding == "gzip" else raw
        except OSError:
            failures.append(f"GET {path}: not gzip")
            return status, headers, raw, b""

    def control(self, path, body=None):
        """POST a test control: hub-sim's /sim/<path> (200) or the mock's /api/v1/__mock/<path>
        (204, or 200 with the settings, like hub-sim's). Anything else is a failure. Returns
        whether it worked."""
        url, ok = ("/api/v1/__mock/", (200, 204)) if MOCK else ("/sim/", (200,))
        status, reply = self.host_call("POST", url + path, body)
        expect(status in ok, f"control {path}: {status} {reply}")
        return status in ok

    def headers_only(self, method, path, content_length):
        """Send a request's headers but none of the body: announcing
        `content_length` JSON bytes, or (None) no Content-Length at all, which
        urllib can't do. Returns (status, json) of the early answer, checked
        against the Error schema."""
        url = urllib.parse.urlsplit(self.base)
        conn = http.client.HTTPConnection(url.hostname, url.port, timeout=10)
        try:
            conn.putrequest(method, "/api/v1" + path, skip_host=True)
            conn.putheader("Host", "127.0.0.1")
            if content_length is not None:
                conn.putheader("Content-Type", "application/json")
                conn.putheader("Content-Length", str(content_length))
            if self.token:
                conn.putheader("Authorization", "Bearer " + self.token)
            elif self.cookie:
                conn.putheader("Cookie", "hubbo_session=" + self.cookie)
            conn.endheaders()
            r = conn.getresponse()
            text = r.read().decode()
        finally:
            conn.close()
        global checked
        checked += 1
        try:
            body = json.loads(text) if text else None
        except ValueError:
            body = None
        for p in self.checker.ref_errors("#/components/schemas/Error", body):
            failures.append(f"{method} {path} (headers only) -> {r.status}: {p}")
        return r.status, body

    @contextlib.contextmanager
    def acting_as(self, token=None, cookie=None):
        """Temporarily switch credentials (restored afterwards)."""
        saved = (self.token, self.cookie)
        self.token, self.cookie = token, cookie
        try:
            yield self
        finally:
            self.token, self.cookie = saved


def revisions(value, path=""):
    """(path, value) of every revision / current_revision in a JSON value."""
    if isinstance(value, dict):
        for k, x in value.items():
            if k in ("revision", "current_revision"):
                yield f"{path}/{k}", x
            yield from revisions(x, f"{path}/{k}")
    elif isinstance(value, list):
        for i, x in enumerate(value):
            yield from revisions(x, f"{path}/{i}")


def wait_for(base, seconds=10):
    deadline = time.monotonic() + seconds
    while True:
        try:
            req = urllib.request.Request(base.rstrip("/") + "/api/v1/auth/session", headers={"Host": "127.0.0.1"})
            urllib.request.urlopen(req, timeout=1).close()
            return
        except (urllib.error.URLError, OSError):
            if time.monotonic() > deadline:
                raise SystemExit(f"the API at {base} did not come up")
            time.sleep(0.2)


def concrete(path, token_id="tok_00000000"):
    return (path.replace("{integration_id}", "ha_main").replace("{device_id}", "dev_denon")
            .replace("{activity_id}", "act_watch_apple_tv").replace("{token_id}", token_id)
            .replace("{entity_id}", "light.family_room"))


def operations(checker, **marker):
    """(op_id, METHOD, concrete path) for spec operations with all `marker` key/values."""
    for path, item in checker.spec["paths"].items():
        for method, op in item.items():
            if method in v.METHODS and all(op.get(k) == x for k, x in marker.items()):
                yield op["operationId"], method.upper(), path


def empty_body(checker, op_id):
    """`{}` for operations that take a JSON body, None (no body) for the rest."""
    body = checker.ops[op_id].get("requestBody", {}).get("content", {})
    return {} if "application/json" in body else None


def expect(cond, message):
    if not cond:
        failures.append(message)


# ---------------------------------------------------------------- flows

def registry_flow(c):
    """Reads: status, remote, devices, capabilities, integrations."""
    _, status, _ = c.call("getStatus", "GET", "/status", expect=200)
    c.call("getRemoteStatus", "GET", "/remote/status", expect=200)
    _, buttons, _ = c.call("listRemoteButtons", "GET", "/remote/buttons", expect=200)
    expect(buttons and len(buttons["buttons"]) == 48, "remote/buttons: expected 48 buttons")
    _, ints, _ = c.call("listIntegrations", "GET", "/integrations", expect=200)
    # Status carries each integration's state and detail, as the list does (one poll).
    summary = lambda body: {i["id"]: (i["state"], i.get("detail")) for i in (body or {}).get("integrations", [])}
    expect(summary(status) == summary(ints), f"status integrations {summary(status)} != list {summary(ints)}")
    _, listing, _ = c.call("listDevices", "GET", "/devices", expect=200)
    devices = (listing or {}).get("devices", [])
    expect(devices, "no devices registered in hub-sim")
    for d in devices:
        c.call("getDevice", "GET", f"/devices/{d['id']}", expect=200)
        _, caps, _ = c.call("getDeviceCapabilities", "GET", f"/devices/{d['id']}/capabilities", expect=200)
        # hub-sim boots from api/fixtures: the served capabilities must equal
        # the fixture the web mock serves, or the mock has drifted.
        if caps is not None and os.path.exists(os.path.join(FIXTURES, "devices", f"{d['id']}.capabilities.json")):
            expect_fixture_commands(d["id"], caps, f"devices/{d['id']}.capabilities.json")
        readable_labels(d["id"], caps)
    c.call("getDevice", "GET", "/devices/dev_nope", expect=404)
    c.call("getDeviceCapabilities", "GET", "/devices/dev_nope/capabilities", expect=404)
    devices_and_profiles(c, devices)


PROFILE_CONTENT = ("commands", "buttons", "power", "timing", "inputs", "channels")


def expect_fixture_commands(what, caps, name):
    """Served capabilities equal the fixture (the web mock serves it), whatever the device id."""
    expect(dict(caps or {}, device_id=None) == dict(fixture(name), device_id=None),
           f"{what}: served capabilities differ from api/fixtures (regenerate the fixture)")


def readable_labels(dev_id, caps):
    """No command is labelled with its raw id, and every integration-service
    param (`<platform>.<service>`) has a label."""
    for x in (caps or {}).get("commands", []):
        expect(x["label"] != x["command_id"], f"{dev_id}: {x['command_id']} is labelled with its id")
        platform = x["command_id"].split(".", 1)[0]
        if "." in x["command_id"] and platform not in HA_DOMAINS:
            for p in x["params"]:
                expect(p.get("label"), f"{dev_id}: {x['command_id']} param {p['name']} has no label")


# The entity domains with a curated allowlist (docs/ha-capabilities.md): the
# spec's discovery `domain` filter, which lint checks against core's list.
HA_DOMAINS = set(v.query_param(v.load_spec(), "getDiscovery", "domain")["schema"]["enum"])


def devices_and_profiles(c, devices):
    """The list is light; a device's detail carries its profile; HA devices
    have none of it."""
    for d in devices:
        expect(d.get("platform") == d["provider"]["type"], f"{d['id']}: platform isn't provider.type: {d}")
        expect(not any(k in d for k in PROFILE_CONTENT), f"{d['id']}: the list carries profile content")
        _, got, _ = c.call("getDevice", "GET", f"/devices/{d['id']}", expect=200, label="device detail")
        got = got or {}
        if d["provider"]["type"] == "ble_hid":
            expect("profile" in d and "commands" in got, f"{d['id']}: a Bluetooth device without its profile")
        else:
            expect(not any(k in got for k in PROFILE_CONTENT + ("profile", "manufacturer", "model")),
                   f"{d['id']}: an HA device with profile fields: {got}")
    _, apple, _ = c.call("getDevice", "GET", "/devices/dev_apple_tv", expect=200, label="fixture Apple TV")
    expect(apple == fixture("devices/dev_apple_tv.json"), "dev_apple_tv differs from api/fixtures")


def profiles_flow(c):
    """Admin: the built-in profiles and kinds equal api/fixtures."""
    _, profiles, _ = c.call("listProfiles", "GET", "/profiles", expect=200)
    expect(profiles == fixture("profiles/list.json"), "GET /profiles differs from api/fixtures")
    for p in (profiles or {}).get("profiles", []):
        slug = p["id"].split(":", 1)[1]
        _, doc, _ = c.call("getProfile", "GET", f"/profiles/{p['id']}", expect=200)
        expect(doc == fixture(f"profiles/{slug}.json"), f"GET /profiles/{p['id']} differs from api/fixtures")
        expect("meta" not in (doc or {}), f"{p['id']}: served with meta")
    c.call("getProfile", "GET", "/profiles/hubbo%3Aapple-tv", expect=200, label="profile id percent-encoded")
    c.call("getProfile", "GET", "/profiles/hubbo:roku", expect=404, label="unknown profile")
    _, kinds, _ = c.call("listKinds", "GET", "/kinds", expect=200)
    expect(kinds == fixture("profiles/kinds.json"), "GET /kinds differs from api/fixtures")


def execute_flow(c):
    # Execute: the fixture request against the fixture device.
    c.call("executeCommand", "POST", "/devices/dev_denon/commands/media_player.volume_set/execute",
           fixture("devices/execute.volume_set.request.json"), expect=200)
    c.call("executeCommand", "POST", "/devices/dev_denon/commands/media_player.volume_set/execute",
           {"params": {"level": 1.5}}, expect=422, label="execute out of range")
    c.call("executeCommand", "POST", "/devices/dev_denon/commands/media_player.volume_set/execute",
           {"params": {"level": 0.3, "entity_id": "all"}}, expect=422, label="execute entity_id")
    c.call("executeCommand", "POST", "/devices/dev_denon/commands/media_player.play_media/execute",
           {"params": {}}, expect=422, label="execute unknown command")
    c.call("executeCommand", "POST", "/devices/dev_nope/commands/media_player.turn_on/execute",
           {"params": {}}, expect=404, label="execute unknown device")
    # Holds are for bindings and waits for sequences: execute refuses both.
    caps = c.call("getDeviceCapabilities", "GET", "/devices/dev_apple_tv/capabilities", expect=200)[1] or {}
    commands = caps.get("commands", [])
    groups = {x["command_id"]: x.get("group") for x in commands}
    expect(groups.get("press") == "remote" and groups.get("key_tap") == "keys"
           and groups.get("usage_tap") == "custom" and groups.get("wait_connected") is None,
           f"Bluetooth command groups: {groups}")
    refused = {"hold": "hold_not_allowed", "wait": "sequence_only"}
    expect({"key_down", "press_down", "usage_down"} <= {x["command_id"] for x in commands if x["effect"] == "hold"},
           f"the *_down commands should be holds: {commands}")
    for x in commands:
        if x["effect"] not in refused:
            continue
        _, body, _ = c.call("executeCommand", "POST", f"/devices/dev_apple_tv/commands/{x['command_id']}/execute",
                            {"params": sample_params(x)}, expect=422, label=f"execute {x['effect']} {x['command_id']}")
        codes = [d["code"] for d in error_details(body)]
        expect(refused[x["effect"]] in codes, f"execute {x['command_id']}: expected {refused[x['effect']]}, got {codes}")
    for command, params in [("press", {"command": "Select"}), ("key_tap", {"key": "KEY_A"}),
                            ("usage_tap", {"usage": 0xCD})]:
        c.call("executeCommand", "POST", f"/devices/dev_apple_tv/commands/{command}/execute",
               {"params": params}, expect=200, label=f"execute {command}")
    # A timed press answers once the key is down; the hub releases it later.
    _, held, _ = c.call("executeCommand", "POST", "/devices/dev_apple_tv/commands/press/execute",
                        {"params": {"command": "Home", "hold_ms": 2000}}, expect=200, label="execute a timed press")
    expect((held or {}).get("status") == "accepted" and (held or {}).get("elapsed_ms", 2000) < 2000,
           f"a timed press should answer at key down: {held}")
    for command, params, label in [("press", {"command": "Netflix"}, "a command the profile doesn't have"),
                                   ("press", {"command": "Select", "hold_ms": 10001}, "hold_ms over 10 s"),
                                   ("usage_tap", {"usage": 1024}, "a usage past 0x3FF")]:
        c.call("executeCommand", "POST", f"/devices/dev_apple_tv/commands/{command}/execute",
               {"params": params}, expect=422, label=f"execute {label}")
    # wait_connected belongs in a sequence: fine in startup, refused in a binding.
    wait = {"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "wait_connected",
            "params": {"timeout_ms": 15000}}
    act = {"name": "Wait check", "startup": [wait], "shutdown": [], "bindings": {}, "macros": []}
    _, ok, _ = c.call("validateActivity", "POST", "/activities/validate", act, expect=200,
                      label="wait_connected in startup")
    expect((ok or {}).get("valid") is True, f"wait_connected in startup: {ok}")
    _, bad, _ = c.call("validateActivity", "POST", "/activities/validate",
                       dict(act, startup=[], bindings={"select:tap": [wait]}), expect=200,
                       label="wait_connected in a binding")
    codes = [e["code"] for e in (bad or {}).get("errors", [])]
    expect("sequence_only" in codes, f"wait_connected in a binding: {bad}")


def sample_params(command):
    """Valid params for a command: each required param's first option or minimum."""
    out = {}
    for p in command["params"]:
        if p["required"]:
            out[p["name"]] = p["options"][0] if p["type"] == "enum" else p.get("minimum", True)
    return out


def activity_flow(c):
    c.call("listActivities", "GET", "/activities", expect=200)
    c.call("getGlobalBindings", "GET", "/global-bindings", expect=200)
    create = fixture("activities/create.request.json")
    _, result, _ = c.call("validateActivity", "POST", "/activities/validate", create, expect=200)
    expect(result and result.get("valid") is True, f"validate fixture: {result}")
    bad = dict(create, startup=[{"integration_id": "ha_main", "device_id": "dev_denon",
                                 "command_id": "media_player.volume_set", "params": {"level": 7}}])
    _, result, _ = c.call("validateActivity", "POST", "/activities/validate", bad, expect=200,
                          label="validate invalid")
    expect(result and result.get("valid") is False and result["errors"], "validate should report errors")

    for name in sorted(os.listdir(os.path.join(FIXTURES, "malicious"))):
        status, _, _ = c.call("createActivity", "POST", "/activities", fixture("malicious/" + name),
                              label=f"malicious {name}")
        expect(status in (400, 422), f"malicious {name}: expected 400/422, got {status}")
    c.call("createActivity", "POST", "/activities", raw=b"{not json", expect=400,
           headers={"Content-Type": "application/json"}, label="malformed json")
    c.call("createActivity", "POST", "/activities", raw=json.dumps(create).encode(), expect=415,
           headers={"Content-Type": "text/plain"}, label="wrong content type")
    c.call("createActivity", "POST", "/activities", raw=b"[" * 40 + b"]" * 40, expect=400,
           headers={"Content-Type": "application/json"}, label="too deep")
    c.call("createActivity", "POST", "/activities", raw=b" " * 40000, expect=413,
           headers={"Content-Type": "application/json"}, label="too large")

    status, created, headers = c.call("createActivity", "POST", "/activities", create, expect=201)
    if status != 201:
        return
    act_id = created["id"]
    # One generation counter: a revision is the config generation of the
    # object's last change, so it equals /status config.generation right now.
    first = created["revision"]
    expect(first == config_state(c).get("generation"), "a new activity's revision is the current generation")
    _, got, _ = c.call("getActivity", "GET", f"/activities/{act_id}", expect=200)

    edited = dict(got, name="Play Xbox (edited)")
    _, replaced, _ = c.call("replaceActivity", "PUT", f"/activities/{act_id}", edited, expect=200)
    current = replaced["revision"] if replaced else first + 1
    expect(current > first, "replace should bump the revision")
    _, conflict, _ = c.call("replaceActivity", "PUT", f"/activities/{act_id}", edited, expect=409,
                            label="stale revision")
    expect(conflict and conflict["error"].get("current_revision") == current, "conflict must carry current_revision")
    no_rev = {k: x for k, x in edited.items() if k != "revision"}
    c.call("replaceActivity", "PUT", f"/activities/{act_id}", no_rev, expect=422, label="missing revision")
    c.call("replaceActivity", "PUT", "/activities/act_nope", dict(edited, id="act_nope"), expect=404)

    _, t, _ = c.call("activateActivity", "POST", f"/activities/{act_id}/activate", expect=200)
    expect(t and t["switched"] and t["active_activity_id"] == act_id and t["startup"]["status"] == "queued",
           f"activate result: {t}")
    _, st, _ = c.call("getStatus", "GET", "/status", expect=200)
    expect(st and st["active_activity_id"] == act_id, "status should show the active activity")
    _, off, _ = c.call("activitiesOff", "POST", "/activities/off", expect=200)
    expect(off and off["switched"] and off["active_activity_id"] is None and off["previous_activity_id"] == act_id,
           f"off result: {off}")
    _, again, _ = c.call("activitiesOff", "POST", "/activities/off", expect=200, label="off with nothing active")
    expect(again and not again["switched"] and again["shutdown"]["status"] == "none", f"second off: {again}")
    c.call("activateActivity", "POST", "/activities/act_nope/activate", expect=404)

    gb = c.call("getGlobalBindings", "GET", "/global-bindings", expect=200)[1]
    if gb:
        c.call("replaceGlobalBindings", "PUT", "/global-bindings", gb, expect=200)
        c.call("replaceGlobalBindings", "PUT", "/global-bindings", gb, expect=409, label="stale global bindings")

    c.call("deleteActivity", "DELETE", f"/activities/{act_id}?revision={first}", expect=409, label="delete stale")
    c.call("deleteActivity", "DELETE", f"/activities/{act_id}", expect=422, label="delete without revision")
    c.call("deleteActivity", "DELETE", f"/activities/{act_id}?revision={current}", expect=204)
    c.call("getActivity", "GET", f"/activities/{act_id}", expect=404)


def admin_flow(c, checker, candidate=None):
    reg = fixture("admin/register_device.request.json")
    reg = registration_from(c, reg, candidate)  # an HA device is registered from its loaded details
    status, dev, _ = c.call("registerDevice", "POST", "/admin/devices", reg, expect=201)
    if status == 201:
        dev_id = dev["id"]
        _, caps, _ = c.call("getDeviceCapabilities", "GET", f"/devices/{dev_id}/capabilities", expect=200)
        readable_labels(dev_id, caps)
        _, upd, _ = c.call("updateDevice", "PUT", f"/admin/devices/{dev_id}",
                           {"name": "Backlight", "revision": dev["revision"]}, expect=200)
        c.call("updateDevice", "PUT", f"/admin/devices/{dev_id}", {"name": "x", "revision": 99}, expect=409,
               label="device stale revision")
        # Rule breaks are refused before the save (422), never a failed save (500).
        rev = upd["revision"] if upd else 2
        for body, label, code, path in [
            ({"kind": "Not A Kind", "revision": rev}, "update to a malformed kind", "unknown_kind", "/kind"),
            ({"kind": "toaster", "revision": rev}, "update to an unknown kind", "unknown_kind", "/kind"),
            ({"provider": {"type": "ble_hid"}, "revision": rev}, "update to another platform",
             "integration_mismatch", "/provider/type"),
        ]:
            _, e, _ = c.call("updateDevice", "PUT", f"/admin/devices/{dev_id}", body, expect=422, label=label)
            got = [(d["code"], d["path"]) for d in error_details(e)]
            expect((code, path) in got, f"{label}: expected {code} at {path}, got {got}")
        c.call("deleteDevice", "DELETE", f"/admin/devices/{dev_id}?revision={rev}", expect=204)
    bad = dict(reg, provider={"type": "home_assistant", "entity_id": "light.all"})
    c.call("registerDevice", "POST", "/admin/devices", bad, expect=422, label="register broadcast entity")
    # A device used by activities can't be deleted.
    _, d, _ = c.call("getDevice", "GET", "/devices/dev_denon", expect=200)
    if d:
        c.call("deleteDevice", "DELETE", f"/admin/devices/dev_denon?revision={d['revision']}", expect=409,
               label="delete referenced device")

    # Tokens need a real admin account, even for the simulator's no-auth admin.
    session = c.call("getSession", "GET", "/auth/session", expect=200)[1] or {}
    if session.get("auth_state") == "fresh":
        c.call("createToken", "POST", "/admin/tokens", {"name": "too early"}, expect=409, label="token before setup")
        c.call("setupAdmin", "POST", "/auth/setup", {"username": ADMIN, "password": PASSWORD}, expect=201)
    _, tok, _ = c.call("createToken", "POST", "/admin/tokens", {"name": "contract test"}, expect=201)
    c.call("listTokens", "GET", "/admin/tokens", expect=200)
    if tok and "id" in tok:
        c.call("revokeToken", "DELETE", f"/admin/tokens/{tok['id']}", expect=204)
        c.call("revokeToken", "DELETE", f"/admin/tokens/{tok['id']}", expect=404, label="revoke twice")

    # Backup round trip: download, dry run, restore.
    _, archive, headers = c.call("downloadBackup", "GET", "/admin/backup", expect=200)
    if archive:
        expect("attachment" in (headers.get("Content-Disposition") or ""), "backup should be an attachment")
        _, dry, _ = c.call("restoreBackup", "POST", "/admin/restore?dry_run=true", archive, expect=200)
        expect(dry and dry["dry_run"] is True, f"dry run: {dry}")
        c.call("restoreBackup", "POST", "/admin/restore", archive, expect=200)
        c.call("restoreBackup", "POST", "/admin/restore", {"format": "not-a-backup"}, expect=422,
               label="restore garbage")
        c.call("restoreBackup", "POST", "/admin/restore", fixture("admin/restore.request.json"), expect=200,
               label="restore fixture")

    firmware_flow(c)
    firmware_release_flow(c)


# ---------------------------------------------------------------- firmware

SIM_BOARD = "simulator"   # hub-sim's board (simulatorFirmwareTarget())
SIM_SLOT = 64 * 1024      # and its slot
SIM_WRITE_MS, SIM_REBOOT_MS = 2000, 1000  # SimulatedFirmwareUpdate::kWriteMs, kRebootMs
SIM_FACTORY_RESET_MS = 1000  # SimulatedFactoryReset::kRestartMs
SIM_RESTART_MS = 1000  # SimulatedRestart::kRestartMs (POST /admin/restart)
CHECK_MS = 500  # SimFirmwareReleases::kCheckMs (the mock: FW_CHECK_MS, set below)
DOWNLOAD_MS = 1000  # SimFirmwareReleases::kDownloadMs (the mock: FW_DOWNLOAD_MS)


def firmware_image(board=SIM_BOARD, layout=1, version="v9.9.9-contract", magic=0xE9, chip=9, min_rev=0,
                   desc_magic=0xABCD5432, hash_ok=True, pad_to=512):
    """A fake image in the real app .bin layout (core/tests/firmware_image_util.h)."""
    import hashlib
    b = bytearray(32)
    b[0] = magic
    b[12:14] = chip.to_bytes(2, "little")
    b[15:17] = min_rev.to_bytes(2, "little")
    b[23] = 1
    desc = bytearray(256)
    desc[0:4] = desc_magic.to_bytes(4, "little")
    b += desc
    if board is not None:
        b += f"HUBBO-IMAGE/1 board={board} layout={layout} version={version}".encode() + b"\0"
    if len(b) < pad_to:
        b += b"\xff" * (pad_to - len(b))
    digest = bytearray(hashlib.sha256(b).digest())
    if not hash_ok:
        digest[0] ^= 1
    return bytes(b + digest)


def upload(c, image, expect, label, content_type="application/octet-stream"):
    status, body, _ = c.call("updateFirmware", "POST", "/admin/firmware/update", raw=image,
                             headers={"Content-Type": content_type}, expect=expect, label=label)
    return body


def error_code(body):
    return (body or {}).get("error", {}).get("code")


def error_details(body):
    return (body or {}).get("error", {}).get("details") or []


def firmware_state(c):
    _, s, _ = c.call("getFirmwareStatus", "GET", "/admin/firmware", expect=200)
    return (s or {}).get("update", {})


def firmware_flow(c):
    """Every refusal changes nothing; a good image writes, reboots and ends idle.
    The reboot ends any session, so a session client logs in again; returns
    the cookie it ends with."""
    _, s, _ = c.call("getFirmwareStatus", "GET", "/admin/firmware", expect=200)
    expect(s and s["update"]["state"] == "idle", f"firmware status: {s}")
    if MOCK:
        skip("firmware image checks and states", "the mock's fake-image rules are its own")
        return c.cookie
    refusals = [
        (firmware_image(pad_to=SIM_SLOT), 413, "payload_too_large", "image over the slot"),
        (b"not an image at all", 422, "invalid_image", "not an image"),
        (firmware_image(magic=0), 422, "invalid_image", "bad magic"),
        (firmware_image(chip=2), 422, "invalid_image", "another chip"),
        (firmware_image(min_rev=99), 422, "invalid_image", "newer chip revision"),
        (firmware_image(desc_magic=0), 422, "invalid_image", "merged factory image"),
        (firmware_image(board=None), 422, "invalid_image", "not a Hubbo image"),
        (firmware_image(board="waveshare_esp32_s3_eth"), 422, "wrong_board", "another board"),
        (firmware_image(layout=2), 422, "invalid_image", "another layout"),
        (firmware_image(hash_ok=False), 422, "invalid_image", "bad checksum"),
    ]
    for image, status, code, label in refusals:
        body = upload(c, image, status, label)
        expect(error_code(body) == code, f"{label}: expected {code}, got {body}")
    body = upload(c, firmware_image(), 415, "image as JSON", content_type="application/json")
    expect(error_code(body) == "unsupported_media_type", f"image as JSON: {body}")
    status, body = c.headers_only("POST", "/admin/firmware/update", None)
    expect(status == 411 and error_code(body) == "length_required", f"no Content-Length: {status} {body}")
    expect(firmware_state(c).get("state") == "idle", "a refused image must change nothing")

    body = upload(c, firmware_image(), 202, "good image")
    expect(body and body["update"]["state"] == "writing", f"accepted: {body}")
    body = upload(c, firmware_image(), 409, "second upload")
    expect(error_code(body) == "update_in_progress", f"second upload: {body}")
    c.control("advance?ms=1000")
    st = firmware_state(c)
    expect(st.get("state") == "writing" and 0 < st.get("progress", 0) < 100, f"halfway: {st}")
    c.control("advance?ms=1000")
    expect(firmware_state(c).get("state") == "rebooting", "written: rebooting")
    upload(c, firmware_image(), 409, "upload while rebooting")
    c.control("advance?ms=1000")
    relogin_after_restart(c, "the firmware reboot")
    expect(firmware_state(c).get("state") == "idle", "after the simulated reboot")

    # A write that fails, then the hub can't take an update at all.
    if c.control("firmware?fail=1&ready=1"):
        upload(c, firmware_image(), 202, "image whose write fails")
        c.control("advance?ms=2000")
        st = firmware_state(c)
        expect(st.get("state") == "failed" and st.get("error") == "write_failed", f"failed write: {st}")
        c.control("firmware?fail=0&ready=0")
        body = upload(c, firmware_image(), 503, "no update support")
        expect(error_code(body) == "unavailable", f"not ready: {body}")
        c.control("firmware?fail=0&ready=1")
    return c.cookie


def admin_login(c, label):
    """Logs the admin in (expecting 200). Returns the session cookie, or None."""
    status, _, headers = c.call("login", "POST", "/auth/login", {"username": ADMIN, "password": PASSWORD},
                                expect=200, label=label)
    return http.cookies.SimpleCookie(headers.get("Set-Cookie"))["hubbo_session"].value if status == 200 else None


def release_check(c, label, probe_busy=False):
    """Check now, let the check finish, and return GET /admin/firmware.
    `probe_busy`: a second check while it runs is 409 busy."""
    _, body, _ = c.call("checkFirmwareRelease", "POST", "/admin/firmware/check", expect=202, label=label)
    expect(body and body["checking"] is True, f"{label}: {body}")
    if probe_busy:
        _, body, _ = c.call("checkFirmwareRelease", "POST", "/admin/firmware/check", expect=409,
                            label="check while one runs")
        expect(error_code(body) == "busy", f"second check: {body}")
    c.control(f"advance?ms={CHECK_MS}")
    _, s, _ = c.call("getFirmwareStatus", "GET", "/admin/firmware", expect=200, label=f"{label}: done")
    expect(s and s["checking"] is False and isinstance(s["checked_ago_ms"], int), f"{label}: still checking: {s}")
    return s or {}


def available_version(s):
    return (s.get("available") or {}).get("version")


def firmware_release_flow(c):
    """Online updates against the fake feed (hub-sim's /sim/release, the
    mock's /__mock/release): a check finds a release, failed checks report
    and keep it, releases this hub can't take aren't offered, and an install
    downloads, writes and reboots. Ends with no release published and the
    running version it started with. Returns the cookie it ends with."""
    _, s, _ = c.call("getFirmwareStatus", "GET", "/admin/firmware", expect=200, label="release fields")
    expect(s and all(k in s for k in ("available", "checking", "checked_ago_ms", "check_error", "rolled_back_from")),
           f"release fields: {s}")
    started_on = (s or {}).get("version", "0.0.0-sim")
    board = (s or {}).get("board", SIM_BOARD)  # the mock's is the real board's
    c.control("release?none=1&running=1.3.0")
    s = release_check(c, "check with no release")
    # GitHub's 404: unreachable, and `available` stays what the last good check found.
    expect(s["check_error"] == "unreachable", f"no release: {s}")
    good = {"version": "1.4.0", "board": board, "layout": "1", "min_version": ""}
    c.control("release?" + urllib.parse.urlencode(dict(good, fault="none")))
    s = release_check(c, "check now", probe_busy=True)
    expect(s["check_error"] is None and s["available"] == {"version": "1.4.0"}, f"release found: {s}")
    # A failed check reports why and keeps the release from the last good one.
    for fault in ("unreachable", "bad_signature", "bad_manifest"):
        c.control(f"release?fault={fault}")
        s = release_check(c, f"check: {fault}")
        expect(s["check_error"] == fault and available_version(s) == "1.4.0", f"{fault}: {s}")
    c.control("release?fault=none")
    # Releases this hub can't take aren't offered (a good check, no error).
    for fields, label in (({"board": "other_board"}, "another board"), ({"layout": "2"}, "another layout"),
                          ({"version": "1.3.0"}, "not newer"), ({"min_version": "1.3.5"}, "needs a newer base")):
        c.control("release?" + urllib.parse.urlencode(dict(good, **fields)))
        s = release_check(c, f"check: {label}")
        expect(s["check_error"] is None and s["available"] is None, f"{label}: {s}")
    c.control("release?" + urllib.parse.urlencode(dict(good, running="0.0.0-sim")))
    s = release_check(c, "check from a development build")
    expect(s["available"] is None, f"a development build is never offered one: {s}")
    c.control("release?running=1.3.0")
    s = release_check(c, "check again")
    expect(available_version(s) == "1.4.0", f"available again: {s}")

    def install(version, expect_status, label):
        return c.call("installFirmwareRelease", "POST", "/admin/firmware/install", {"version": version},
                      expect=expect_status, label=label)[1]

    body = install("1.5.0", 409, "install another version")
    expect(error_code(body) == "not_available", f"install 1.5.0: {body}")
    body = install("v1.4.0", 422, "install a version that isn't X.Y.Z")
    expect(("pattern", "/version") in [(d["code"], d["path"]) for d in error_details(body)], f"v1.4.0: {body}")
    c.call("installFirmwareRelease", "POST", "/admin/firmware/install", {}, expect=422, label="install no version")
    c.control("release?rolled_back_from=1.3.9")
    body = install("1.4.0", 202, "install 1.4.0")
    expect(body and body.get("update", {}).get("state") == "downloading" and body.get("rolled_back_from") is None,
           f"install: {body}")
    body = install("1.4.0", 409, "install during a download")
    expect(error_code(body) == "update_in_progress", f"second install: {body}")
    body = upload(c, firmware_image(board=board), 409, "upload during a download")
    expect(error_code(body) == "update_in_progress", f"upload during a download: {body}")
    c.control(f"advance?ms={DOWNLOAD_MS}")
    expect(firmware_state(c).get("state") == "writing", "downloaded: writing")
    for _ in range(SIM_WRITE_MS // 250):  # the write's own time, however the download's step counted
        if firmware_state(c).get("state") != "writing":
            break
        c.control("advance?ms=250")
    expect(firmware_state(c).get("state") == "rebooting", "written: rebooting")
    c.control(f"advance?ms={SIM_RESTART_MS}")
    relogin_after_restart(c, "the release's reboot")
    _, s, _ = c.call("getFirmwareStatus", "GET", "/admin/firmware", expect=200, label="after the release's reboot")
    expect(s and s["update"]["state"] == "idle" and s["version"] == "1.4.0" and s["available"] is None,
           f"running the release: {s}")

    # A rollback shows until the next update; a failed download fails the update.
    c.control("release?rolled_back_from=1.5.0")
    _, s, _ = c.call("getFirmwareStatus", "GET", "/admin/firmware", expect=200, label="after a rollback")
    expect(s and s["rolled_back_from"] == "1.5.0", f"rolled back: {s}")
    c.control("release?version=1.5.0&running=1.4.0")
    expect(available_version(release_check(c, "check for 1.5.0")) == "1.5.0", "1.5.0 available")
    c.control("release?download=fail" if MOCK else "release?fault=unreachable")
    install("1.5.0", 202, "install whose download fails")
    c.control(f"advance?ms={DOWNLOAD_MS}")
    st = firmware_state(c)
    expect(st.get("state") == "failed" and st.get("error") == "download_failed", f"failed download: {st}")
    c.control(f"release?none=1&fault=none&running={started_on}&rolled_back_from="
              + ("&download=ok" if MOCK else ""))
    return c.cookie


def relogin_after_restart(c, what):
    """A restart drops every session, as on the board: the old cookie gets
    401, then log in again (c.cookie). No-op without a session (noauth)."""
    if not c.cookie:
        return
    c.call("listDevices", "GET", "/devices", expect=401, label=f"old session after {what}")
    c.cookie = admin_login(c, f"login after {what}") or c.cookie


def factory_reset_flow(c):
    """The real factory reset: refused during a firmware update, 202, then the
    simulated restart erases everything and setup is open again. Runs last in
    each mode's main flow (it erases the admin, tokens and config), and sets
    the admin up again with the same login."""
    _, fw, _ = c.call("getFirmwareStatus", "GET", "/admin/firmware", expect=200, label="board before a factory reset")
    upload(c, firmware_image(board=(fw or {}).get("board", SIM_BOARD)), 202, "image before a factory reset")
    _, body, _ = c.call("factoryReset", "POST", "/admin/factory-reset", expect=409, label="factory reset mid-update")
    expect(error_code(body) == "update_in_progress", f"factory reset mid-update: {body}")
    _, body, _ = c.call("restartHub", "POST", "/admin/restart", expect=409, label="restart mid-update")
    expect(error_code(body) == "update_in_progress", f"restart mid-update: {body}")
    c.control(f"advance?ms={SIM_WRITE_MS + SIM_REBOOT_MS}")
    relogin_after_restart(c, "the firmware reboot")
    if c.control("factory-reset?ready=0"):
        _, body, _ = c.call("factoryReset", "POST", "/admin/factory-reset", expect=503, label="no factory reset")
        expect(error_code(body) == "unavailable", f"factory reset not ready: {body}")
        c.control("factory-reset?ready=1")

    c.call("factoryReset", "POST", "/admin/factory-reset", expect=202)
    c.control(f"advance?ms={SIM_FACTORY_RESET_MS}")
    session_mode = bool(c.cookie)
    with c.acting_as():
        s = c.call("getSession", "GET", "/auth/session", expect=200, label="session after a factory reset")[1] or {}
        # setup_required stays false for the simulator's no-auth admin.
        expect(s.get("auth_state") == "fresh" and s.get("setup_required") is session_mode,
               f"after a factory reset: {s}")
        if session_mode:
            c.call("login", "POST", "/auth/login", {"username": ADMIN, "password": PASSWORD}, expect=409,
                   label="old login after a factory reset")
    if session_mode:
        c.call("listDevices", "GET", "/devices", expect=401, label="old session after a factory reset")
    else:  # noauth: still the simulated admin, on a fresh hub
        _, st, _ = c.call("getStatus", "GET", "/status", expect=200, label="status after a factory reset")
        expect(st and st["auth_state"] == "fresh", f"status after a factory reset: {st}")
    with c.acting_as():
        c.call("setupAdmin", "POST", "/auth/setup", {"username": ADMIN, "password": PASSWORD}, expect=201,
               label="setup after a factory reset")
        cookie = admin_login(c, "login after a factory reset") if session_mode else None
    if session_mode and cookie is None:
        return
    c.cookie = cookie or c.cookie
    _, tokens, _ = c.call("listTokens", "GET", "/admin/tokens", expect=200, label="tokens after a factory reset")
    expect(tokens is not None and not tokens["tokens"], f"tokens survived a factory reset: {tokens}")
    hub = get_hub(c, "hub after a factory reset")
    expect_default_hub(hub, "the hub's name after a factory reset")
    if DEFAULT_HUB:
        expect(hub == DEFAULT_HUB, f"a factory reset should bring back {DEFAULT_HUB}: {hub}")


# ---------------------------------------------------------------- discovery

def error_docs(body):
    return (body or {}).get("error", {}).get("docs")


def discovery_flow(c):
    """With no credentials: the index, the served spec (equal to api/openapi.yaml)
    and guide (equal to docs/agent-api.md), plain and gzipped, /llms.txt, and the
    docs hint on an unknown path but not on a missing id."""
    with c.acting_as():
        _, index, _ = c.call("getApiIndex", "GET", "/", expect=200, label="API index")
        index = index or {}
        expect("hub" not in index, "the index names the hub")
        _, spec, _ = c.call("getOpenApi", "GET", "/openapi.json", expect=200, label="served spec")
        expect(spec == json.loads(json.dumps(c.checker.spec, default=str)),
               "GET /openapi.json differs from api/openapi.yaml")
        _, guide, _ = c.call("getAgentGuide", "GET", "/agent.md", expect=200, label="served guide")
        with open(GUIDE_PATH, encoding="utf-8") as f:
            expect(guide == f.read(), "GET /agent.md differs from docs/agent-api.md")
        # The index's links (the spec fixes them) resolve, plain and gzipped alike.
        for link in (GUIDE, SPEC_LINK):
            status, _, _, identity = c.fetch(link)
            zstatus, _, raw, body = c.fetch(link, compressed=True)
            expect(status == zstatus == 200 and body == identity and len(raw) < len(identity),
                   f"GET {link}: {status}, gzipped {zstatus}, {len(raw)} bytes for {len(identity)}")
        status, headers, _, llms = c.fetch("/llms.txt")
        expect(status == 200 and (headers.get("Content-Type") or "").startswith("text/"), f"/llms.txt: {status}")
        with open(LLMS_PATH, "rb") as f:
            expect(llms == f.read(), "GET /llms.txt differs from docs/llms.txt")
        text = llms.decode("utf-8", "replace")
        expect(f"({GUIDE})" in text and f"({SPEC_LINK})" in text, f"/llms.txt should link the guide and the spec: {text[:200]}")
        status, e = c.host_call("GET", "/api/v1/no-such-thing")
        for p in c.checker.ref_errors("#/components/schemas/Error", e):
            failures.append(f"GET /api/v1/no-such-thing: {p}")
        expect(status == 404 and error_code(e) == "not_found" and error_docs(e) == GUIDE,
               f"an unknown path should be 404 not_found with docs {GUIDE}: {status} {e}")
    # A missing id under a real route is a plain 404: the hint is for paths the API doesn't have.
    _, e, _ = c.call("getDevice", "GET", "/devices/dev_no_such_device", expect=404, label="missing device")
    expect(error_docs(e) is None, f"a missing id's 404 has the docs hint: {e}")


# ---------------------------------------------------------------- hub name, restart

# A hub never renamed is "Hubbo XXXX" (its MAC's end); hub_flow records it,
# and a factory reset brings exactly it back.
DEFAULT_HUB_NAME = re.compile(r"^Hubbo [0-9A-F]{4}$")
DEFAULT_HUB = {}  # hub-sim's name before hub_flow renames it (the mock's seeded hub is already renamed)


def expect_default_hub(hub, label):
    """A hub never renamed: "Hubbo XXXX", its derived hostname, no conflict."""
    expect(DEFAULT_HUB_NAME.match(hub.get("name", "")) and hub.get("hostname") == hub["name"].lower().replace(" ", "-")
           and hub.get("hostname_conflict") is False, f"{label}: {hub}")


def get_hub(c, label):
    return c.call("getHub", "GET", "/admin/hub", expect=200, label=label)[1] or {}


def rename_hub(c, name, hostname, label, sent=None):
    """PUT `sent` (default: `name`) and expect the hub to be called `name`."""
    _, hub, _ = c.call("updateHub", "PUT", "/admin/hub", {"name": sent or name}, expect=200, label=label)
    expect(hub == {"name": name, "hostname": hostname, "hostname_conflict": False, "check_updates": True,
                   "radio_power": "max"}, f"{label}: {hub}")


def hub_flow(c):
    """Naming the hub: the name, the hostname derived from it, the names
    refused, then a restart (202, every session gone, the name kept). Ends
    named "Living Room"."""
    if not MOCK:  # hub-sim starts never renamed
        hub = get_hub(c, "the hub's default name")
        expect_default_hub(hub, "a new hub's default name")
        DEFAULT_HUB.update(hub)
    rename_hub(c, "Living Room", "living-room", "rename the hub")
    _, st, _ = c.call("getStatus", "GET", "/status", expect=200, label="status after a rename")
    expect(st and st["hubbo"].get("name") == "Living Room", f"status hubbo.name after a rename: {st and st['hubbo']}")
    hostname = (st or {}).get("network", {}).get("hostname")
    expect(hostname in (None, "living-room"), f"status network.hostname after a rename: {hostname}")
    for name, hostname in [("Hubbo 2", "hubbo-2"), ("Den's TV!", "den-s-tv"), ("--Den--", "den"),
                           ("***", "hubbo"), ("x" * 32, "x" * 32), ("A", "a")]:
        rename_hub(c, name, hostname, f"hostname of {name!r}")
    # Spaces at either end are dropped before the checks.
    rename_hub(c, "Den", "den", "a padded name", sent="  Den  ")
    rename_hub(c, "x" * 32, "x" * 32, "a padded 32-character name", sent=" " + "x" * 32 + " ")
    for name, label, code in [("", "an empty name", "required"), ("   ", "a blank name", "required"),
                              ("x" * 33, "a 33-character name", "too_long"),
                              ("Den\x07", "a control character", "invalid_name"),
                              ("Den\nTV", "a newline", "invalid_name"), ("\tDen", "a tab", "invalid_name"),
                              ("Den\x7f", "DEL", "invalid_name")]:
        _, e, _ = c.call("updateHub", "PUT", "/admin/hub", {"name": name}, expect=422, label=f"hub name: {label}")
        got = [(d["code"], d["path"]) for d in error_details(e)]
        expect((code, "/name") in got, f"hub name {label}: expected {code} at /name, got {got}")
    for body, label in [({"name": 5}, "a number"), ({}, "an empty body"),
                        ({"name": "Den", "hostname": "den"}, "a hostname (read-only)")]:
        c.call("updateHub", "PUT", "/admin/hub", body, expect=422, label=f"hub name: {label}")
    expect(get_hub(c, "hub after refused names").get("name") == "x" * 32, "a refused name changed the hub")
    rename_hub(c, "Living Room", "living-room", "rename the hub back")
    rename_hub(c, "Living Room", "living-room", "the same name again")
    # "Check for updates": on by default, set alone (the name stays), back on.
    expect(get_hub(c, "check_updates by default").get("check_updates") is True, "check_updates is on by default")
    _, hub, _ = c.call("updateHub", "PUT", "/admin/hub", {"check_updates": False}, expect=200,
                       label="turn update checks off")
    expect(hub and hub["check_updates"] is False and hub["name"] == "Living Room", f"check_updates off: {hub}")
    c.call("updateHub", "PUT", "/admin/hub", {"check_updates": "no"}, expect=422, label="check_updates not a bool")
    _, hub, _ = c.call("updateHub", "PUT", "/admin/hub", {"name": "Living Room", "check_updates": True}, expect=200,
                       label="a name and check_updates together")
    expect(hub and hub["check_updates"] is True, f"check_updates back on: {hub}")
    # The remote radio's power: max by default, each level alone (the rest
    # stays), a bad one refused with nothing saved. persistence_flow restarts.
    expect(hub and hub["radio_power"] == "max", f"radio_power is max by default: {hub}")
    for level in ["low", "medium", "high", "max"]:
        _, hub, _ = c.call("updateHub", "PUT", "/admin/hub", {"radio_power": level}, expect=200,
                           label=f"radio_power {level}")
        expect(hub and hub["radio_power"] == level and hub["name"] == "Living Room" and hub["check_updates"] is True,
               f"radio_power {level}: {hub}")
    _, e, _ = c.call("updateHub", "PUT", "/admin/hub", {"name": "Den", "radio_power": "loud"}, expect=422,
                     label="radio_power: an unknown level")
    got = [(d["code"], d["path"]) for d in error_details(e)]
    expect(("invalid_enum_value", "/radio_power") in got, f"radio_power loud: expected invalid_enum_value, got {got}")
    hub = get_hub(c, "hub after refused radio_power")
    expect(hub.get("name") == "Living Room" and hub.get("radio_power") == "max", f"a refused radio_power saved: {hub}")

    _, body, _ = c.call("restartHub", "POST", "/admin/restart", expect=202)
    expect(body == {}, f"restart: {body}")
    c.control(f"advance?ms={SIM_RESTART_MS}")
    relogin_after_restart(c, "a restart")
    expect(get_hub(c, "hub after a restart").get("name") == "Living Room", "the name didn't survive a restart")


def until(fn, seconds=5):
    """Polls fn() until it returns something truthy (hub-sim's loop runs meanwhile)."""
    deadline = time.monotonic() + seconds
    while True:
        result = fn()
        if result or time.monotonic() > deadline:
            return result
        time.sleep(0.05)


def integration_state(c, integration_id):
    """An integration's state as /status reports it."""
    _, status, _ = c.call("getStatus", "GET", "/status", expect=200, label=f"{integration_id} state")
    return next((i["state"] for i in (status or {}).get("integrations", []) if i["id"] == integration_id), None)


def ha_flow(c):
    """HA settings (the token is write-only), async discovery, registration
    from it. Returns a registrable candidate (or None)."""
    _, cfg, _ = c.call("getIntegrationConfig", "GET", "/admin/integrations/ha_main", expect=200)
    if not cfg:
        return None
    c.call("getIntegrationConfig", "GET", "/admin/integrations/nope", expect=404)
    url = cfg["settings"]["url"]
    _, cfg, _ = c.call("configureIntegration", "PUT", "/admin/integrations/ha_main",
                       {"settings": {"type": "home_assistant", "url": url, "access_token": HA_TOKEN},
                        "revision": cfg["revision"]}, expect=200, label="set HA token")
    expect(cfg and cfg["settings"]["has_access_token"] is True, f"has_access_token after set: {cfg}")
    if not cfg:
        return None
    c.call("configureIntegration", "PUT", "/admin/integrations/ha_main",
           {"settings": {"type": "home_assistant", "url": "http://user@ha.local"}, "revision": cfg["revision"]},
           expect=422, label="HA URL with credentials")
    c.call("configureIntegration", "PUT", "/admin/integrations/ha_main",
           {"settings": {"type": "ble_hid", "device_name": "x"}, "revision": cfg["revision"]},
           expect=422, label="other integration's settings")
    c.call("configureIntegration", "PUT", "/admin/integrations/ha_main", {"name": "x", "revision": 999},
           expect=409, label="stale integration revision")
    _, again, _ = c.call("getIntegrationConfig", "GET", "/admin/integrations/ha_main", expect=200)
    expect(again and again["settings"]["has_access_token"] is True, "token kept")
    expect(until(lambda: integration_state(c, "ha_main") == "connected"), "fake HA never connected with the stored token")

    # The token is in no backup.
    c.call("downloadBackup", "GET", "/admin/backup", expect=200, label="backup after HA token")

    c.call("discoverIntegration", "POST", "/admin/integrations/ble/discover", expect=404, label="discover BLE")
    _, started, _ = c.call("discoverIntegration", "POST", "/admin/integrations/ha_main/discover", expect=202)
    expect(started and started["state"] in ("running", "done"), f"discover: {started}")

    done = wait_discovery(c)
    expect(done and done["state"] == "done", f"discovery didn't finish: {done}")
    if not done or done["state"] != "done":
        return None
    total = done["total"]
    expect(isinstance(done.get("elapsed_ms"), int), f"done carries elapsed_ms: {done}")
    expect(done.get("truncated") == 0, f"the fake HA's list should be complete: {done.get('truncated')}")
    expect(total == len(done["candidates"]) or total > 50, "candidates on one page")
    _, page, _ = c.call("getDiscovery", "GET", "/admin/integrations/ha_main/discover?limit=1", expect=200,
                        label="discovery page")
    if page and total > 1:
        expect(len(page["candidates"]) == 1 and page["next_cursor"], f"page: {page}")
        _, rest, _ = c.call("getDiscovery", "GET",
                            f"/admin/integrations/ha_main/discover?limit=100&cursor={page['next_cursor']}",
                            expect=200, label="discovery next page")
        expect(rest and len(rest["candidates"]) == min(total, 101) - 1, "second page has the rest")
    _, lights, _ = c.call("getDiscovery", "GET", "/admin/integrations/ha_main/discover?domain=light&q=FAMILY",
                          expect=200, label="discovery filter")
    expect(lights and all(x["domain"] == "light" for x in lights["candidates"]), "domain filter")
    # Query values are percent-decoded ("%5F" is "_"), per parameter.
    _, plain, _ = c.call("getDiscovery", "GET", "/admin/integrations/ha_main/discover?q=family_room", expect=200,
                         label="discovery q")
    _, coded, _ = c.call("getDiscovery", "GET", "/admin/integrations/ha_main/discover?q=family%5Froom",
                         expect=200, label="discovery q percent-encoded")
    expect(plain and coded and plain["candidates"] and coded["candidates"] == plain["candidates"],
           "a percent-encoded q should match like the plain one")
    c.call("getDiscovery", "GET", "/admin/integrations/ha_main/discover?limit=0", expect=422, label="bad limit")
    c.call("getDiscovery", "GET", "/admin/integrations/ha_main/discover?domain=sensor", expect=422,
           label="bad domain")
    c.call("getDiscovery", "GET", "/admin/integrations/ha_main/discover?available=yes", expect=422,
           label="bad available")
    # available=true filters before paging: every candidate is registrable, and a page is full unless it's the last.
    _, avail, _ = c.call("getDiscovery", "GET", "/admin/integrations/ha_main/discover?available=true&limit=1",
                         expect=200, label="discovery available")
    registrable = [x for x in done["candidates"] if x["supported"] and x["registered_device_id"] is None]
    got = (avail or {}).get("candidates", [])
    expect(all(x["supported"] and x["registered_device_id"] is None for x in got), f"available=true: {got}")
    expect(len(got) == 1 if registrable else len(got) <= 1,
           f"available=true limit=1 page: {len(got)} (registrable on page one: {len(registrable)})")

    # One device per entity: a registered entity's details name its device, and registering it again is refused.
    taken = [x for x in done["candidates"] if x["registered_device_id"] and x["supported"]]
    expect(taken, "discovery should show a registered entity")
    if taken:
        d = load_details(c, taken[0]["provider"]["entity_id"])
        expect(d.get("registered_device_id") == taken[0]["registered_device_id"], f"registered details: {d}")
        _, e, _ = c.call("registerDevice", "POST", "/admin/devices",
                         registration_from(c, {"name": "Twin", "integration_id": "ha_main"}, taken[0]),
                         expect=422, label="register a registered entity")
        codes = [x["code"] for x in error_details(e)]
        expect("duplicate_id" in codes, f"register a registered entity: {codes}")
    # A YAML-only entity (no registry entry) stays pickable: no platform, no unique_id.
    yaml_only = [x for x in done["candidates"] if "platform" not in x and x["supported"]]
    expect(yaml_only, "discovery should list a YAML-only entity (no platform)")
    if yaml_only:
        d = load_details(c, yaml_only[0]["provider"]["entity_id"])
        expect(d.get("state") == "done" and "platform" not in d and "unique_id" not in d.get("provider", {})
               and d.get("supported"), f"YAML-only details: {d}")
    # Registering needs the entity's details; an entity HA doesn't have fails to load them.
    reg = {"name": "Not in HA", "integration_id": "ha_main",
           "provider": {"type": "home_assistant", "entity_id": "switch.not_in_ha"}}
    _, e, _ = c.call("registerDevice", "POST", "/admin/devices", reg, expect=409, label="register without details")
    expect((e or {}).get("error", {}).get("code") == "details_required", f"without details: {e}")
    failed = load_details(c, "switch.not_in_ha")
    expect(failed.get("state") == "failed" and failed.get("error"), f"details of an unknown entity: {failed}")
    c.call("registerDevice", "POST", "/admin/devices", reg, expect=409, label="register unknown entity")
    details = "/admin/integrations/ha_main/entities/{}/details"
    c.call("requestEntityDetails", "POST", details.format("sensor.outside"), expect=422, label="details of a sensor")
    c.call("requestEntityDetails", "POST", "/admin/integrations/ble/entities/light.x/details", expect=404,
           label="details from BLE")
    # An unregistered entity, so only the client-sent snapshot can be the reason for the 422.
    fresh = pick_candidate(done)
    snap = {"name": "Spoof", "integration_id": "ha_main", "ha_capabilities": {},
            "provider": fresh["provider"] if fresh else {"type": "home_assistant", "entity_id": "light.family_room"}}
    c.call("registerDevice", "POST", "/admin/devices", snap, expect=422, label="client ha_capabilities")
    return pick_candidate(done, registered_too=True)


def mqtt_put(c, revision, settings, expect=200, label=None, **fields):
    _, cfg, _ = c.call("configureIntegration", "PUT", "/admin/integrations/mqtt_main",
                       {"settings": {"type": "mqtt", **settings}, "revision": revision, **fields},
                       expect=expect, label=label)
    return cfg


def mqtt_flow(c):
    """The MQTT broker settings (the password is write-only, a new host
    forgets it), the status entry, and on hub-sim the fake broker: the hub
    connects, reports auth_failed and unreachable, and an activity chosen in
    Home Assistant runs."""
    _, cfg, _ = c.call("getIntegrationConfig", "GET", "/admin/integrations/mqtt_main", expect=200)
    if not cfg:
        return
    if cfg.get("type") != "mqtt" or (cfg.get("settings") or {}).get("type") != "mqtt":
        return expect(False, f"mqtt_main: {cfg}")
    # (Earlier flows restored a backup, so the built-in's defaults are gone.)
    cfg = mqtt_put(c, cfg["revision"], {}, label="disable MQTT", enabled=False)
    expect(integration_state(c, "mqtt_main") == "disabled", "a disabled mqtt_main reports disabled")
    if not cfg:
        return

    broker = {"host": "192.0.2.10", "port": 1883, "username": "hubbo"}
    cfg = mqtt_put(c, cfg["revision"], {**broker, "password": MQTT_PASSWORD}, label="set MQTT broker",
                   enabled=True)
    expect(cfg and cfg["settings"]["has_password"] is True, f"has_password after set: {cfg}")
    if not cfg:
        return
    e = mqtt_put(c, cfg["revision"], {"host": "192.168.1.5:1883"}, expect=422, label="MQTT host with a port")
    expect([d["path"] for d in error_details(e)] == ["/settings/host"], f"host:port is a host error: {e}")
    for bad, label in (({"port": 0}, "port 0"), ({"host": "a b"}, "host with a space"),
                       ({"password": ""}, "empty password"), ({"access_token": "x"}, "HA's field")):
        mqtt_put(c, cfg["revision"], bad, expect=422, label=f"MQTT {label}")
    mqtt_put(c, cfg["revision"], {"type": "home_assistant", "url": ""}, expect=422, label="HA settings on mqtt_main")
    _, again, _ = c.call("getIntegrationConfig", "GET", "/admin/integrations/mqtt_main", expect=200)
    expect(again and again["settings"]["has_password"] is True and again["settings"]["host"] == broker["host"],
           f"password kept: {again}")
    c.call("downloadBackup", "GET", "/admin/backup", expect=200, label="backup after MQTT password")

    if MOCK:
        skip("MQTT broker connection, auth_failed, unreachable, activity from HA", "the mock has no broker")
    else:
        mqtt_broker_flow(c)

    # A new host (IPv6 is fine) forgets the password; null clears it.
    cfg = mqtt_put(c, cfg["revision"], {"host": "fd00::11"}, label="MQTT new IPv6 host")
    expect(cfg and cfg["settings"]["has_password"] is False, f"a new host clears the password: {cfg}")
    if not cfg:
        return
    cfg = mqtt_put(c, cfg["revision"], {"password": MQTT_PASSWORD}, label="MQTT password again")
    cfg = cfg and mqtt_put(c, cfg["revision"], {"password": None}, label="MQTT clear password")
    expect(cfg and cfg["settings"]["has_password"] is False, f"null clears the password: {cfg}")
    if cfg:
        cfg = mqtt_put(c, cfg["revision"], {"host": ""}, label="MQTT off", enabled=False)
    expect(integration_state(c, "mqtt_main") == "disabled", "disabled again")


def mqtt_broker_flow(c):
    """hub-sim's fake broker (GET/POST /sim/mqtt, POST /sim/mqtt/publish)."""
    expect(until(lambda: integration_state(c, "mqtt_main") == "connected"), "never connected to the fake broker")
    status, broker = c.host_call("GET", "/sim/mqtt")
    expect(status == 200 and broker and broker["connected"], f"/sim/mqtt: {status} {broker}")
    if not broker:
        return
    node = broker["client_id"]
    expect(node.startswith("hubbo_") and len(node) == 18, f"client id: {node}")
    expect(MQTT_PASSWORD not in json.dumps(broker), "the password reached nothing the broker stores")
    expect(f"homeassistant/device/{node}/config" in broker["retained"], f"no discovery: {list(broker['retained'])}")
    expect(broker["retained"].get(f"hubbo/{node}/availability") == "online", "availability online")

    if c.control("mqtt?auth=reject"):
        expect(until(lambda: integration_state(c, "mqtt_main") == "auth_failed"), "a refused login reports auth_failed")
        c.control("mqtt?auth=ok")
    if c.control("mqtt?reachable=0"):
        expect(until(lambda: integration_state(c, "mqtt_main") == "unreachable"), "a broker gone reports unreachable")
        c.control("mqtt?reachable=1")
    expect(until(lambda: integration_state(c, "mqtt_main") == "connected"), "reconnected")

    # Home Assistant chooses an activity, then PowerOff (api::runTransition).
    _, listed, _ = c.call("listActivities", "GET", "/activities", expect=200, label="activities for MQTT")
    names = [a["name"] for a in (listed or {}).get("activities", [])]
    target = next((a for a in (listed or {}).get("activities", [])
                   if names.count(a["name"]) == 1 and not a["name"].startswith("PowerOff")), None)
    if target is None:
        skip("activity from HA", "no activity with a unique name")
        return

    def active():
        _, now, _ = c.call("listActivities", "GET", "/activities", expect=200, label="active after MQTT")
        return (now or {}).get("active_activity_id", "?")

    def choose(label):  # what Home Assistant's Activity select sends
        query = urllib.parse.urlencode({"topic": f"hubbo/{node}/cmd/activity", "payload": label, "retain": 0})
        return c.control("mqtt/publish?" + query)

    if choose(target["name"]):
        expect(until(lambda: active() == target["id"]), f"HA's {target['name']!r} didn't start {target['id']}")
        expect(until(lambda: ((c.host_call("GET", "/sim/mqtt")[1] or {}).get("retained") or {})
                     .get(f"hubbo/{node}/activity") == target["name"]), "the activity state follows")
    if choose("PowerOff"):
        expect(until(lambda: active() is None), "HA's PowerOff didn't turn the hub off")


def wait_discovery(c):
    """Polls GET .../discover until it's done or failed; the last body, or {}."""
    def finished():
        _, d, _ = c.call("getDiscovery", "GET", "/admin/integrations/ha_main/discover", expect=200,
                         label="poll discovery")
        return d if d and d["state"] in ("done", "failed") else None
    return until(finished) or {}


def pick_candidate(done, registered_too=False):
    """A supported candidate, preferring one not registered yet (or None)."""
    supported = [x for x in done.get("candidates", []) if x["supported"]]
    fresh = [x for x in supported if x["registered_device_id"] is None]
    if fresh:
        return fresh[0]
    return supported[0] if registered_too and supported else None


def load_details(c, entity_id):
    """Loads an entity's details (POST, then GET until done or failed); the last body, or {}."""
    path = f"/admin/integrations/ha_main/entities/{entity_id}/details"
    _, started, _ = c.call("requestEntityDetails", "POST", path, expect=202)
    expect((started or {}).get("state") in ("running", "done", "failed"), f"details request: {started}")

    def finished():
        _, d, _ = c.call("getEntityDetails", "GET", path, expect=200, label="poll details")
        return d if d and d.get("state") in ("done", "failed") else None
    return until(finished) or {}


def registration_from(c, reg, candidate):
    """A register request for this discovery candidate, its details loaded
    (or `reg` unchanged without one)."""
    if not candidate:
        return reg
    details = load_details(c, candidate["provider"]["entity_id"])
    expect(details.get("state") == "done", f"details of {candidate['provider']['entity_id']}: {details}")
    if details.get("state") == "done":
        expect(set(candidate["command_ids"]) <= set(details["command_ids"]),
               "the details offer at least the discovery's domain commands")
    return dict(reg, name=candidate["name"][:64], provider=details.get("provider", candidate["provider"]))


def discover_candidate(c):
    """Stores the fake HA's token, runs a discovery and returns an
    unregistered supported candidate (or None)."""
    _, cfg, _ = c.call("getIntegrationConfig", "GET", "/admin/integrations/ha_main", expect=200)
    if not cfg:
        return None
    c.call("configureIntegration", "PUT", "/admin/integrations/ha_main",
           {"settings": {"type": "home_assistant", "access_token": HA_TOKEN}, "revision": cfg["revision"]},
           expect=200, label="token for discovery")
    expect(until(lambda: integration_state(c, "ha_main") == "connected"), "fake HA never connected")
    c.call("discoverIntegration", "POST", "/admin/integrations/ha_main/discover", expect=202)

    return pick_candidate(wait_discovery(c))


def fresh_ha_setup(c):
    """A hub with no HA config yet: ha_main is there (not_configured) and the
    settings PUT is all it takes to connect and discover."""
    _, listing, _ = c.call("listIntegrations", "GET", "/integrations", expect=200, label="fresh integrations")
    ids = [i["id"] for i in (listing or {}).get("integrations", [])]
    expect("ha_main" in ids, f"ha_main should be built in on a fresh hub: {ids}")
    _, cfg, _ = c.call("getIntegrationConfig", "GET", "/admin/integrations/ha_main", expect=200,
                       label="fresh ha_main")
    if not cfg:
        return
    expect(cfg["settings"]["url"] == "", f"fresh ha_main has no URL: {cfg}")
    _, cfg, _ = c.call("configureIntegration", "PUT", "/admin/integrations/ha_main",
                       {"enabled": True, "settings": {"type": "home_assistant",
                                                      "url": "https://homeassistant.local:8123",
                                                      "access_token": HA_TOKEN},
                        "revision": cfg["revision"]}, expect=200, label="set up HA on a fresh hub")
    expect(cfg and cfg["settings"]["has_access_token"] is True, f"fresh setup: {cfg}")
    expect(until(lambda: integration_state(c, "ha_main") == "connected"), "fresh ha_main never connected")
    c.call("discoverIntegration", "POST", "/admin/integrations/ha_main/discover", expect=202,
           label="discover on a fresh hub")

    done = wait_discovery(c)
    expect(done.get("state") == "done" and done.get("total", 0) > 0, f"fresh discovery: {done}")


# hub-sim's fake HA: sonos.set_sleep_timer's sleep_time has HA's default,
# step, unit and translated name and description (sim/src/fake_ha.cpp).
SLEEP_TIME = {"name": "sleep_time", "label": "Sleep Time", "type": "int", "required": False,
              "minimum": 0, "maximum": 7200, "step": 60, "unit": "s",
              "description": "Number of seconds to set the timer.", "default": 600}


def integration_param_flow(c):
    """Re-linking an HA device takes its integration's services from HA, and
    the capabilities carry each field's label, description, default, unit and step."""
    if MOCK:
        skip("integration service metadata", "the mock serves fixed capabilities, not HA's services")
        return
    _, dev, _ = c.call("getDevice", "GET", "/devices/dev_sonos", expect=200, label="sonos before re-link")
    details = load_details(c, "media_player.family_room_sonos")
    expect(details.get("state") == "done", f"sonos details: {details}")
    if not dev or details.get("state") != "done":
        return
    c.call("updateDevice", "PUT", "/admin/devices/dev_sonos",
           {"provider": details["provider"], "revision": dev["revision"]}, expect=200, label="re-link sonos")
    _, caps, _ = c.call("getDeviceCapabilities", "GET", "/devices/dev_sonos/capabilities", expect=200,
                        label="sonos after re-link")
    timer = next((x for x in (caps or {}).get("commands", []) if x["command_id"] == "sonos.set_sleep_timer"), None)
    expect(timer and timer["label"] == "Set timer", f"sonos.set_sleep_timer after re-link: {timer}")
    if timer:
        expect(timer["params"] == [SLEEP_TIME], f"sleep_time param: {timer['params']}")


def mute_toggle_flow(c):
    """media_player.mute_toggle reads the entity's is_volume_muted at press
    time and sends the opposite (HA has no mute toggle)."""
    if MOCK:
        skip("mute toggle", "the mock has no Home Assistant state")
        return
    entity = "media_player.family_room_denon"
    path = "/devices/dev_denon/commands/media_player.mute_toggle/execute"

    def toggle(was, sends):
        _, body, _ = c.call("executeCommand", "POST", path, {"params": {}}, expect=200,
                            label=f"mute toggle, {was}")
        expect((body or {}).get("status") == "accepted", f"mute toggle, {was}: {body}")
        _, reply = c.host_call("POST", "/sim/ha")
        last = (reply or {}).get("last_call")
        want = {"service": "media_player.volume_mute", "entity_id": entity, "data": {"is_volume_muted": sends}}
        expect(last == want, f"mute toggle, {was}: sent {last}, want {want}")

    c.control(f"ha?muted={entity}:1")
    toggle("muted", False)
    toggle("unmuted by the toggle", True)  # the fake applied the first call
    c.control(f"ha?muted={entity}:0")
    toggle("unmuted", True)
    c.control(f"ha?muted={entity}:0")  # leave the Denon unmuted for later flows


def refused_at(c, label, body, code, at, path, op="executeCommand", method="POST"):
    """A 422 whose details include `code` at `at`."""
    _, reply, _ = c.call(op, method, path, body, expect=422, label=label)
    details = [(d["code"], d["path"]) for d in error_details(reply)]
    expect((code, at) in details, f"{label}: want {code} at {at}, got {details}")


def ha_action_flow(c):
    """A Home Assistant action (call_service): the LG TV offers its webostv
    services with HA's hints, an execute sends the service and data verbatim
    to the TV's entity, and the call's rules answer 422 at their paths."""
    _, caps, _ = c.call("getDeviceCapabilities", "GET", "/devices/dev_lg_tv/capabilities", expect=200)
    action = next((x for x in (caps or {}).get("commands", []) if x["command_id"] == "call_service"), None)
    want = next(x for x in fixture("devices/dev_lg_tv.capabilities.json")["commands"]
                if x["command_id"] == "call_service")
    expect(action == want, f"dev_lg_tv call_service: {action}")
    _, denon, _ = c.call("getDeviceCapabilities", "GET", "/devices/dev_denon/capabilities", expect=200)
    expect(all(x["command_id"] != "call_service" for x in (denon or {}).get("commands", [])),
           "dev_denon has no custom services, so no Home Assistant action")

    path = "/devices/dev_lg_tv/commands/call_service/execute"
    info = {"service": "webostv.button", "data": {"button": "INFO"}}
    _, body, _ = c.call("executeCommand", "POST", path, {"params": {}, "call": info}, expect=200,
                        label="Home Assistant action")
    expect((body or {}).get("status") == "accepted", f"Home Assistant action: {body}")
    if not MOCK:
        _, reply = c.host_call("POST", "/sim/ha")
        last = (reply or {}).get("last_call")
        expect(last == {"service": "webostv.button", "entity_id": "media_player.family_room_lg_tv",
                        "data": {"button": "INFO"}}, f"Home Assistant action sent {last}")


    def with_data(data):
        return {"params": {}, "call": {"service": "webostv.button", "data": data}}

    deep = {"a": {"b": {"c": {"d": {"e": 1}}}}}
    refused_at(c, "call missing", {"params": {}}, "required", "/call", path=path)
    refused_at(c, "call on another command", {"params": {}, "call": info}, "unknown_field", "/call",
               path="/devices/dev_lg_tv/commands/media_player.volume_up/execute")
    refused_at(c, "service pattern", {"params": {}, "call": {"service": "webostv"}},
               "pattern", "/call/service", path=path)
    refused_at(c, "service of another integration", {"params": {}, "call": {"service": "homeassistant.stop"}},
               "forbidden_param", "/call/service", path=path)
    refused_at(c, "remote service on a media player", {"params": {}, "call": {"service": "remote.send_command"}},
               "forbidden_param", "/call/service", path=path)
    refused_at(c, "data not an object", with_data(["INFO"]), "wrong_type", "/call/data", path=path)
    refused_at(c, "data too big", with_data({"button": "x" * 1100}), "too_long", "/call/data", path=path)
    refused_at(c, "data too deep", with_data(deep), "too_deep", "/call/data", path=path)
    refused_at(c, "data names a target", with_data({"entity_id": "lock.front_door"}), "forbidden_param",
               "/call/data/entity_id", path=path)
    refused_at(c, "data holds a template", with_data({"payload": {"target": "{{ states('x') }}"}}),
               "template_not_supported", "/call/data/payload/target", path=path)
    refused_at(c, "no custom services", {"params": {}, "call": info}, "unknown_command", "/command_id",
               path="/devices/dev_denon/commands/call_service/execute")

    # The same rules in a binding, at the action's path.
    gb = c.call("getGlobalBindings", "GET", "/global-bindings", expect=200)[1]
    if gb:
        act = {"integration_id": "ha_main", "device_id": "dev_lg_tv", "command_id": "call_service", "params": {}}
        put = {"path": "/global-bindings", "op": "replaceGlobalBindings", "method": "PUT"}

        def bind(action):
            return dict(gb, bindings=dict(gb["bindings"], **{"info:tap": [action]}))
        for label, extra, code, at in (
                ("binding without call", {}, "required", "/bindings/info:tap/0/call"),
                ("binding data names a target", {"call": {"service": "webostv.button", "data": {"device_id": "x"}}},
                 "forbidden_param", "/bindings/info:tap/0/call/data/device_id"),
                ("binding service of another integration", {"call": {"service": "hassio.host_shutdown"}},
                 "forbidden_param", "/bindings/info:tap/0/call/service"),
                # 10 levels, the request limit: still parsed, so too_deep at its path.
                ("binding data too deep", {"call": {"service": "webostv.button", "data": deep}},
                 "too_deep", "/bindings/info:tap/0/call/data")):
            refused_at(c, label, bind(dict(act, **extra)), code, at, **put)
        stray = {"integration_id": "system", "device_id": None, "command_id": "off", "params": {}, "call": info}
        refused_at(c, "call on another command, in a binding", bind(stray), "unknown_field",
                   "/bindings/info:tap/0/call", **put)


def command_ids(press):
    """A press as {list: [command_id(:step)]}, for short messages."""
    out = {}
    for key in ("tap", "hold", "release"):
        out[key] = [a["command_id"] + (f":{a['params']['step']}" if "step" in a["params"] else "")
                    for a in press[key]]
    return out


def activity_runs(c, label):
    """GET /activity-runs, checking the ids are newest first and that
    /status last_run sums up the newest. Returns the runs."""
    _, body, _ = c.call("listActivityRuns", "GET", "/activity-runs", expect=200, label=label)
    got = (body or {}).get("runs", [])
    ids = [r["id"] for r in got]
    expect(ids == sorted(ids, reverse=True) and len(set(ids)) == len(ids), f"{label}: ids newest first: {ids}")
    for r in got:
        macro = r["kind"] == "macro"
        expect((r["button"] is not None) == macro and (r["trigger"] is not None) == macro
               and (r["activity_id"] is not None or macro), f"{label}: run {r['id']}'s button/trigger/activity: {r}")
    _, st, _ = c.call("getStatus", "GET", "/status", expect=200, label=f"{label}: last_run")
    want = None
    if got:
        want = {k: got[0][k] for k in ("id", "activity_id", "kind", "button", "trigger", "outcome")}
        want["failed_steps"] = sum(x["result"] in ("failed", "timed_out") for x in got[0]["steps"])
    expect(st and st.get("last_run") == want, f"{label}: last_run {st and st.get('last_run')}, want {want}")
    return got


def activity_runs_flow(c):
    """/activity-runs and /status last_run agree. On hub-sim (HA connected):
    a startup goes on past a step HA refuses (completed_with_errors), and
    off's shutdown is then the newest run. State checks: with the Denon
    already on, on another input and slow to answer, Play Xbox skips its
    power, its input change gets no_reply, and the run is completed."""
    def last_run_matches(label):
        got = activity_runs(c, label)
        return got[0] if got else {}

    def switch(path, label, setup=None, reset=None):
        """POST `path` with the fake HA set up, let the run finish, and
        return the newest run."""
        if setup:
            c.control(f"ha?{setup}")
        op = "activitiesOff" if path.endswith("/off") else "activateActivity"
        c.call(op, "POST", path, expect=200, label=label)
        c.control("advance?ms=6000")  # the runs' waits, and the slow Denon's 5 s reply
        if reset:
            c.control(f"ha?{reset}")
        return last_run_matches(label)

    last_run_matches("activity runs")
    if MOCK:
        skip("activity runs as they happen", "the mock's runs are its own; hub-sim runs the engine")
        return
    expect(until(lambda: integration_state(c, "ha_main") == "connected"), "activity runs need HA connected")

    act = "act_watch_apple_tv"
    # The LG off, so its turn_on isn't skipped (the fake remembers power).
    run = switch(f"/activities/{act}/activate", "a failing step",
                 "state=media_player.family_room_lg_tv:off&fail=media_player.turn_on", "fail=none")
    steps = run.get("steps", [])
    expect(run.get("activity_id") == act and run.get("kind") == "startup"
           and run.get("outcome") == "completed_with_errors", f"a startup with a failing step: {run}")
    expect(steps and steps[0]["command_id"] == "media_player.turn_on" and steps[0]["result"] == "failed"
           and steps[0]["error"] not in (None, "adapter_unavailable"), f"the step HA refused: {steps[:1]}")
    expect(len(steps) > 1 and all(x["result"] not in ("pending", "cancelled") for x in steps[1:]),
           f"the steps after it still ran: {steps}")
    run = switch("/activities/off", "off after a failing step")
    expect(run.get("kind") == "shutdown" and run.get("activity_id") == act
           and run.get("outcome") in ("completed", "completed_with_errors"), f"off's shutdown: {run}")

    denon = "media_player.family_room_denon"
    run = switch("/activities/act_play_xbox/activate", "state checks",
                 f"state={denon}:on&attr={denon}:source=TV&slow={denon}:5000", "slow=none")
    by_step = {(x["device_id"], x["command_id"]): x for x in run.get("steps", [])}
    power = by_step.get(("dev_denon", "media_player.turn_on"), {})
    source = by_step.get(("dev_denon", "media_player.select_source"), {})
    expect(power.get("result") == "skipped" and power.get("error") == "already_set",
           f"the Denon's power, already on: {power}")
    expect(source.get("result") == "no_reply" and source.get("error") is None
           and 1000 <= (source.get("duration_ms") or 0) <= 1500, f"the slow Denon's input (1 s limit): {source}")
    expect(run.get("outcome") == "completed", f"a run with skipped and no_reply steps: {run}")
    switch("/activities/off", "off after the state checks")


def expect_macro_run(run, label, button, trigger, activity, commands):
    """A completed macro run: its button, trigger, activity and steps."""
    expect(run.get("kind") == "macro" and run.get("button") == button and run.get("trigger") == trigger
           and run.get("activity_id") == activity and run.get("outcome") == "completed"
           and [x["command_id"] for x in run.get("steps", [])] == commands, f"{label}: {run}")


def macro_runs_flow(c):
    """Macros (triggers listed in `macros`) on hub-sim: a list of more than
    one action needs marking; an activity's and a global one are runs of
    kind macro with their button and trigger (a global one has no
    activity), a wait between the steps, and a one-command macro too;
    a press while four wait is dropped (not listed); a switch of activity cancels
    a macro mid-wait. The bindings are put back after."""
    if MOCK:
        skip("macro runs as they happen", "the mock's runs are its own; hub-sim runs the engine")
        return
    expect(until(lambda: integration_state(c, "ha_main") == "connected"), "macro runs need HA connected")
    act_id = "act_watch_apple_tv"
    _, act, _ = c.call("getActivity", "GET", f"/activities/{act_id}", expect=200, label="macro activity")
    _, gb, _ = c.call("getGlobalBindings", "GET", "/global-bindings", expect=200, label="macro globals")
    if not act or not gb:
        return
    sonos = {"integration_id": "ha_main", "device_id": "dev_sonos"}
    macro = [dict(sonos, command_id="media_player.volume_up", params={}),
             {"integration_id": "system", "device_id": None, "command_id": "wait", "params": {"duration_ms": 300}},
             dict(sonos, command_id="media_player.volume_down", params={})]
    names = [x["command_id"] for x in macro]
    # Guide's own binding (the seed's macro on down) gives way to these.
    plain = {k: v for k, v in act.items() if k != "macros"}
    plain["bindings"] = {k: v for k, v in act["bindings"].items() if not k.startswith("guide:")}
    edited = dict(copy.deepcopy(plain), macros=[k for k in act["macros"] if not k.startswith("guide:")])
    edited["bindings"].update({"guide:tap": macro, "guide:hold": macro[2:]})
    # Unmarked, a list of more than one action is refused.
    put_act = {"path": f"/activities/{act_id}", "op": "replaceActivity", "method": "PUT"}
    refused_at(c, "an unmarked macro", edited, "macro_required", "/bindings/guide:tap", **put_act)
    for macros, code, at, label in [
            (["guide:up"], "invalid_trigger", "/macros/0", "a macro on up"),
            (["guide:tap", "guide:tap"], "duplicate_trigger", "/macros/1", "a macro listed twice"),
            (["guide:tap", "red:tap"], "unbound_macro", "/macros/1", "a macro with no binding"),
            ([5], "wrong_type", "/macros/0", "a macro that isn't a trigger")]:
        refused_at(c, label, dict(edited, macros=macros), code, at, **put_act)
    # Left out, `macros` means none.
    _, none, _ = c.call("replaceActivity", "PUT", f"/activities/{act_id}", plain, expect=200, label="no macros")
    expect((none or {}).get("macros") == [], f"no macros reads back as []: {none and none.get('macros')}")
    edited["revision"] = (none or edited)["revision"]
    edited["macros"] += ["guide:tap", "guide:hold"]
    _, put, _ = c.call("replaceActivity", "PUT", f"/activities/{act_id}", edited, expect=200,
                       label="Press and Long press macros")
    globals_ = dict(gb, bindings=dict(gb["bindings"], **{"dvr:down": macro}), macros=gb["macros"] + ["dvr:down"])
    _, gput, _ = c.call("replaceGlobalBindings", "PUT", "/global-bindings", globals_, expect=200,
                        label="a global macro")
    if not put or not gput:
        return
    c.call("activateActivity", "POST", f"/activities/{act_id}/activate", expect=200, label="activate for macros")
    c.control("advance?ms=20000")

    def pressed(button, action, label):
        c.control(f"remote/{button}/{action}")
        c.control("advance?ms=2000")
        runs = activity_runs(c, label)
        return runs[0] if runs else {}

    for button, action, trigger, activity in [("guide", "tap", "press", act_id),
                                              ("dvr", "tap", "press", None)]:
        run = pressed(button, action, f"{button} macro")
        expect_macro_run(run, f"the {button} macro's run", button, trigger, activity, names)
        steps = run.get("steps", [])
        expect(len(steps) == 3 and steps[1]["device_id"] is None and 300 <= (steps[1]["duration_ms"] or 0) < 400,
               f"the {button} macro's wait: {steps}")
    c.control("remote/guide/press")
    c.control("advance?ms=1500")  # past the hold time
    c.control("remote/guide/release")
    c.control("advance?ms=2000")
    run = (activity_runs(c, "a Long press macro") or [{}])[0]
    expect_macro_run(run, "the Long press macro's run (one command)", "guide", "long_press", act_id,
                     ["media_player.volume_down"])

    # One runs and four wait (a 2 s wait each, longer than six taps take);
    # the sixth press is dropped, and not listed.
    slow = [macro[0], dict(macro[1], params={"duration_ms": 2000}), macro[2]]
    c.call("replaceActivity", "PUT", f"/activities/{act_id}",
           dict(edited, bindings=dict(edited["bindings"], **{"guide:tap": slow}), revision=put["revision"]),
           expect=200, label="a long macro")
    before = max((r["id"] for r in activity_runs(c, "before a full macro lane")), default=0)
    log = sim_commands(c)
    for _ in range(6):
        c.control("remote/guide/tap")
    c.control("advance?ms=12000")
    full = [r for r in activity_runs(c, "a full macro lane") if r["id"] > before]
    expect([r["outcome"] for r in full] == ["completed"] * 5 and [r["id"] for r in full] == list(range(before + 5, before, -1)),
           f"a full macro lane: {[(r['id'], r['outcome']) for r in full]}")
    sent = [e.get("text", "") for e in commands_since(log, sim_commands(c))]
    ups, downs = (sum("media_player.volume_" + x in t for t in sent) for x in ("up", "down"))
    expect(ups == 5 and downs == 5, f"a full macro lane sent {ups} volume_up, {downs} volume_down: {sent}")

    # Trying a macro from the editor: queued as written, recorded under its run_id.
    tried = {"actions": macro, "trigger": "guide:tap", "activity_id": act_id}
    _, queued, _ = c.call("runMacro", "POST", "/macros/run", tried, expect=202, label="try a macro")
    c.control("advance?ms=2000")
    run = next((r for r in activity_runs(c, "a tried macro") if r["id"] == (queued or {}).get("run_id")), {})
    expect_macro_run(run, "a tried macro's run", "guide", "press", act_id, names)
    hold = {"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "press_down",
            "params": {"command": "Select"}}
    for body, code, at, label in [
            (dict(tried, actions=[hold]), "hold_not_allowed", "/actions/0/command_id", "try a macro with a hold"),
            (dict(tried, actions=[macro[1]]), "sequence_only", "/actions/0/command_id", "try a macro of only a wait"),
            (dict(tried, trigger="guide:up"), "invalid_trigger", "/trigger", "try a macro on up"),
            (dict(tried, activity_id="act_nope"), "unknown_activity", "/activity_id", "try a macro in no activity")]:
        refused_at(c, label, body, code, at, "/macros/run", op="runMacro")
    # One runs and four wait: the sixth try is 503 busy, nothing queued.
    slow_try = dict(tried, actions=slow)
    for i in range(5):
        c.call("runMacro", "POST", "/macros/run", slow_try, expect=202, label=f"try a slow macro {i + 1}")
    _, busy, _ = c.call("runMacro", "POST", "/macros/run", slow_try, expect=503, label="try a macro, lane full")
    expect(error_code(busy) == "busy", f"a full macro lane: {busy}")
    c.control("advance?ms=12000")

    # A switch mid-wait cancels the macro.
    c.control("remote/guide/tap")
    c.call("activitiesOff", "POST", "/activities/off", expect=200, label="off mid-macro")
    c.control("advance?ms=6000")
    macros = [r for r in activity_runs(c, "a macro cut short") if r["kind"] == "macro"]
    expect(macros and macros[0]["outcome"] == "cancelled"
           and any(x["result"] == "cancelled" for x in macros[0]["steps"]), f"a macro cut short: {macros[:1]}")

    _, cur, _ = c.call("getActivity", "GET", f"/activities/{act_id}", expect=200, label="macro restore")
    c.call("replaceActivity", "PUT", f"/activities/{act_id}", dict(act, revision=(cur or act)["revision"]),
           expect=200, label="put the activity back")
    c.call("replaceGlobalBindings", "PUT", "/global-bindings", dict(gb, revision=gput["revision"]), expect=200,
           label="put the global bindings back")


def smart_buttons_flow(c):
    """The four slots: as seeded with their derived actions, a replace and
    its actions, revision conflicts, an unknown device, a slot's device
    refused deletion, and selected_slot. persistence_flow restores a backup
    without smart_buttons (four empty slots)."""
    _, sb, _ = c.call("getSmartButtons", "GET", "/smart-buttons", expect=200)
    if not sb:
        return
    seeded = fixture("smart_buttons/get.json")
    expect({k: sb[k] for k in ("slots", "actions")} == {k: seeded[k] for k in ("slots", "actions")},
           f"smart buttons as seeded: {diff(sb, seeded)}")
    _, st, _ = c.call("getStatus", "GET", "/status", expect=200, label="selected slot")
    expect(st and st.get("selected_slot") == "light_1", f"selected_slot after boot: {st and st.get('selected_slot')}")

    for name in sorted(os.listdir(os.path.join(FIXTURES, "smart_buttons"))):
        if not name.startswith("actions."):
            continue
        dev_id = name[len("actions."):-len(".json")]
        _, got, _ = c.call("getSmartButtonActions", "GET", f"/smart-buttons/actions?device_id={dev_id}",
                           expect=200, label=f"preview for {dev_id}")
        want = fixture(f"smart_buttons/{name}")
        expect(got == want, f"preview for {dev_id}: {diff(got, want)}")
    c.call("getSmartButtonActions", "GET", "/smart-buttons/actions?device_id=dev_nope", expect=404,
           label="preview for a missing device")
    _, e, _ = c.call("getSmartButtonActions", "GET", "/smart-buttons/actions", expect=422,
                     label="preview without a device")
    expect(("required", "/device_id") in [(d["code"], d["path"]) for d in error_details(e)],
           f"preview without a device: {e}")

    body = dict(fixture("smart_buttons/replace.request.json"), revision=sb["revision"])
    _, put, _ = c.call("replaceSmartButtons", "PUT", "/smart-buttons", body, expect=200)
    current = put["revision"] if put else sb["revision"] + 1
    want = fixture("smart_buttons/replace.json")
    expect(put and put["slots"] == body["slots"] and put["actions"] == want["actions"]
           and current > sb["revision"], f"replaced smart buttons: {put and diff(put, want)}")
    _, conflict, _ = c.call("replaceSmartButtons", "PUT", "/smart-buttons", body, expect=409,
                            label="stale smart buttons")
    expect(error_code(conflict) == "revision_conflict" and conflict["error"].get("current_revision") == current,
           f"stale smart buttons: {conflict}")
    put_op = {"path": "/smart-buttons", "op": "replaceSmartButtons", "method": "PUT"}
    refused_at(c, "a slot with an unknown device", {"revision": current, "slots": dict(body["slots"], light_2="dev_nope")},
               "unknown_device", "/slots/light_2", **put_op)
    refused_at(c, "actions sent back (read-only)", dict(body, revision=current, actions=put["actions"] if put else {}),
               "unknown_field", "/actions", **put_op)

    lights = body["slots"]["light_1"]
    _, dev, _ = c.call("getDevice", "GET", f"/devices/{lights}", expect=200, label="a slot's device")
    if dev:
        _, e, _ = c.call("deleteDevice", "DELETE", f"/admin/devices/{lights}?revision={dev['revision']}",
                         expect=409, label="delete a slot's device")
        expect(error_code(e) == "device_in_use"
               and "/smart_buttons/slots/light_1" in [d["path"] for d in error_details(e)],
               f"delete a slot's device: {e}")
    c.call("replaceSmartButtons", "PUT", "/smart-buttons", {"slots": sb["slots"], "revision": current},
           expect=200, label="smart buttons back as seeded")

    _, backup, _ = c.call("downloadBackup", "GET", "/admin/backup", expect=200, label="backup for smart buttons")
    expect((backup or {}).get("smart_buttons", {}).get("slots") == seeded["slots"],
           "the backup carries the smart buttons' slots")


def new_domain_actions(c, entity_id, dev):
    """Actions by kind on a registered newer-domain device, set in Light 2:
    the cover with a position steps it, and a kind decides whatever the
    domain (an input_boolean as a switch, then as a light without
    brightness). Light 2 is empty again after, so the device can go."""
    if entity_id not in ("cover.office_blinds", "input_boolean.guest_mode"):
        return
    _, sb, _ = c.call("getSmartButtons", "GET", "/smart-buttons", expect=200, label=f"{entity_id} in Light 2")
    if not sb:
        return
    _, put, _ = c.call("replaceSmartButtons", "PUT", "/smart-buttons",
                       {"slots": dict(sb["slots"], light_2=dev["id"]), "revision": sb["revision"]},
                       expect=200, label=f"{entity_id} in Light 2")
    if not put:
        return

    def slot_actions(label):
        _, got, _ = c.call("getSmartButtons", "GET", "/smart-buttons", expect=200, label=label)
        return {p: command_ids(got["actions"]["light_2"][p]) for p in ("button", "plus", "minus")} if got else {}

    if entity_id == "cover.office_blinds":
        got = slot_actions("actions for a cover")
        want = {"button": {"tap": ["cover.open_cover"], "hold": ["cover.close_cover"], "release": []},
                "plus": {"tap": ["cover.position_step:10"], "hold": ["cover.open_cover"],
                         "release": ["cover.stop_cover"]},
                "minus": {"tap": ["cover.position_step:-10"], "hold": ["cover.close_cover"],
                          "release": ["cover.stop_cover"]}}
        expect(got == want, f"actions for a cover: {got}")
        # Change position reads current_position (the fake's starts at 50).
        path = f"/devices/{dev['id']}/commands/cover.position_step/execute"
        _, r, _ = c.call("executeCommand", "POST", path, {"params": {"step": 10}}, expect=200, label="change position")
        expect((r or {}).get("status") == "accepted", f"change position: {r}")
        _, reply = c.host_call("POST", "/sim/ha")
        last = (reply or {}).get("last_call")
        want_call = {"service": "cover.set_cover_position", "entity_id": entity_id, "data": {"position": 60}}
        expect(last == want_call, f"change position sent {last}, want {want_call}")
    else:
        got = slot_actions("actions for another kind")
        expect(all(not any(p.values()) for p in got.values()), f"an `other` device gets empty presses: {got}")
        rev = dev["revision"]
        for kind in ("switch", "light"):
            _, upd, _ = c.call("updateDevice", "PUT", f"/admin/devices/{dev['id']}", {"kind": kind, "revision": rev},
                               expect=200, label=f"guest mode as a {kind}")
            rev = upd["revision"] if upd else rev
            got = slot_actions(f"actions for an input_boolean as a {kind}")
            expect(got.get("button", {}).get("tap") == ["input_boolean.turn_on"] and not any(got.get("plus", {}).values()),
                   f"an input_boolean as a {kind}: {got}")
        dev["revision"] = rev
    c.call("replaceSmartButtons", "PUT", "/smart-buttons", {"slots": sb["slots"], "revision": put["revision"]},
           expect=200, label=f"Light 2 empty again after {entity_id}")


# One made-up entity per newer domain (gen_fixtures.py NEW_DOMAIN_ENTITIES);
# hub-sim's fake HA has the same ones.
NEW_DOMAIN_FIXTURES = os.path.join(FIXTURES, "integrations", "ha_main.entities")


def new_domains_flow(c):
    """Each newer-domain entity is a candidate with the fixture's
    suggested_kind, and registering it gives exactly the fixture's commands."""
    if MOCK:
        skip("newer HA domains", "the mock serves these fixtures; hub-sim checks them against the allowlist")
        return
    c.call("discoverIntegration", "POST", "/admin/integrations/ha_main/discover", expect=202,
           label="discover the newer domains")
    done = wait_discovery(c)
    found = {x["provider"]["entity_id"]: x for x in done.get("candidates", [])}
    want = {x["provider"]["entity_id"]: x for x in fixture("integrations/ha_main.discovery.json")["candidates"]}
    for name in sorted(os.listdir(NEW_DOMAIN_FIXTURES)):
        entity_id = name[:-len(".capabilities.json")]
        cand = found.get(entity_id)
        expect(cand, f"{entity_id} isn't a discovery candidate")
        if not cand:
            continue
        expect(cand["suggested_kind"] == want[entity_id]["suggested_kind"]
               and cand["command_ids"] == want[entity_id]["command_ids"],
               f"{entity_id}: candidate differs from the fixture: {cand}")
        reg = registration_from(c, {"name": cand["name"], "integration_id": "ha_main",
                                    "kind": cand["suggested_kind"]}, cand)
        _, dev, _ = c.call("registerDevice", "POST", "/admin/devices", reg, expect=201, label=f"register {entity_id}")
        if not dev:
            continue
        _, caps, _ = c.call("getDeviceCapabilities", "GET", f"/devices/{dev['id']}/capabilities", expect=200,
                            label=f"{entity_id} capabilities")
        expect_fixture_commands(entity_id, caps, f"integrations/ha_main.entities/{name}")
        if entity_id.startswith("remote."):
            remote_action(c, dev["id"], entity_id)
        new_domain_actions(c, entity_id, dev)
        c.call("deleteDevice", "DELETE", f"/admin/devices/{dev['id']}?revision={dev['revision']}", expect=204,
               label=f"remove {entity_id}")


def remote_action(c, dev_id, entity_id):
    """A remote sends HA's remote.send_command through the Home Assistant
    action; another integration's service is still refused."""
    path = f"/devices/{dev_id}/commands/call_service/execute"
    send = {"service": "remote.send_command", "data": {"command": "DPAD_UP"}}
    _, body, _ = c.call("executeCommand", "POST", path, {"params": {}, "call": send}, expect=200,
                        label="remote.send_command")
    expect((body or {}).get("status") == "accepted", f"remote.send_command: {body}")
    _, reply = c.host_call("POST", "/sim/ha")
    last = (reply or {}).get("last_call")
    expect(last == {"service": "remote.send_command", "entity_id": entity_id, "data": {"command": "DPAD_UP"}},
           f"remote.send_command sent {last}")
    refused_at(c, "another integration's service on a remote",
               {"params": {}, "call": {"service": "webostv.button", "data": {"button": "UP"}}},
               "forbidden_param", "/call/service", path)


def ha_unlink_flow(c):
    """Cancel drops the candidates; a new URL clears the token."""
    c.call("cancelDiscovery", "DELETE", "/admin/integrations/ha_main/discover", expect=204)
    _, d, _ = c.call("getDiscovery", "GET", "/admin/integrations/ha_main/discover", expect=200,
                     label="after cancel")
    expect(d and d["state"] == "idle" and "candidates" not in d, f"after cancel: {d}")
    _, cfg, _ = c.call("getIntegrationConfig", "GET", "/admin/integrations/ha_main", expect=200)
    if not cfg:
        return
    url = cfg["settings"]["url"]
    _, moved, _ = c.call("configureIntegration", "PUT", "/admin/integrations/ha_main",
                         {"settings": {"type": "home_assistant", "url": "http://other.local:8123"},
                          "revision": cfg["revision"]}, expect=200, label="new HA URL")
    expect(moved and moved["settings"]["has_access_token"] is False, "a new URL clears the token")
    if moved:
        c.call("configureIntegration", "PUT", "/admin/integrations/ha_main",
               {"settings": {"type": "home_assistant", "url": url}, "revision": moved["revision"]},
               expect=200, label="HA URL back")
    expect(until(lambda: integration_state(c, "ha_main") == "not_configured"), "without a token HA is not_configured")
    c.call("discoverIntegration", "POST", "/admin/integrations/ha_main/discover", expect=503,
           label="discover without HA")


def remote_flow(c):
    """The remote endpoint (the console's `radio set` rule) and the Hub scan."""
    c.call("getRemoteConfig", "GET", "/admin/remote", expect=200)
    for bad in ({"channel": 125, "address": "AB12CD34EF"}, {"channel": 5, "address": "0000000000"},
                {"channel": 5, "address": "12345678AA"}, {"channel": 5, "address": "nothex"},
                {"channel": 5}):
        c.call("setRemoteEndpoint", "PUT", "/admin/remote/endpoint", bad, expect=422, label=f"bad endpoint {bad}")
    _, cfg, _ = c.call("setRemoteEndpoint", "PUT", "/admin/remote/endpoint",
                       fixture("admin/remote_endpoint.request.json"), expect=200)
    expect(cfg and cfg["endpoint"] == {"channel": 74, "address": "FC5C97550F"}, f"endpoint: {cfg}")
    _, scan, _ = c.call("startRemotePairing", "POST", "/admin/remote/pair", expect=202)
    expect(scan and scan["state"] == "scanning", f"scan: {scan}")
    c.call("setRemoteEndpoint", "PUT", "/admin/remote/endpoint", {"channel": 5, "address": "AB12CD34EF"},
           expect=409, label="endpoint while scanning")
    c.control("radio/found?channel=40&address=1122334466")
    _, found, _ = c.call("getRemotePairing", "GET", "/admin/remote/pair", expect=200)
    expect(found and found["state"] == "found" and found["channel"] == 40, f"found: {found}")
    c.call("startRemotePairing", "POST", "/admin/remote/pair", expect=202, label="scan again")
    c.call("cancelRemotePairing", "DELETE", "/admin/remote/pair", expect=204)
    _, stopped, _ = c.call("getRemotePairing", "GET", "/admin/remote/pair", expect=200, label="after cancel")
    expect(stopped and stopped["state"] == "stopped", f"after cancel: {stopped}")
    # Leave the endpoint as the fixture's.
    c.call("setRemoteEndpoint", "PUT", "/admin/remote/endpoint", fixture("admin/remote_endpoint.request.json"),
           expect=200, label="endpoint back")


def remote_log_flow(c):
    """The remote log: a hub-sim press and release show up as packets and
    events in order (oldest first), with the press's signal."""
    if MOCK:
        return skip("the remote log", "the mock has no radio")
    c.control("remote/info/press?signal=weak")
    c.control("advance?ms=100")
    c.control("remote/info/release?signal=strong")  # the signal sticks: back to the default
    c.control("advance?ms=100")
    log = c.call("getRemoteLog", "GET", "/remote/log", expect=200, label="the remote log after a press")[1] or {}
    expect(0 <= log.get("span_ms", -1) <= 300000, f"remote log span: {log.get('span_ms')}")
    entries = log.get("entries", [])
    ages = [e["age_ms"] for e in entries]
    expect(ages == sorted(ages, reverse=True), f"remote log not oldest first: {ages}")
    info = [e for e in entries if e.get("button") == "info"]
    downs = [i for i, e in enumerate(info) if (e["type"], e["kind"]) == ("packet", "down")]
    press = info[downs[-1]:] if downs else []  # from this press's down packet on
    seen = [(e["type"], e["kind"]) for e in press]
    want = [("packet", "down"), ("event", "down"), ("packet", "up"), ("event", "up")]
    expect([x for x in seen if x in want] == want, f"the press in the remote log: {seen}")
    expect([e["signal"] for e in press if e["type"] == "packet"][:1] == ["weak"], f"the press's signal: {press}")


def ble_pairing_flow(c):
    """Bluetooth hosts, one per device: pairing a device's own host (the
    `ble/bond` control bonds one in the open window), one window at a time,
    a re-pair that runs out keeping the old host, forget, the limit of 4,
    and the radio going down (`ble/radio`)."""
    profiles_flow(c)
    c.call("getDevicePairing", "GET", "/admin/devices/dev_apple_tv/pair", expect=200)
    c.call("startDevicePairing", "POST", "/admin/devices/dev_apple_tv/pair?window_s=29", expect=422,
           label="pair window too short")
    _, body, _ = c.call("startDevicePairing", "POST", "/admin/devices/dev_denon/pair", expect=404,
                        label="pair a non-Bluetooth device")
    expect(error_code(body) == "not_found", f"pair a non-Bluetooth device: {body}")
    c.call("getDevicePairing", "GET", "/admin/devices/dev_nope/pair", expect=404, label="pair an unknown device")

    # A Bluetooth device comes from a profile, never from the client.
    for body, label, code, path in [
        ({"profile_id": "hubbo:roku"}, "an unknown profile", "unknown_profile", "/profile_id"),
        ({"profile_id": "hubbo:fire-tv", "kind": "toaster"}, "an unknown kind", "unknown_kind", "/kind"),
        ({"profile_id": "hubbo:fire-tv", "kind": "Not A Kind"}, "a malformed kind", "unknown_kind", "/kind"),
        ({"profile_id": "hubbo:fire-tv", "commands": {}}, "a profile with commands", "unknown_field", "/commands"),
        ({"profile_id": "hubbo:fire-tv", "integration_id": "ble"}, "a profile with an integration",
         "unknown_field", "/integration_id"),
        ({"profile": fixture("profiles/fire-tv.json")}, "a whole profile", "unknown_field", "/profile"),
        ({"name": "Shield", "integration_id": "ble", "kind": "streamer", "provider": {"type": "ble_hid"}},
         "a Bluetooth device without a profile", None, None),
    ]:
        _, body, _ = c.call("registerDevice", "POST", "/admin/devices", body, expect=422, label=label)
        details = [(d["code"], d["path"]) for d in error_details(body)]
        if code:
            expect((code, path) in details, f"{label}: expected {code} at {path}, got {details}")
    ble = {"profile_id": "hubbo:fire-tv"}
    _, dev, _ = c.call("registerDevice", "POST", "/admin/devices", dict(ble, name="Shield"), expect=201,
                       label="add a Bluetooth device")
    if not dev:
        return
    want = fixture("profiles/fire-tv.json")
    expect(dev.get("integration_id") == "ble" and dev.get("platform") == "ble_hid"
           and dev.get("kind") == want["kind"] and dev.get("model") == want["model"]
           and dev.get("commands") == want["commands"]
           and dev.get("profile", {}).get("id") == "hubbo:fire-tv",
           f"a device from a profile: {dev}")
    _, plain, _ = c.call("registerDevice", "POST", "/admin/devices", {"profile_id": "hubbo:android-tv",
                                                                     "kind": "set_top_box"},
                         expect=201, label="add with the default name")
    if plain:
        expect(plain.get("name") == "Android TV / Google TV" and plain.get("kind") == "set_top_box",
               f"name defaults to the model, kind as sent: {plain}")
        c.call("deleteDevice", "DELETE", f"/admin/devices/{plain['id']}?revision={plain['revision']}", expect=204,
               label="remove the default-name device")
    shield = dev["id"]
    expect(dev.get("bluetooth") == {"state": "not_paired"}, f"a new Bluetooth device: {dev}")
    pair = f"/admin/devices/{shield}/pair"
    status = lambda label: (c.call("getDevicePairing", "GET", pair, expect=200, label=label)[1] or {}).get("state")

    _, opened, _ = c.call("startDevicePairing", "POST", pair, expect=202)
    expect((opened or {}).get("state") == "pairing" and opened.get("window_s") == 120, f"pair: {opened}")
    _, again, _ = c.call("startDevicePairing", "POST", pair, expect=202, label="pair again while its window is open")
    expect((again or {}).get("state") == "pairing", f"pair again: {again}")
    _, body, _ = c.call("startDevicePairing", "POST", "/admin/devices/dev_apple_tv/pair", expect=409,
                        label="pair while another device pairs")
    expect(error_code(body) == "busy", f"second window: {body}")
    c.control(f"ble/bond?device={shield}")
    expect(status("after bonding") == "connected", "a bonded host is connected")
    _, listed, _ = c.call("listDevices", "GET", "/devices", expect=200, label="devices with Bluetooth status")
    mine = next((d for d in (listed or {}).get("devices", []) if d["id"] == shield), {})
    expect(mine.get("bluetooth") == {"state": "connected"} and mine.get("availability") == "available",
           f"listed Bluetooth device: {mine}")
    c.control(f"ble/link?device={shield}&up=0")
    _, got, _ = c.call("getDevice", "GET", f"/devices/{shield}", expect=200, label="host dropped")
    expect((got or {}).get("bluetooth") == {"state": "not_connected"} and got.get("availability") == "unavailable",
           f"a dropped host: {got}")
    c.control(f"ble/link?device={shield}&up=1")

    # A re-pair that runs out, or is cancelled, keeps the old host.
    c.call("startDevicePairing", "POST", pair + "?window_s=30", expect=202, label="re-pair")
    c.control("advance?ms=30000")
    expect(status("re-pair ran out") == "connected", "a re-pair that ran out keeps the old host")
    c.call("startDevicePairing", "POST", pair, expect=202, label="re-pair again")
    c.call("cancelDevicePairing", "DELETE", pair, expect=204)
    expect(status("re-pair cancelled") == "connected", "a cancelled re-pair keeps the old host")
    c.call("forgetDevicePairing", "DELETE", f"/admin/devices/{shield}/pairing", expect=204)
    expect(status("after forget") == "not_paired", "forget drops only this device's host")

    # At most 4 Bluetooth devices.
    _, devices, _ = c.call("listDevices", "GET", "/devices", expect=200, label="count Bluetooth devices")
    count = sum(1 for d in (devices or {}).get("devices", []) if d["integration_id"] == "ble")
    extra = []
    for n in range(count, 4):
        _, d, _ = c.call("registerDevice", "POST", "/admin/devices", dict(ble, name=f"Host {n + 1}"), expect=201,
                         label="Bluetooth device up to the limit")
        if d:
            extra.append(d)
    _, body, _ = c.call("registerDevice", "POST", "/admin/devices", dict(ble, name="Fifth"), expect=422,
                        label="a fifth Bluetooth device")
    detail = (error_details(body) or [{}])[0]
    expect(detail.get("code") == "too_many_devices" and detail.get("path") == "/integration_id",
           f"a fifth Bluetooth device: {body}")
    limit_text = "You can add up to 4 Bluetooth devices"
    expect(detail.get("message") == limit_text and (body or {}).get("error", {}).get("message") == limit_text,
           f"a fifth Bluetooth device's message: {body}")
    for d in extra + [dev]:
        _, cur, _ = c.call("getDevice", "GET", f"/devices/{d['id']}", expect=200, label="before removing")
        c.call("deleteDevice", "DELETE", f"/admin/devices/{d['id']}?revision={(cur or d)['revision']}", expect=204,
               label="remove a Bluetooth device")

    # The radio down: 503, and the integration says so.
    if c.control("ble/radio?ready=0"):
        _, body, _ = c.call("startDevicePairing", "POST", "/admin/devices/dev_apple_tv/pair", expect=503,
                            label="pair with the radio down")
        expect(error_code(body) == "integration_unavailable", f"radio down: {body}")
        _, ints, _ = c.call("listIntegrations", "GET", "/integrations", expect=200, label="radio down")
        radio = next((i for i in (ints or {}).get("integrations", []) if i["id"] == "ble"), {})
        expect(radio.get("state") == "error" and radio.get("detail") == "Bluetooth isn't working",
               f"radio down: {radio}")
        c.control("ble/radio?ready=1")


def noauth(c, checker):
    _, s, _ = c.call("getSession", "GET", "/auth/session", expect=200)
    mode = "enforced" if MOCK else "simulator_no_auth"  # the mock: its dev admin (see main)
    expect(s and s["auth_mode"] == mode and s["role"] == "admin", f"session: {s}")
    discovery_flow(c)
    registry_flow(c)
    execute_flow(c)
    activity_flow(c)
    smart_buttons_flow(c)
    remote_flow(c)
    remote_log_flow(c)
    ble_pairing_flow(c)
    candidate = ha_flow(c)
    admin_flow(c, checker, candidate)
    hub_flow(c)
    integration_param_flow(c)
    mute_toggle_flow(c)
    activity_runs_flow(c)
    macro_runs_flow(c)
    ha_action_flow(c)
    new_domains_flow(c)
    ha_unlink_flow(c)
    mqtt_flow(c)
    factory_reset_flow(c)


# ---------------------------------------------------------------- auth mode

def setup_and_login(c):
    """First-run setup, login checks. Returns the session cookie or None."""
    _, s, _ = c.call("getSession", "GET", "/auth/session", expect=200)
    expect(s and s["auth_mode"] == "enforced", f"session: {s}")
    if s and s["setup_required"]:
        c.call("login", "POST", "/auth/login", {"username": ADMIN, "password": PASSWORD}, expect=409,
               label="login before setup")
        c.call("listDevices", "GET", "/devices", expect=401, label="request before setup")
        c.call("setupAdmin", "POST", "/auth/setup", {"username": "not valid!", "password": PASSWORD}, expect=422,
               label="setup with a bad username")
        c.call("setupAdmin", "POST", "/auth/setup", {"username": ADMIN, "password": PASSWORD}, expect=201)
    c.call("setupAdmin", "POST", "/auth/setup", {"username": "x", "password": PASSWORD}, expect=409,
           label="setup twice")
    c.call("login", "POST", "/auth/login", {"username": "admin", "password": PASSWORD}, expect=401,
           label="no default admin username")
    c.call("listDevices", "GET", "/devices", expect=401, label="anonymous")
    c.call("login", "POST", "/auth/login", {"username": ADMIN, "password": "wrong password!"}, expect=401)
    c.call("login", "POST", "/auth/login", {"username": ADMIN, "password": PASSWORD},
           headers={"Origin": "http://evil.example"}, expect=403, label="login cross-origin")
    status, body, headers = c.call("login", "POST", "/auth/login", {"username": ADMIN, "password": PASSWORD},
                                   expect=200)
    if status != 200:
        return None
    expect("csrf_token" not in (body or {}), "no CSRF token any more")
    return http.cookies.SimpleCookie(headers.get("Set-Cookie"))["hubbo_session"].value


def token_role_matrix(c, checker, token):
    """A token can do the token operations and gets 403 on every admin one."""
    with c.acting_as(token=token["token"]):
        registry_flow(c)
        execute_flow(c)
        activity_flow(c)
        c.call("listActivityRuns", "GET", "/activity-runs", expect=200, label="token reads activity runs")
        if not MOCK:
            c.call("getRemoteLog", "GET", "/remote/log", expect=200, label="token reads the remote log")
        c.call("runMacro", "POST", "/macros/run", {"actions": [], "trigger": "guide:tap", "activity_id": None},
               expect=422, label="token tries a macro")
        _, s, _ = c.call("getSession", "GET", "/auth/session", expect=200, label="token session")
        expect(s and s["role"] == "token", f"token session: {s}")
        for op_id, method, path in operations(checker, **{"x-hubbo-role": "admin"}):
            status, body, _ = c.call(op_id, method, concrete(path, token["id"]), empty_body(checker, op_id),
                                     label="token on admin")
            expect(status == 403 and body and body["error"]["code"] == "forbidden",
                   f"token on {op_id}: expected 403 forbidden, got {status}")
        # With a real image too: a token never reaches the firmware port.
        body = upload(c, firmware_image(), 403, "token uploads firmware")
        expect(error_code(body) == "forbidden", f"token firmware upload: {body}")


def anonymous_matrix(c, checker):
    """Without credentials, every operation that isn't public is 401."""
    with c.acting_as():
        for role in ("token", "admin"):
            for op_id, method, path in operations(checker, **{"x-hubbo-role": role}):
                status, body, _ = c.call(op_id, method, concrete(path), empty_body(checker, op_id),
                                         label="anonymous")
                expect(status == 401 and error_code(body) == "unauthenticated",
                       f"anonymous {op_id}: expected 401 unauthenticated, got {status}")
                expect(error_docs(body) == GUIDE, f"anonymous {op_id}: no docs hint")


def auth_mode(c, checker):
    login = setup_and_login(c)
    if login is None:
        return
    anonymous_matrix(c, checker)
    cookie = login
    with c.acting_as(cookie=cookie):
        discovery_flow(c)
        _, e, _ = c.call("createToken", "POST", "/admin/tokens", {"name": "x"},
                         headers={"Origin": "http://evil.example"}, expect=403, label="cross-site request")
        expect(error_code(e) == "csrf_failed", f"a foreign Origin should be csrf_failed: {e}")
        _, s, _ = c.call("getSession", "GET", "/auth/session", expect=200, label="admin session")
        expect(s and s.get("username") == ADMIN, f"the user's own username: {s}")
        c.call("listDevices", "GET", "/devices", expect=200, label="admin cookie")
        _, token, _ = c.call("createToken", "POST", "/admin/tokens", {"name": "contract agent"}, expect=201)
        registry_flow(c)
        activity_flow(c)
        smart_buttons_flow(c)
        remote_flow(c)
        remote_log_flow(c)
        ble_pairing_flow(c)
        ha_flow(c)
        ha_unlink_flow(c)
        hub_flow(c)
        cookie = firmware_flow(c)
        cookie = firmware_release_flow(c)
    if not token:
        return
    if MOCK:
        skip("token role matrix, revoked token", "the mock takes no bearer tokens")
    else:
        token_role_matrix(c, checker, token)

    # Revoke: the token stops working at once. Logout ends the session.
    with c.acting_as(cookie=cookie):
        c.call("revokeToken", "DELETE", f"/admin/tokens/{token['id']}", expect=204)
    if not MOCK:
        with c.acting_as(token=token["token"]):
            c.call("listDevices", "GET", "/devices", expect=401, label="revoked token")
    with c.acting_as(cookie=cookie):
        c.call("logout", "POST", "/auth/logout", expect=204)
        c.call("listDevices", "GET", "/devices", expect=401, label="after logout")

    # No rate limit: wrong passwords stay 401, and the right one still works.
    for _ in range(8):
        c.call("login", "POST", "/auth/login", {"username": ADMIN, "password": "wrong password!"}, expect=401,
               label="wrong password again")
    cookie = admin_login(c, "login after wrong passwords")
    if cookie:
        with c.acting_as(cookie=cookie):
            factory_reset_flow(c)


# ---------------------------------------------------------------- persistence

class Hub:
    """Restarts hub-sim through api/contract.sh's supervisor (same --data)."""

    def __init__(self, base, ctl):
        self.base, self.ctl = base, ctl

    def _started(self):
        try:
            with open(os.path.join(self.ctl, "started"), encoding="utf-8") as f:
                return int(f.read().strip() or 0)
        except (OSError, ValueError):
            return 0

    def restart(self, *args, write_record=None, damage=None):
        """Stop and start hub-sim; `args` are extra hub-sim flags for the new start.
        `write_record=(name, file)` writes that record into --data while it's stopped;
        `damage=[slot file, ...]` overwrites bytes in the middle of those slots."""
        before = self._started()
        if damage:
            with open(os.path.join(self.ctl, "damage"), "w", encoding="utf-8") as f:
                f.write(" ".join(damage))
        if write_record:
            with open(os.path.join(self.ctl, "prepare"), "w", encoding="utf-8") as f:
                f.write(" ".join(write_record))
        with open(os.path.join(self.ctl, "restart"), "w", encoding="utf-8") as f:
            f.write(" ".join(args))
        deadline = time.monotonic() + 20
        while self._started() == before:
            if time.monotonic() > deadline:
                raise SystemExit("hub-sim did not restart")
            time.sleep(0.05)
        wait_for(self.base)

    def signal(self, name):
        """Send USR1 (storage unavailable) or USR2 (available) to the running hub-sim."""
        path = os.path.join(self.ctl, "signaled")
        def count():
            try:
                with open(path, encoding="utf-8") as f:
                    return int(f.read().strip() or 0)
            except (OSError, ValueError):
                return 0
        before = count()
        with open(os.path.join(self.ctl, "signal"), "w", encoding="utf-8") as f:
            f.write(name)
        deadline = time.monotonic() + 10
        while count() == before:
            if time.monotonic() > deadline:
                raise SystemExit(f"hub-sim was not signalled ({name})")
            time.sleep(0.05)
        time.sleep(0.2)  # let hub-sim handle it


def snapshot(c):
    """Everything a client can observe about the configuration, as one dict."""
    state = {}
    state["activities"] = c.call("listActivities", "GET", "/activities", expect=200)[1]
    state["global_bindings"] = c.call("getGlobalBindings", "GET", "/global-bindings", expect=200)[1]
    state["smart_buttons"] = c.call("getSmartButtons", "GET", "/smart-buttons", expect=200)[1]
    state["integrations"] = c.call("listIntegrations", "GET", "/integrations", expect=200)[1]
    devices = c.call("listDevices", "GET", "/devices", expect=200)[1] or {"devices": []}
    state["devices"] = devices
    for d in devices["devices"]:
        state[f"capabilities {d['id']}"] = c.call("getDeviceCapabilities", "GET",
                                                  f"/devices/{d['id']}/capabilities", expect=200)[1]
    status = c.call("getStatus", "GET", "/status", expect=200)[1] or {}
    state["active_activity_id"] = status.get("active_activity_id")
    state["hub"] = get_hub(c, "snapshot")
    backup = dict(c.call("downloadBackup", "GET", "/admin/backup", expect=200)[1] or {})
    backup.pop("created_at", None)
    state["backup"] = backup
    return state


def diff(a, b, path=""):
    """First difference between two JSON values, as text, or None."""
    if type(a) is not type(b):
        return f"{path}: {a!r} != {b!r}"
    if isinstance(a, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                return f"{path}/{k}: only on one side"
            d = diff(a[k], b[k], f"{path}/{k}")
            if d:
                return d
        return None
    if isinstance(a, list):
        if len(a) != len(b):
            return f"{path}: {len(a)} items != {len(b)}"
        for i, (x, y) in enumerate(zip(a, b)):
            d = diff(x, y, f"{path}/{i}")
            if d:
                return d
        return None
    return None if a == b else f"{path}: {a!r} != {b!r}"


ACTIVE_SETTLE_S = 6  # ActivePersister saves the active activity once stable for 5 s
STARTUP_SETTLE_S = 3  # the fixture activities' startup takes ~1.5 s (a wait step)


def without_revisions(value):
    if isinstance(value, dict):
        return {k: without_revisions(x) for k, x in value.items() if k != "revision"}
    if isinstance(value, list):
        return [without_revisions(x) for x in value]
    return value


def survives_restart(c, hub, what, relogin=None, settle_s=0):
    """State before == state after a restart with the same --data.

    `settle_s`: wait before restarting, for state the hub saves lazily (the
    active activity)."""
    before = snapshot(c)
    time.sleep(settle_s)
    hub.restart()
    if relogin:
        relogin()
    after = snapshot(c)
    d = diff(before, after)
    expect(d is None, f"restart after {what} changed state: {d}")
    return after


def restore_agrees(c, backup, label, expect_status):
    """A dry run is the real restore without the save: same status, same counts,
    and a failed restore changes nothing."""
    before = snapshot(c)
    dry_status, dry, _ = c.call("restoreBackup", "POST", "/admin/restore?dry_run=true", backup,
                                label=f"dry run: {label}")
    if dry_status != 200:
        d = diff(before, snapshot(c))
        expect(d is None, f"a refused dry run changed state ({label}): {d}")
    real_status, real, _ = c.call("restoreBackup", "POST", "/admin/restore", backup, label=f"restore: {label}")
    expect(dry_status == real_status, f"{label}: dry run {dry_status} but restore {real_status}")
    if expect_status is None:
        expect(400 <= real_status < 500, f"{label}: expected a refusal, got {real_status}")
    else:
        expect(real_status == expect_status, f"{label}: expected {expect_status}, got {real_status}")
    if dry_status == 200 and real_status == 200:
        counts = lambda r: {k: r.get(k) for k in ("devices", "activities", "integrations")}
        expect(counts(dry) == counts(real), f"{label}: dry run {counts(dry)} != restore {counts(real)}")
    elif real_status != 200:
        d = diff(before, snapshot(c))
        expect(d is None, f"a refused restore changed state ({label}): {d}")


def checker_buttons(c):
    return c.checker.spec["components"]["schemas"]["Button"]["enum"]


def persistence_flow(c, hub, relogin=None):
    def restart_with(*args):
        hub.restart(*args)
        if relogin:
            relogin()

    # First boot: nothing configured.
    restart_with("--reset", "--seed", "api/fixtures/admin/backup.empty.json")
    state = survives_restart(c, hub, "first boot (empty seed)", relogin)
    expect(not state["devices"]["devices"] and not state["activities"]["activities"],
           f"empty seed should leave nothing configured: {state['devices']}")

    # Setting up Home Assistant on that fresh hub: ha_main is built in.
    fresh_ha_setup(c)
    survives_restart(c, hub, "HA set up on a fresh hub", relogin)
    expect(until(lambda: integration_state(c, "ha_main") == "connected"), "the stored HA settings didn't reconnect after a restart")

    # Default seed.
    restart_with("--reset")
    survives_restart(c, hub, "default seed", relogin)

    # Each kind of edit, then a restart.
    rename_hub(c, "Den", "den", "persist the hub's name")
    c.call("updateHub", "PUT", "/admin/hub", {"radio_power": "medium"}, expect=200, label="persist radio_power")
    _, created, _ = c.call("createActivity", "POST", "/activities", fixture("activities/create.request.json"),
                           expect=201, label="persist create")
    if not created:
        return
    act = created["id"]
    survives_restart(c, hub, "create activity", relogin)
    # The revision read before the restart is still the one to send.
    edited = dict(created, name="Persisted edit")
    _, replaced, _ = c.call("replaceActivity", "PUT", f"/activities/{act}", edited, expect=200,
                            label="edit with pre-restart revision")
    c.call("replaceActivity", "PUT", f"/activities/{act}", edited, expect=409, label="stale after restart")
    survives_restart(c, hub, "replace activity", relogin)

    # An HA device is registered from a discovery; its stored capabilities
    # (never in the API's Device) must survive the restart too.
    reg = fixture("admin/register_device.request.json")
    candidate = discover_candidate(c)
    reg = registration_from(c, reg, candidate)
    status, dev, _ = c.call("registerDevice", "POST", "/admin/devices", reg, expect=201, label="persist register")
    if status != 201:
        dev = None
    caps_before = dev and c.call("getDeviceCapabilities", "GET", f"/devices/{dev['id']}/capabilities",
                                 expect=200, label="persisted HA capabilities")[1]
    survives_restart(c, hub, "register device", relogin)
    if dev:
        _, caps_after, _ = c.call("getDeviceCapabilities", "GET", f"/devices/{dev['id']}/capabilities",
                                  expect=200, label="HA capabilities after restart")
        expect(caps_before and caps_after == caps_before, "an HA device's capabilities changed across a restart")
    if dev:
        _, dev, _ = c.call("updateDevice", "PUT", f"/admin/devices/{dev['id']}",
                           {"name": "Persisted device", "revision": dev["revision"]}, expect=200)
        survives_restart(c, hub, "update device", relogin)
    if dev:
        c.call("deleteDevice", "DELETE", f"/admin/devices/{dev['id']}?revision={dev['revision']}", expect=204)
        survives_restart(c, hub, "delete device", relogin)

    gb = c.call("getGlobalBindings", "GET", "/global-bindings", expect=200)[1]
    if gb:
        gb = dict(gb, bindings=dict(gb["bindings"], **{"green:hold": [
            {"integration_id": "system", "device_id": None, "command_id": "off", "params": {}}]}))
        c.call("replaceGlobalBindings", "PUT", "/global-bindings", gb, expect=200, label="persist global bindings")
        survives_restart(c, hub, "replace global bindings", relogin)

    # Activate, restart; then delete the active activity (the hub switches to
    # the next one), restart.
    c.call("activateActivity", "POST", f"/activities/{act}/activate", expect=200)
    state = survives_restart(c, hub, "activate", relogin, settle_s=ACTIVE_SETTLE_S)
    expect(state["active_activity_id"] == act, f"active activity after restart: {state['active_activity_id']}")
    c.call("deleteActivity", "DELETE", f"/activities/{act}?revision={replaced['revision'] if replaced else 2}",
           expect=204, label="delete active activity")
    state = survives_restart(c, hub, "delete active activity", relogin, settle_s=ACTIVE_SETTLE_S)
    expect(state["active_activity_id"] != act, "deleted activity is still active after restart")
    c.call("activitiesOff", "POST", "/activities/off", expect=200)
    survives_restart(c, hub, "off", relogin, settle_s=ACTIVE_SETTLE_S)

    # Restores stamp everything with a new generation: revisions in the
    # backup may be missing or 0, and any revision read before is now stale.
    backup = c.call("downloadBackup", "GET", "/admin/backup", expect=200)[1]
    if not backup:
        return
    stale = c.call("getGlobalBindings", "GET", "/global-bindings", expect=200)[1]
    for label, edit in (("revision 0", lambda o: o.update(revision=0)), ("missing revisions", lambda o: o.pop("revision", None))):
        variant = json.loads(json.dumps(backup))
        for obj in variant["activities"] + variant["devices"] + [variant["global_bindings"]]:
            edit(obj)
        c.call("restoreBackup", "POST", "/admin/restore", variant, expect=200, label=f"restore with {label}")
        state = survives_restart(c, hub, f"restore with {label}", relogin)
        generation = (c.call("getStatus", "GET", "/status", expect=200)[1] or {}).get("config", {}).get("generation")
        expect(all(a["revision"] == generation for a in state["activities"]["activities"]),
               f"restore with {label}: activities not stamped with generation {generation}")
    if stale:
        c.call("replaceGlobalBindings", "PUT", "/global-bindings", stale, expect=409, label="stale revision after restore")

    # A large but valid backup (> 64 KiB): every trigger bound in three
    # activities. A hub must be able to restore what it can back up.
    big = fixture("admin/backup.json")
    buttons = checker_buttons(c)
    for n in range(3):
        bindings = {f"{b}:{e}": [{"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "press",
                                  "params": {"command": "Select"}}] for b in buttons for e in ("down", "up", "tap", "hold")}
        big["activities"].append({"id": f"act_bulk_{n}", "name": f"Bulk {n}", "startup": [], "shutdown": [],
                                  "bindings": bindings, "macros": [], "schema_version": 1, "revision": 1})
    size = len(json.dumps(big))
    expect(size > 64 * 1024, f"bulk backup is only {size} bytes")
    c.call("restoreBackup", "POST", "/admin/restore", big, expect=200, label=f"restore {size // 1024} KiB backup")
    survives_restart(c, hub, "large restore", relogin)

    # The restore limit derives from the config size limit (512 KiB saved
    # config + 128 KiB headroom): a pretty-printed backup well past the old
    # 256 KiB, near the config maximum, goes through the body path; one past
    # the restore limit is 413. The compiled-action cap keeps the content
    # itself smaller, so indentation makes up the size.
    limit = c.checker.ops["restoreBackup"]["x-hubbo-max-body"]
    indent, text = 0, json.dumps(big)
    while len(text) < 560 * 1024:
        indent += 2
        text = json.dumps(big, indent=indent)
    expect(512 * 1024 <= len(text) <= limit, f"pretty backup is {len(text)} bytes (limit {limit})")
    c.call("restoreBackup", "POST", "/admin/restore", raw=text.encode(), headers={"Content-Type": "application/json"},
           expect=200, label=f"restore {len(text) // 1024} KiB pretty-printed backup")
    # One byte over: refused from Content-Length alone, before the body is
    # read (so only the headers are sent; hub-sim then closes the connection).
    status, body = c.headers_only("POST", "/admin/restore", limit + 1)
    expect(status == 413, f"restore of {limit + 1} bytes: expected 413, got {status}")
    expect(isinstance(body, dict) and body["error"]["code"] == "payload_too_large", f"over-limit restore: {body}")

    restore_activity_flow(c, hub, relogin)
    blocked_flow(c, hub, relogin)
    storage_flow(c, hub, relogin)

    # Dry run and restore agree.
    no_ble = fixture("admin/backup.json")
    no_ble["integrations"] = [i for i in no_ble["integrations"] if i["id"] != "ble"]
    restore_agrees(c, no_ble, "backup without the built-in ble integration", 200)
    # A backup without smart_buttons restores to four empty slots. (Here, not
    # in smart_buttons_flow: a restore drops the Bluetooth bonds later flows
    # compare.)
    older = {k: x for k, x in fixture("admin/backup.json").items() if k != "smart_buttons"}
    restore_agrees(c, older, "backup without smart_buttons", 200)
    _, empty, _ = c.call("getSmartButtons", "GET", "/smart-buttons", expect=200, label="after an older restore")
    four_empty = fixture("admin/backup.empty.json")["smart_buttons"]["slots"]
    expect(empty and empty["slots"] == four_empty,
           f"a restore without smart_buttons leaves four empty slots: {empty and diff(empty['slots'], four_empty)}")
    huge = fixture("admin/backup.json")
    for n in range(11):  # 11 x 192 triggers > limits::kMaxCompiledActions (2048)
        huge["activities"].append({
            "id": f"act_cap_{n}", "name": f"Cap {n}", "startup": [], "shutdown": [], "schema_version": 1, "macros": [],
            "bindings": {f"{b}:{e}": [{"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "press",
                                       "params": {"command": "Select"}}]
                         for b in checker_buttons(c) for e in ("down", "up", "tap", "hold")}})
    huge["activities"] = huge["activities"][:16]
    restore_agrees(c, huge, "backup over the compiled-config capacity", None)

    c.call("restoreBackup", "POST", "/admin/restore", fixture("admin/backup.json"), expect=200, label="persist restore")
    first = survives_restart(c, hub, "restore", relogin)["backup"]
    # backup -> restore -> backup round-trips (apart from created_at).
    c.call("restoreBackup", "POST", "/admin/restore", dict(first, created_at=None), expect=200,
           label="restore own backup")
    second = snapshot(c)["backup"]
    # A restore stamps every object with the new generation; all else must match.
    d = diff(without_revisions(first), without_revisions(second))
    expect(d is None, f"backup -> restore -> backup differs: {d}")


def sim_commands(c):
    """hub-sim's command log (what the fake adapters were sent)."""
    status, body = c.host_call("GET", "/sim/log")
    expect(status == 200 and body is not None, f"/sim/log: {status}")
    return (body or {}).get("commands", [])


def commands_since(before, after):
    """Entries of `after` (a capped log) that came after the last of `before`."""
    if not before:
        return after
    last = before[-1]
    for i in range(len(after) - 1, -1, -1):
        if after[i] == last:
            return after[i + 1:]
    return after


def restore_activity_flow(c, hub, relogin=None):
    """Restore and reset never start or stop an activity: the active one stays
    if its id survives, otherwise the hub goes inactive, running nothing."""
    base = fixture("admin/backup.json")
    act = base["activities"][0]["id"]
    c.call("restoreBackup", "POST", "/admin/restore", base, expect=200, label="activity phase starts from the fixtures")
    c.call("activateActivity", "POST", f"/activities/{act}/activate", expect=200)
    time.sleep(STARTUP_SETTLE_S)  # let the startup sequence (with its waits) finish

    def quiet_restore(label, body, path="/admin/restore", op_id="restoreBackup"):
        before = sim_commands(c)
        c.call(op_id, "POST", path, body, expect=200, label=label)
        time.sleep(1)  # anything queued would have run by now
        ran = commands_since(before, sim_commands(c))
        expect(not ran, f"{label}: commands ran: {[e.get('text') for e in ran]}")

    # Same ids: the activity stays active.
    quiet_restore("restore keeping the active id", base)
    active = (c.call("getStatus", "GET", "/status", expect=200)[1] or {}).get("active_activity_id")
    expect(active == act, f"restore with the same ids: active {active!r}, expected {act!r}")

    # Different ids: inactive, no startup or shutdown.
    renamed = json.loads(json.dumps(base).replace('"act_', '"act_r_'))
    quiet_restore("restore with different activity ids", renamed)
    active = (c.call("getStatus", "GET", "/status", expect=200)[1] or {}).get("active_activity_id")
    expect(active is None, f"restore with different ids: still active {active!r}")
    state = survives_restart(c, hub, "restore with different ids", relogin, settle_s=ACTIVE_SETTLE_S)
    expect(state["active_activity_id"] is None, f"inactive after restore, then restart: {state['active_activity_id']}")

    # Factory reset while active: same rule.
    c.call("restoreBackup", "POST", "/admin/restore", base, expect=200, label="fixtures again")
    c.call("activateActivity", "POST", f"/activities/{act}/activate", expect=200)
    time.sleep(STARTUP_SETTLE_S)
    quiet_restore("factory reset while active", None, "/admin/reset", "resetConfiguration")
    activities = [a["id"] for a in (c.call("listActivities", "GET", "/activities", expect=200)[1] or {}).get("activities", [])]
    active = (c.call("getStatus", "GET", "/status", expect=200)[1] or {}).get("active_activity_id")
    expect(active == (act if act in activities else None),
           f"after reset: active {active!r} (defaults have {activities})")


def config_state(c):
    return ((c.call("getStatus", "GET", "/status", expect=200)[1] or {}).get("config") or {})


def blocked_flow(c, hub, relogin=None):
    """A saved config from newer firmware blocks the hub; restore and reset unblock it."""
    for fixture_file in ("sim/fixtures/config.newer-schema.json", "sim/fixtures/config.does-not-compile.json"):
        hub.restart(write_record=("config", fixture_file))
        if relogin:
            relogin()
        state = config_state(c)
        expect(state.get("state") == "blocked" and state.get("reason"),
               f"{fixture_file}: expected config blocked with a reason, got {state}")
        expect(c.call("getStatus", "GET", "/status", expect=200)[1].get("active_activity_id") is None,
               "a blocked hub runs nothing")
        _, body, _ = c.call("createActivity", "POST", "/activities", fixture("activities/create.request.json"),
                            expect=409, label="edit while blocked")
        expect(body and body["error"]["code"] == "config_blocked", f"edit while blocked: {body}")
        for op_id, path in (("activateActivity", "/activities/act_watch_apple_tv/activate"),
                            ("activitiesOff", "/activities/off")):
            _, body, _ = c.call(op_id, "POST", path, expect=409, label=f"{op_id} while blocked")
            expect(body and body["error"]["code"] == "config_blocked", f"{op_id} while blocked: {body}")
        # The backup is the saved record's bytes, marked as blocked, never
        # the empty runtime config.
        with open(fixture_file, encoding="utf-8") as f:
            saved = f.read()
        _, raw, headers = c.call("downloadBackup", "GET", "/admin/backup", expect=200, label="backup while blocked")
        expect(headers.get("X-Hubbo-Backup") == "blocked-raw",
               f"backup while blocked: X-Hubbo-Backup {headers.get('X-Hubbo-Backup')}")
        expect((headers.get("Content-Type") or "").startswith("application/octet-stream"),
               f"backup while blocked: Content-Type {headers.get('Content-Type')}")
        expect("hubbo-backup-BLOCKED-" in (headers.get("Content-Disposition") or ""),
               f"backup while blocked: Content-Disposition {headers.get('Content-Disposition')}")
        expect(isinstance(raw, str) and raw.strip() == saved.strip(),
               f"backup while blocked is not the saved record: {str(raw)[:120]}")
        if fixture_file.endswith("newer-schema.json") and isinstance(raw, str):
            # This firmware can't read it: refused, and nothing changes.
            _, body, _ = c.call("restoreBackup", "POST", "/admin/restore", raw=raw.encode(),
                                headers={"Content-Type": "application/json"}, expect=422,
                                label="restore a blocked-raw export from newer firmware")
            details = error_details(body) or [{}]
            expect(details[0].get("code") == "unsupported_schema", f"blocked-raw restore refusal: {body}")
            expect(config_state(c).get("state") == "blocked", "a refused restore must leave the hub blocked")
        if fixture_file.endswith("does-not-compile.json") and isinstance(raw, str):
            _, body, _ = c.call("restoreBackup", "POST", "/admin/restore", raw=raw.encode(),
                                headers={"Content-Type": "application/json"}, expect=422,
                                label="restore a blocked-raw export that doesn't compile")
            expect(config_state(c).get("state") == "blocked", "a refused restore must leave the hub blocked")
        # Still blocked after a restart: nothing rewrote the slots.
        hub.restart()
        if relogin:
            relogin()
        expect(config_state(c).get("state") == "blocked", "blocked must survive a restart untouched")
        _, raw_again, _ = c.call("downloadBackup", "GET", "/admin/backup", expect=200, label="backup, still blocked")
        expect(raw_again == raw, "the blocked record changed across a restart")
        if fixture_file.endswith("newer-schema.json"):
            # The same export once the firmware can read it (as after an
            # update): a blocked-raw record restores and unblocks.
            with open("sim/fixtures/config.does-not-compile.json", encoding="utf-8") as f:
                current = json.load(f)["schema_version"]
            upgraded = json.loads(raw) if isinstance(raw, str) else {}
            upgraded["schema_version"] = current
            upgraded.pop("a_field_from_the_future", None)
            _, done, _ = c.call("restoreBackup", "POST", "/admin/restore", upgraded, expect=200,
                                label="restore a readable blocked-raw export")
            expect(config_state(c).get("state") == "ok", f"restoring a readable record should unblock: {done}")
            c.call("restoreBackup", "POST", "/admin/restore", fixture("admin/backup.json"), expect=200,
                   label="back to the fixtures")
        else:
            _, reset, _ = c.call("resetConfiguration", "POST", "/admin/reset", expect=200, label="reset unblocks")
            expect(reset and reset["state"] == "ok", f"reset result: {reset}")
        expect(config_state(c).get("state") == "ok", "restore/reset should leave blocked")
        survives_restart(c, hub, "unblock", relogin)

    # Both config slots present but not intact: Blocked (damaged), never a
    # fresh hub with defaults saved over them; nothing intact to back up.
    hub.restart(damage=["config.a", "config.b"])
    if relogin:
        relogin()
    for attempt in ("after damage", "after another restart"):
        state = config_state(c)
        expect(state.get("state") == "blocked" and "damaged" in (state.get("reason") or ""),
               f"damaged config slots ({attempt}): expected blocked, reason damaged; got {state}")
        _, body, _ = c.call("downloadBackup", "GET", "/admin/backup", expect=409,
                            label=f"backup, config slots damaged ({attempt})")
        expect(isinstance(body, dict) and body["error"]["code"] == "config_blocked",
               f"backup with damaged config slots: {body}")
        _, body, _ = c.call("createActivity", "POST", "/activities", fixture("activities/create.request.json"),
                            expect=409, label=f"edit, config slots damaged ({attempt})")
        if attempt == "after damage":
            hub.restart()  # nothing may have written the slots: still blocked
            if relogin:
                relogin()
    _, reset, _ = c.call("resetConfiguration", "POST", "/admin/reset", expect=200, label="reset a damaged config")
    expect(config_state(c).get("state") == "ok", f"reset should leave blocked (damaged): {config_state(c)}")
    survives_restart(c, hub, "reset after damage", relogin)


def storage_flow(c, hub, relogin=None):
    """Storage down: edits get 503 storage_unavailable, switching still works.
    Credentials are read once at boot, so a medium lost at runtime doesn't log
    anyone out (with auth, the session keeps working). Back up: edits work
    again and survive a restart."""

    # Booting with storage down: the public session endpoint says so.
    hub.restart("--storage-unavailable")
    session = c.call("getSession", "GET", "/auth/session", expect=200, label="session with storage down")[1] or {}
    expect(session.get("config_state") == "storage_unavailable",
           f"boot with storage down: session config_state {session.get('config_state')}")
    # The load itself couldn't read storage: nothing trustworthy to export
    # (with auth, nobody can be authenticated either: the same 503).
    _, body, _ = c.call("downloadBackup", "GET", "/admin/backup", expect=503, label="backup, booted with storage down")
    expect(isinstance(body, dict) and body["error"]["code"] == "storage_unavailable",
           f"backup after a boot with storage down must be refused, never empty: {body}")
    hub.restart()
    if relogin:
        relogin()
    expect(config_state(c).get("state") == "ok", f"after storage came back: {config_state(c)}")

    c.call("restoreBackup", "POST", "/admin/restore", fixture("admin/backup.json"), expect=200,
           label="storage phase starts from the fixtures")
    status, _ = c.host_call("POST", "/sim/storage?health=bogus")
    expect(status == 400, f"/sim/storage with a bad value should be 400, got {status}")

    before_generation = config_state(c).get("generation")
    up_backup = c.call("downloadBackup", "GET", "/admin/backup", expect=200, label="backup before storage goes down")[1]
    hub.signal("USR1")  # storage unavailable (local control: works whatever the auth mode)
    session = c.call("getSession", "GET", "/auth/session", expect=200, label="session, storage down")[1] or {}
    expect(session.get("config_state") == "storage_unavailable",
           f"config state should follow the medium down: {session.get('config_state')}")
    down = config_state(c)
    expect(down.get("state") == "storage_unavailable" and down.get("generation") == before_generation,
           f"/status config while storage is down: {down} (generation was {before_generation})")
    create = fixture("activities/create.request.json")
    _, body, _ = c.call("createActivity", "POST", "/activities", create, expect=503, label="edit with storage down")
    expect(body and body["error"]["code"] == "storage_unavailable", f"edit with storage down: {body}")
    # Loaded fine before the medium went away: the running config is the
    # last acknowledged save, so it can still be exported.
    _, down_backup, headers = c.call("downloadBackup", "GET", "/admin/backup", expect=200,
                                     label="backup with storage down after a good load")
    expect(headers.get("X-Hubbo-Backup") == "config", f"backup with storage down: {headers.get('X-Hubbo-Backup')}")
    if isinstance(down_backup, dict) and isinstance(up_backup, dict):
        d = diff(dict(down_backup, created_at=None), dict(up_backup, created_at=None))
        expect(d is None, f"backup with storage down differs from the running config: {d}")
    gb = c.call("getGlobalBindings", "GET", "/global-bindings", expect=200)[1]
    if gb:
        c.call("replaceGlobalBindings", "PUT", "/global-bindings", gb, expect=503, label="bindings with storage down")
    _, t, _ = c.call("activateActivity", "POST", "/activities/act_watch_apple_tv/activate", expect=200,
                     label="activate with storage down")
    expect(t and t.get("switched"), f"switching must work with storage down: {t}")
    c.call("activitiesOff", "POST", "/activities/off", expect=200, label="off with storage down")

    hub.signal("USR2")  # available again
    up = config_state(c)
    expect(up.get("state") == "ok" and up.get("generation") == before_generation,
           f"/status config after storage came back: {up} (generation was {before_generation})")
    status, created, _ = c.call("createActivity", "POST", "/activities", create, expect=201,
                                label="edit after storage back")
    state = survives_restart(c, hub, "edit after storage came back", relogin)
    if status == 201:
        ids = [a["id"] for a in state["activities"]["activities"]]
        expect(created["id"] in ids, "an edit made after storage came back must survive a restart")


def auth_persistence(c, hub):
    """Credentials and tokens survive a restart; a revoked token stays revoked."""
    class LoginLost(Exception):
        pass

    def login():
        status, body, headers = c.call("login", "POST", "/auth/login", {"username": ADMIN, "password": PASSWORD},
                                       expect=200, label="login after restart")
        if status != 200:
            raise LoginLost(f"admin can't log in after a restart ({status}): credentials were not persisted")
        c.token = None
        c.cookie = http.cookies.SimpleCookie(headers.get("Set-Cookie"))["hubbo_session"].value

    try:
        hub.restart()
        login()
        keep_status, keep, _ = c.call("createToken", "POST", "/admin/tokens", {"name": "survivor"}, expect=201)
        gone_status, gone, _ = c.call("createToken", "POST", "/admin/tokens", {"name": "revoked"}, expect=201)
        if keep_status != 201 or gone_status != 201:
            return
        token_persistence(c, hub, login, keep, gone)
    except LoginLost as e:
        failures.append(str(e))


def token_persistence(c, hub, login, keep, gone):
    c.call("revokeToken", "DELETE", f"/admin/tokens/{gone['id']}", expect=204)
    tokens = c.call("listTokens", "GET", "/admin/tokens", expect=200)[1]
    hub.restart()
    with c.acting_as(token=keep["token"]):
        c.call("listDevices", "GET", "/devices", expect=200, label="token after restart")
    with c.acting_as(token=gone["token"]):
        c.call("listDevices", "GET", "/devices", expect=401, label="revoked token after restart")
    with c.acting_as(cookie=c.cookie):
        c.call("listDevices", "GET", "/devices", expect=401, label="session after restart")
    login()
    _, after, _ = c.call("listTokens", "GET", "/admin/tokens", expect=200, label="tokens after restart")
    # last_used_at is RAM only, so compare the rest.
    strip = lambda t: [{k: x for k, x in tok.items() if k != "last_used_at"} for tok in (t or {}).get("tokens", [])]
    expect(strip(tokens) == strip(after), f"tokens changed across restart: {strip(tokens)} != {strip(after)}")
    persistence_flow(c, hub, relogin=login)
    credential_state_flow(c, hub, login)


def credential_state_flow(c, hub, login):
    """"Can't read" is never "doesn't exist": credentials unreadable or damaged
    at boot never offer setup, and logins say why instead of 401. Recovery is a
    reboot (or the wipe)."""
    attacker = {"username": "mallory", "password": "a password of mine"}

    # Storage down at boot: the secrets can't be read.
    hub.restart("--storage-unavailable")
    unreadable_credentials(c, attacker, "secrets unreadable")

    # Storage back: the original admin logs in; setup is still refused.
    hub.restart()
    login()
    with c.acting_as():
        s = c.call("getSession", "GET", "/auth/session", expect=200)[1] or {}
        expect(s.get("auth_state") == "ok" and s.get("setup_required") is False, f"after storage came back: {s}")
        c.call("setupAdmin", "POST", "/auth/setup", attacker, expect=409, label="setup after storage came back")
        c.call("login", "POST", "/auth/login", attacker, expect=401, label="attacker can't log in")

    # Storage goes away at runtime: credentials were read at boot, so the
    # session, login and the setup guard all carry on.
    login()
    cookie = c.cookie
    hub.signal("USR1")
    with c.acting_as(cookie=cookie):
        c.call("listDevices", "GET", "/devices", expect=200, label="session while storage is down")
    with c.acting_as():
        s = c.call("getSession", "GET", "/auth/session", expect=200, label="session state, storage down")[1] or {}
        expect(s.get("auth_state") == "ok" and s.get("setup_required") is False, f"storage down at runtime: {s}")
        c.call("setupAdmin", "POST", "/auth/setup", attacker, expect=409, label="setup while storage is down")
    login()
    hub.signal("USR2")

    # One torn secrets slot next to an intact one: the intact record loads.
    hub.restart(damage=["secrets.b"])
    login()
    with c.acting_as():
        s = c.call("getSession", "GET", "/auth/session", expect=200, label="session, one secrets slot damaged")[1] or {}
        expect(s.get("auth_state") == "ok", f"one damaged slot should fall back to the other: {s}")

    # Both secrets slots present but not intact (torn or rotted):
    # unavailable, never a fresh hub. Absence needs proof.
    hub.restart(damage=["secrets.a", "secrets.b"])
    unreadable_credentials(c, attacker, "secrets slots damaged")

    # A damaged secrets record: setup and login refused. (Last: the network
    # can't repair credentials.)
    hub.restart(write_record=("secrets", "api/tools/contract-data/secrets.corrupt.json"))
    unreadable_credentials(c, attacker, "secrets corrupt")


def unreadable_credentials(c, attacker, why):
    """Credentials that couldn't be read at boot: `unavailable`, never a fresh
    hub; setup, login and every protected request get 503 storage_unavailable."""
    with c.acting_as():
        s = c.call("getSession", "GET", "/auth/session", expect=200, label=f"session, {why}")[1] or {}
        expect(s.get("auth_state") == "unavailable" and s.get("setup_required") is False,
               f"{why} must not look like a fresh hub: {s}")
        for op_id, path, body in (("setupAdmin", "/auth/setup", attacker),
                                  ("login", "/auth/login", {"username": ADMIN, "password": PASSWORD})):
            _, reply, _ = c.call(op_id, "POST", path, body, expect=503, label=f"{op_id}, {why}")
            expect(error_code(reply) == "storage_unavailable", f"{op_id} with {why}: {reply}")
        c.call("listDevices", "GET", "/devices", expect=503, label=f"request, {why}")


def mock_first_run(c):
    """The mock's first run (no admin, no devices): the same setup checks as
    hub-sim's auth mode, which runs them on a seeded hub the mock can't be."""
    c.control("reset", {"firstRun": True, "requireAuth": True})
    _, s, _ = c.call("getSession", "GET", "/auth/session", expect=200, label="first-run session")
    expect(s and s["setup_required"] is True and s["auth_state"] == "fresh", f"mock first run: {s}")
    setup_and_login(c)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--mode", choices=("noauth", "auth", "mock", "mock-auth"), required=True)
    ap.add_argument("--ctl", help="contract.sh supervisor directory (enables the restart checks)")
    args = ap.parse_args()
    global MOCK, CHECK_MS, DOWNLOAD_MS
    MOCK = args.mode.startswith("mock")
    if MOCK:
        CHECK_MS, DOWNLOAD_MS = 1000, 2000  # web/mock/handler.ts FW_CHECK_MS, FW_DOWNLOAD_MS
    wait_for(args.base, 90 if MOCK else 10)  # the mock's container installs node_modules first
    checker = v.SpecValidator(v.load_spec())
    global GUIDE, SPEC_LINK  # the index's links, as the spec fixes them
    links = checker.spec["components"]["schemas"]["ApiIndex"]["properties"]
    GUIDE, SPEC_LINK = links["docs"]["const"], links["openapi"]["const"]
    client = Client(args.base, checker)
    if MOCK:
        if args.mode == "mock-auth":
            mock_first_run(client)
            client.control("reset", {"requireAuth": True})  # the seed, login required
        else:  # the seed, signed in as the dev admin: same cookie rules as the hub
            client.control("reset")
            _, s, _ = client.call("getSession", "GET", "/auth/session", expect=200, label="dev session")
            client.cookie = "dev"
        skip("restart persistence, blocked config, storage faults, damaged credentials",
             "need hub-sim restarts and its storage (contract.sh's supervisor)")
    (noauth if args.mode in ("noauth", "mock") else auth_mode)(client, checker)
    if args.ctl:
        hub = Hub(args.base, args.ctl)
        if args.mode == "noauth":
            persistence_flow(client, hub)
        else:
            auth_persistence(client, hub)
    for what, why in skips:
        print(f"SKIPPED ({args.mode}): {what}: {why}")
    if failures:
        print(f"CONTRACT FAILED [{args.mode}] ({len(failures)} of {checked} checks):")
        for f in failures:
            print("  " + f)
        return 1
    print(f"CONTRACT OK [{args.mode}]: {checked} requests checked against api/openapi.yaml")
    return 0


if __name__ == "__main__":
    sys.exit(main())