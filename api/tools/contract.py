#!/usr/bin/env python3
"""Contract tests: drive a running hub-sim over HTTP and check every response
against api/openapi.yaml (status declared for the operation, body valid,
no secrets).

    contract.py --base http://127.0.0.1:18080 --mode noauth
    contract.py --base http://127.0.0.1:18081 --mode auth

noauth: SIMULATOR_NO_AUTH, every request is a simulated admin. Walks the
        Phase 2 gate flow (registry, activity CRUD/validate/activate/off,
        execute, tokens, 501s) with the request fixtures.
auth:   production auth policy. First-run setup, login, cookies + CSRF,
        tokens, the full role matrix over HTTP, Host allowlist, rate limit.

Both modes then restart hub-sim (same --data, i.e. a reboot) after a first
boot, the default seed, every kind of edit and restores, and require the
whole observable state (activities, devices, capabilities, global bindings,
integrations, active activity, backup) to be identical across the restart.
Every `revision` in every response must be >= 1. Run via api/contract.sh.
"""
import argparse
import contextlib
import http.client
import http.cookies
import json
import os
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
TRANSPORT_ERRORS = (405, 421)  # any request may get these (api/openapi.yaml conventions)

failures = []
checked = 0
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


def fixture(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as f:
        return json.load(f)


class Client:
    def __init__(self, base, checker):
        self.base = base.rstrip("/")
        self.checker = checker
        self.cookie = None
        self.csrf = None
        self.token = None

    def call(self, op_id, method, path, body=None, expect=None, headers=None, raw=None, label=None):
        """One request. Validates the response against op_id's spec; checks the status if given."""
        global checked
        url = self.base + "/api/v1" + path
        remember_secrets(body)
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        h = {"Host": "127.0.0.1"}
        if data is not None and raw is None:
            h["Content-Type"] = "application/json"
        if self.token:
            h["Authorization"] = "Bearer " + self.token
        elif self.cookie:
            h["Cookie"] = "hubbo_session=" + self.cookie
            if self.csrf and method != "GET":
                h["X-Hubbo-CSRF"] = self.csrf
        h.update(headers or {})
        req = urllib.request.Request(url, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                status, text, resp_headers = r.status, r.read().decode(), r.headers
        except urllib.error.HTTPError as e:
            status, text, resp_headers = e.code, e.read().decode(), e.headers
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
        elif status != 204:
            if body_json is None:
                failures.append(f"{where}: empty body")
            else:
                for p in self.checker.errors(op_id, str(status), body_json)[:5]:
                    failures.append(f"{where}: {p}")
        for path_, value in revisions(body_json):
            if not isinstance(value, int) or isinstance(value, bool) or value < 1:
                failures.append(f"{where}: {path_} = {value!r}; revisions start at 1")
        if body_json is not None and status < 300 and op_id != "createToken":
            for path_ in v.find_secrets(body_json):
                failures.append(f"{where}: secret field {path_} in response")
        cookie = resp_headers.get("Set-Cookie")
        if cookie:
            morsel = http.cookies.SimpleCookie(cookie).get("hubbo_session")
            if morsel is not None:
                if "httponly" not in cookie.lower() or "samesite=strict" not in cookie.lower():
                    failures.append(f"{where}: session cookie must be HttpOnly and SameSite=Strict")
        return status, body_json, resp_headers

    def host_call(self, method, path):
        """A hub-sim tool route outside /api/v1 (e.g. /sim/storage); returns (status, json)."""
        h = {"Host": "127.0.0.1"}
        if self.token:
            h["Authorization"] = "Bearer " + self.token
        elif self.cookie:
            h["Cookie"] = "hubbo_session=" + self.cookie
            if self.csrf:
                h["X-Hubbo-CSRF"] = self.csrf
        req = urllib.request.Request(self.base + path, method=method, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                status, text = r.status, r.read().decode()
        except urllib.error.HTTPError as e:
            status, text = e.code, e.read().decode()
        try:
            return status, json.loads(text) if text else None
        except ValueError:
            return status, None

    def headers_only(self, method, path, content_length):
        """Send a request's headers announcing `content_length` body bytes but
        none of the body; returns (status, json) of the early answer."""
        url = urllib.parse.urlsplit(self.base)
        conn = http.client.HTTPConnection(url.hostname, url.port, timeout=10)
        try:
            conn.putrequest(method, "/api/v1" + path, skip_host=True)
            conn.putheader("Host", "127.0.0.1")
            conn.putheader("Content-Type", "application/json")
            conn.putheader("Content-Length", str(content_length))
            if self.token:
                conn.putheader("Authorization", "Bearer " + self.token)
            elif self.cookie:
                conn.putheader("Cookie", "hubbo_session=" + self.cookie)
                if self.csrf:
                    conn.putheader("X-Hubbo-CSRF", self.csrf)
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
    def acting_as(self, token=None, cookie=None, csrf=None):
        """Temporarily switch credentials (restored afterwards)."""
        saved = (self.token, self.cookie, self.csrf)
        self.token, self.cookie, self.csrf = token, cookie, csrf
        try:
            yield self
        finally:
            self.token, self.cookie, self.csrf = saved


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
                raise SystemExit(f"hub-sim at {base} did not come up")
            time.sleep(0.2)


def concrete(path, token_id="tok_00000000"):
    return (path.replace("{integration_id}", "ha_main").replace("{device_id}", "dev_denon")
            .replace("{activity_id}", "act_watch_apple_tv").replace("{token_id}", token_id))


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
    c.call("getStatus", "GET", "/status", expect=200)
    c.call("getRemoteStatus", "GET", "/remote/status", expect=200)
    _, buttons, _ = c.call("listRemoteButtons", "GET", "/remote/buttons", expect=200)
    expect(buttons and len(buttons["buttons"]) == 48, "remote/buttons: expected 48 buttons")
    c.call("listIntegrations", "GET", "/integrations", expect=200)
    _, listing, _ = c.call("listDevices", "GET", "/devices", expect=200)
    devices = (listing or {}).get("devices", [])
    expect(devices, "no devices registered in hub-sim")
    for d in devices:
        c.call("getDevice", "GET", f"/devices/{d['id']}", expect=200)
        _, caps, _ = c.call("getDeviceCapabilities", "GET", f"/devices/{d['id']}/capabilities", expect=200)
        # hub-sim boots from api/fixtures: the served capabilities must equal
        # the fixture the web mock serves, or the mock has drifted.
        path = os.path.join(FIXTURES, "devices", f"{d['id']}.capabilities.json")
        if caps is not None and os.path.exists(path):
            expect(caps == fixture(f"devices/{d['id']}.capabilities.json"),
                   f"{d['id']}: served capabilities differ from api/fixtures (regenerate the fixture)")
    c.call("getDevice", "GET", "/devices/dev_nope", expect=404)
    c.call("getDeviceCapabilities", "GET", "/devices/dev_nope/capabilities", expect=404)


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
    # Holds are for bindings: execute refuses every `hold` command, accepts momentary ones.
    caps = c.call("getDeviceCapabilities", "GET", "/devices/dev_apple_tv/capabilities", expect=200)[1] or {}
    holds = [x["command_id"] for x in caps.get("commands", []) if x["effect"] == "hold"]
    expect("key_down" in holds, f"key_down should be a hold: {holds}")
    for command in holds:
        _, body, _ = c.call("executeCommand", "POST", f"/devices/dev_apple_tv/commands/{command}/execute",
                            {"params": {"key": "SELECT"}}, expect=422, label=f"execute hold {command}")
        codes = [d["code"] for d in (body or {}).get("error", {}).get("details", [])]
        expect("hold_not_allowed" in codes, f"execute {command}: expected hold_not_allowed, got {codes}")
    c.call("executeCommand", "POST", "/devices/dev_apple_tv/commands/key_tap/execute",
           {"params": {"key": "SELECT"}}, expect=200, label="execute key_tap")


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
    reg = registration_from(reg, candidate)  # an HA device is registered from the latest discovery
    status, dev, _ = c.call("registerDevice", "POST", "/admin/devices", reg, expect=201)
    if status == 201:
        dev_id = dev["id"]
        c.call("getDeviceCapabilities", "GET", f"/devices/{dev_id}/capabilities", expect=200)
        _, upd, _ = c.call("updateDevice", "PUT", f"/admin/devices/{dev_id}",
                           {"name": "Backlight", "revision": dev["revision"]}, expect=200)
        c.call("updateDevice", "PUT", f"/admin/devices/{dev_id}", {"name": "x", "revision": 99}, expect=409,
               label="device stale revision")
        c.call("deleteDevice", "DELETE", f"/admin/devices/{dev_id}?revision={upd['revision'] if upd else 2}",
               expect=204)
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
        c.call("setupAdmin", "POST", "/auth/setup", {"username": "admin", "password": PASSWORD}, expect=201)
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

    for op_id, method, path in operations(checker, **{"x-hubbo-not-implemented": True}):
        status, reply, _ = c.call(op_id, method, concrete(path), empty_body(checker, op_id))
        expect(status == 501 and reply and reply["error"]["code"] == "not_implemented",
               f"{op_id}: expected 501 not_implemented, got {status}")


def until(fn, seconds=5):
    """Polls fn() until it returns something truthy (hub-sim's loop runs meanwhile)."""
    deadline = time.monotonic() + seconds
    while True:
        result = fn()
        if result or time.monotonic() > deadline:
            return result
        time.sleep(0.05)


def ha_state(c):
    """The HA client's state as /status reports it."""
    _, status, _ = c.call("getStatus", "GET", "/status", expect=200, label="ha state")
    return next((i["state"] for i in (status or {}).get("integrations", []) if i["id"] == "ha_main"), None)


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
    expect(until(lambda: ha_state(c) == "connected"), "fake HA never connected with the stored token")

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

    reg = {"name": "Not in HA", "integration_id": "ha_main",
           "provider": {"type": "home_assistant", "entity_id": "switch.not_in_ha"}}
    _, e, _ = c.call("registerDevice", "POST", "/admin/devices", reg, expect=422, label="register unknown entity")
    expect(e and e["error"].get("details", [{}])[0].get("code") == "unknown_device", f"unknown entity: {e}")
    snap = {"name": "Spoof", "integration_id": "ha_main", "ha_capabilities": {},
            "provider": {"type": "home_assistant", "entity_id": "light.family_room"}}
    c.call("registerDevice", "POST", "/admin/devices", snap, expect=422, label="client ha_capabilities")
    return pick_candidate(done, registered_too=True)


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


def registration_from(reg, candidate):
    """A register request for this discovery candidate (or `reg` unchanged)."""
    return dict(reg, name=candidate["name"][:64], provider=candidate["provider"]) if candidate else reg


def discover_candidate(c):
    """Stores the fake HA's token, runs a discovery and returns an
    unregistered supported candidate (or None)."""
    _, cfg, _ = c.call("getIntegrationConfig", "GET", "/admin/integrations/ha_main", expect=200)
    if not cfg:
        return None
    c.call("configureIntegration", "PUT", "/admin/integrations/ha_main",
           {"settings": {"type": "home_assistant", "access_token": HA_TOKEN}, "revision": cfg["revision"]},
           expect=200, label="token for discovery")
    expect(until(lambda: ha_state(c) == "connected"), "fake HA never connected")
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
    expect(until(lambda: ha_state(c) == "connected"), "fresh ha_main never connected")
    c.call("discoverIntegration", "POST", "/admin/integrations/ha_main/discover", expect=202,
           label="discover on a fresh hub")

    done = wait_discovery(c)
    expect(done.get("state") == "done" and done.get("total", 0) > 0, f"fresh discovery: {done}")


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
    expect(until(lambda: ha_state(c) == "not_configured"), "without a token HA is not_configured")
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
    status, _ = c.host_call("POST", "/sim/radio/found?channel=40&address=1122334466")
    expect(status == 200, f"/sim/radio/found: {status}")
    _, found, _ = c.call("getRemotePairing", "GET", "/admin/remote/pair", expect=200)
    expect(found and found["state"] == "found" and found["channel"] == 40, f"found: {found}")
    c.call("startRemotePairing", "POST", "/admin/remote/pair", expect=202, label="scan again")
    c.call("cancelRemotePairing", "DELETE", "/admin/remote/pair", expect=204)
    _, stopped, _ = c.call("getRemotePairing", "GET", "/admin/remote/pair", expect=200, label="after cancel")
    expect(stopped and stopped["state"] == "stopped", f"after cancel: {stopped}")
    # Leave the endpoint as the fixture's.
    c.call("setRemoteEndpoint", "PUT", "/admin/remote/endpoint", fixture("admin/remote_endpoint.request.json"),
           expect=200, label="endpoint back")


def noauth(c, checker):
    _, s, _ = c.call("getSession", "GET", "/auth/session", expect=200)
    expect(s and s["auth_mode"] == "simulator_no_auth" and s["role"] == "admin", f"session: {s}")
    registry_flow(c)
    execute_flow(c)
    activity_flow(c)
    remote_flow(c)
    candidate = ha_flow(c)
    admin_flow(c, checker, candidate)
    ha_unlink_flow(c)


# ---------------------------------------------------------------- auth mode

def setup_and_login(c):
    """First-run setup, login checks. Returns (cookie, csrf) or None."""
    _, s, _ = c.call("getSession", "GET", "/auth/session", expect=200)
    expect(s and s["auth_mode"] == "enforced", f"session: {s}")
    if s and s["setup_required"]:
        c.call("login", "POST", "/auth/login", {"username": "admin", "password": PASSWORD}, expect=409,
               label="login before setup")
        c.call("setupAdmin", "POST", "/auth/setup", {"username": "admin", "password": PASSWORD}, expect=201)
    c.call("setupAdmin", "POST", "/auth/setup", {"username": "x", "password": PASSWORD}, expect=409,
           label="setup twice")
    c.call("listDevices", "GET", "/devices", expect=401, label="anonymous")
    c.call("login", "POST", "/auth/login", {"username": "admin", "password": "wrong password!"}, expect=401)
    c.call("login", "POST", "/auth/login", {"username": "admin", "password": PASSWORD},
           headers={"Origin": "http://evil.example"}, expect=403, label="login cross-origin")
    status, body, headers = c.call("login", "POST", "/auth/login", {"username": "admin", "password": PASSWORD},
                                   expect=200)
    if status != 200:
        return None
    return http.cookies.SimpleCookie(headers.get("Set-Cookie"))["hubbo_session"].value, body["csrf_token"]


def token_role_matrix(c, checker, token):
    """A token can do the token operations and gets 403 on every admin one."""
    with c.acting_as(token=token["token"]):
        registry_flow(c)
        execute_flow(c)
        activity_flow(c)
        _, s, _ = c.call("getSession", "GET", "/auth/session", expect=200, label="token session")
        expect(s and s["role"] == "token", f"token session: {s}")
        for op_id, method, path in operations(checker, **{"x-hubbo-role": "admin"}):
            status, body, _ = c.call(op_id, method, concrete(path, token["id"]), empty_body(checker, op_id),
                                     label="token on admin")
            expect(status == 403 and body and body["error"]["code"] == "forbidden",
                   f"token on {op_id}: expected 403 forbidden, got {status}")
        status, _, _ = c.call("listDevices", "GET", "/devices", headers={"Host": "evil.example"}, label="bad host")
        expect(status == 421, f"bad Host: expected 421, got {status}")


def auth_mode(c, checker):
    login = setup_and_login(c)
    if login is None:
        return
    cookie, csrf = login
    with c.acting_as(cookie=cookie):  # no CSRF header
        _, e, _ = c.call("createToken", "POST", "/admin/tokens", {"name": "x"}, expect=403, label="no csrf")
        expect(e and e["error"]["code"] == "csrf_failed", "missing CSRF should be csrf_failed")
    with c.acting_as(cookie=cookie, csrf=csrf):
        c.call("listDevices", "GET", "/devices", expect=200, label="admin cookie")
        _, token, _ = c.call("createToken", "POST", "/admin/tokens", {"name": "contract agent"}, expect=201)
        registry_flow(c)
        activity_flow(c)
        remote_flow(c)
        ha_flow(c)
        ha_unlink_flow(c)
    if not token:
        return
    token_role_matrix(c, checker, token)

    # Revoke: the token stops working at once. Logout ends the session.
    with c.acting_as(cookie=cookie, csrf=csrf):
        c.call("revokeToken", "DELETE", f"/admin/tokens/{token['id']}", expect=204)
    with c.acting_as(token=token["token"]):
        c.call("listDevices", "GET", "/devices", expect=401, label="revoked token")
    with c.acting_as(cookie=cookie, csrf=csrf):
        c.call("logout", "POST", "/auth/logout", expect=204)
        c.call("listDevices", "GET", "/devices", expect=401, label="after logout")

    # Rate limit: enough wrong passwords lock the client out.
    seen = None
    for _ in range(8):
        seen, _, headers = c.call("login", "POST", "/auth/login",
                                  {"username": "admin", "password": "wrong password!"}, label="brute force")
        if seen == 429:
            expect(headers.get("Retry-After"), "429 needs Retry-After")
            break
    expect(seen == 429, "login was never rate limited")


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
    state["integrations"] = c.call("listIntegrations", "GET", "/integrations", expect=200)[1]
    devices = c.call("listDevices", "GET", "/devices", expect=200)[1] or {"devices": []}
    state["devices"] = devices
    for d in devices["devices"]:
        state[f"capabilities {d['id']}"] = c.call("getDeviceCapabilities", "GET",
                                                  f"/devices/{d['id']}/capabilities", expect=200)[1]
    status = c.call("getStatus", "GET", "/status", expect=200)[1] or {}
    state["active_activity_id"] = status.get("active_activity_id")
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
    expect(until(lambda: ha_state(c) == "connected"), "the stored HA settings didn't reconnect after a restart")

    # Default seed.
    restart_with("--reset")
    survives_restart(c, hub, "default seed", relogin)

    # Each kind of edit, then a restart.
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
    reg = registration_from(reg, candidate)
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
        bindings = {f"{b}:{e}": [{"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "key_tap",
                                  "params": {"key": "SELECT"}}] for b in buttons for e in ("down", "up", "tap", "hold")}
        big["activities"].append({"id": f"act_bulk_{n}", "name": f"Bulk {n}", "startup": [], "shutdown": [],
                                  "bindings": bindings, "schema_version": 1, "revision": 1})
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
    huge = fixture("admin/backup.json")
    for n in range(11):  # 11 x 192 triggers > limits::kMaxCompiledActions (2048)
        huge["activities"].append({
            "id": f"act_cap_{n}", "name": f"Cap {n}", "startup": [], "shutdown": [], "schema_version": 1,
            "bindings": {f"{b}:{e}": [{"integration_id": "ble", "device_id": "dev_apple_tv", "command_id": "key_tap",
                                       "params": {"key": "SELECT"}}]
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
    quiet_restore("factory reset while active", None, "/admin/reset", "factoryReset")
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
            details = (body or {}).get("error", {}).get("details") or [{}]
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
            _, reset, _ = c.call("factoryReset", "POST", "/admin/reset", expect=200, label="reset unblocks")
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
    _, reset, _ = c.call("factoryReset", "POST", "/admin/reset", expect=200, label="reset a damaged config")
    expect(config_state(c).get("state") == "ok", f"reset should leave blocked (damaged): {config_state(c)}")
    survives_restart(c, hub, "reset after damage", relogin)


def storage_flow(c, hub, relogin=None):
    """Storage down: edits get 503 storage_unavailable. Without auth, switching
    still works; with auth nothing can be authenticated (credentials can't be
    read), so everything protected is 503. Back up: edits work again and
    survive a restart."""
    authenticated = relogin is not None

    # Booting with storage down: the public session endpoint says so.
    hub.restart("--storage-unavailable")
    session = c.call("getSession", "GET", "/auth/session", expect=200, label="session with storage down")[1] or {}
    expect(session.get("config_state") == "storage_unavailable",
           f"boot with storage down: session config_state {session.get('config_state')}")
    if not authenticated:
        # The load itself couldn't read storage: nothing trustworthy to export.
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
    if not authenticated:
        down = config_state(c)
        expect(down.get("state") == "storage_unavailable" and down.get("generation") == before_generation,
               f"/status config while storage is down: {down} (generation was {before_generation})")
    create = fixture("activities/create.request.json")
    _, body, _ = c.call("createActivity", "POST", "/activities", create, expect=503, label="edit with storage down")
    expect(body and body["error"]["code"] == "storage_unavailable", f"edit with storage down: {body}")
    if authenticated:
        c.call("downloadBackup", "GET", "/admin/backup", expect=503,
               label="backup with storage down (nobody can be authenticated)")
    else:
        # Loaded fine before the medium went away: the running config is the
        # last acknowledged save, so it can still be exported.
        _, down_backup, headers = c.call("downloadBackup", "GET", "/admin/backup", expect=200,
                                         label="backup with storage down after a good load")
        expect(headers.get("X-Hubbo-Backup") == "config", f"backup with storage down: {headers.get('X-Hubbo-Backup')}")
        if isinstance(down_backup, dict) and isinstance(up_backup, dict):
            d = diff(dict(down_backup, created_at=None), dict(up_backup, created_at=None))
            expect(d is None, f"backup with storage down differs from the running config: {d}")
    gb = None if authenticated else c.call("getGlobalBindings", "GET", "/global-bindings", expect=200)[1]
    if gb:
        c.call("replaceGlobalBindings", "PUT", "/global-bindings", gb, expect=503, label="bindings with storage down")
    if authenticated:
        c.call("activateActivity", "POST", "/activities/act_watch_apple_tv/activate", expect=503,
               label="activate with storage down (nobody can be authenticated)")
    else:
        _, t, _ = c.call("activateActivity", "POST", "/activities/act_watch_apple_tv/activate", expect=200,
                         label="activate with storage down")
        expect(t and t.get("switched"), f"switching must work with storage down: {t}")
        c.call("activitiesOff", "POST", "/activities/off", expect=200, label="off with storage down")

    hub.signal("USR2")  # available again
    if relogin:
        relogin()
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
        status, body, headers = c.call("login", "POST", "/auth/login", {"username": "admin", "password": PASSWORD},
                                       expect=200, label="login after restart")
        if status != 200:
            raise LoginLost(f"admin can't log in after a restart ({status}): credentials were not persisted")
        c.token = None
        c.cookie = http.cookies.SimpleCookie(headers.get("Set-Cookie"))["hubbo_session"].value
        c.csrf = body["csrf_token"]

    try:
        hub.restart()  # also clears the login rate limiter the brute-force check tripped
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
    """"Can't read" is never "doesn't exist": unreadable or damaged credentials
    never offer setup, and logins say why instead of 401."""
    attacker = {"username": "mallory", "password": "a password of mine"}

    # Storage down at boot: the secrets can't be read.
    hub.restart("--storage-unavailable")
    with c.acting_as():
        s = c.call("getSession", "GET", "/auth/session", expect=200, label="session, secrets unreadable")[1] or {}
        expect(s.get("auth_state") == "unavailable" and s.get("setup_required") is False,
               f"unreadable secrets must not look like a fresh hub: {s}")
        _, body, _ = c.call("setupAdmin", "POST", "/auth/setup", attacker, expect=503, label="setup, secrets unreadable")
        expect(body and body["error"]["code"] == "storage_unavailable", f"setup with secrets unreadable: {body}")
        _, body, _ = c.call("login", "POST", "/auth/login", {"username": "admin", "password": PASSWORD}, expect=503,
                            label="login, secrets unreadable")
        expect(body and body["error"]["code"] == "storage_unavailable", f"login with secrets unreadable: {body}")
        c.call("listDevices", "GET", "/devices", expect=503, label="request, secrets unreadable")

    # Storage back: the original admin logs in; setup is still refused.
    hub.restart()
    login()
    with c.acting_as():
        s = c.call("getSession", "GET", "/auth/session", expect=200)[1] or {}
        expect(s.get("auth_state") == "ok" and s.get("setup_required") is False, f"after storage came back: {s}")
        c.call("setupAdmin", "POST", "/auth/setup", attacker, expect=409, label="setup after storage came back")
        c.call("login", "POST", "/auth/login", attacker, expect=401, label="attacker can't log in")

    # Storage goes away at runtime while the admin is logged in, then comes back.
    login()
    cookie, csrf = c.cookie, c.csrf
    hub.signal("USR1")
    with c.acting_as(cookie=cookie, csrf=csrf):
        c.call("listDevices", "GET", "/devices", expect=503, label="session while storage is down")
    with c.acting_as():
        s = c.call("getSession", "GET", "/auth/session", expect=200, label="session state, storage down")[1] or {}
        expect(s.get("auth_state") == "unavailable" and s.get("setup_required") is False,
               f"storage down at runtime: {s}")
        c.call("login", "POST", "/auth/login", {"username": "admin", "password": PASSWORD}, expect=503,
               label="login while storage is down")
        _, body, _ = c.call("setupAdmin", "POST", "/auth/setup", attacker, expect=503,
                            label="setup while storage is down")
        expect(body and body["error"]["code"] == "storage_unavailable", f"setup while storage down: {body}")
    hub.signal("USR2")
    login()  # the original admin, on the very next request
    with c.acting_as():
        c.call("setupAdmin", "POST", "/auth/setup", attacker, expect=409, label="setup after storage is back")
        c.call("login", "POST", "/auth/login", attacker, expect=401, label="attacker after storage is back")

    # One torn secrets slot next to an intact one: the intact record loads.
    hub.restart(damage=["secrets.b"])
    login()
    with c.acting_as():
        s = c.call("getSession", "GET", "/auth/session", expect=200, label="session, one secrets slot damaged")[1] or {}
        expect(s.get("auth_state") == "ok", f"one damaged slot should fall back to the other: {s}")

    # Both secrets slots present but not intact (torn or rotted): Corrupt,
    # never a fresh hub. Absence needs proof.
    hub.restart(damage=["secrets.a", "secrets.b"])
    with c.acting_as():
        s = c.call("getSession", "GET", "/auth/session", expect=200, label="session, secrets slots damaged")[1] or {}
        expect(s.get("auth_state") == "corrupt" and s.get("setup_required") is False,
               f"damaged secrets slots must not look like a fresh hub: {s}")
        _, body, _ = c.call("setupAdmin", "POST", "/auth/setup", attacker, expect=409,
                            label="setup, secrets slots damaged")
        expect(body and body["error"]["code"] == "credentials_corrupt", f"setup with damaged slots: {body}")
        _, body, _ = c.call("login", "POST", "/auth/login", {"username": "admin", "password": PASSWORD},
                            expect=409, label="login, secrets slots damaged")
        expect(body and body["error"]["code"] == "credentials_corrupt", f"login with damaged slots: {body}")

    # A damaged secrets record: setup and login refused. (Last: the network
    # can't repair credentials.)
    hub.restart(write_record=("secrets", "api/tools/contract-data/secrets.corrupt.json"))
    with c.acting_as():
        s = c.call("getSession", "GET", "/auth/session", expect=200, label="session, secrets corrupt")[1] or {}
        expect(s.get("auth_state") == "corrupt" and s.get("setup_required") is False, f"corrupt secrets: {s}")
        _, body, _ = c.call("setupAdmin", "POST", "/auth/setup", attacker, expect=409, label="setup, secrets corrupt")
        expect(body and body["error"]["code"] == "credentials_corrupt", f"setup with corrupt secrets: {body}")
        c.call("login", "POST", "/auth/login", {"username": "admin", "password": PASSWORD}, expect=409,
               label="login, secrets corrupt")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--mode", choices=("noauth", "auth"), required=True)
    ap.add_argument("--ctl", help="contract.sh supervisor directory (enables the restart checks)")
    args = ap.parse_args()
    wait_for(args.base)
    checker = v.SpecValidator(v.yaml.safe_load(v.read(v.SPEC_PATH)))
    client = Client(args.base, checker)
    (noauth if args.mode == "noauth" else auth_mode)(client, checker)
    if args.ctl:
        hub = Hub(args.base, args.ctl)
        if args.mode == "noauth":
            persistence_flow(client, hub)
        else:
            auth_persistence(client, hub)
    if failures:
        print(f"CONTRACT FAILED [{args.mode}] ({len(failures)} of {checked} checks):")
        for f in failures:
            print("  " + f)
        return 1
    print(f"CONTRACT OK [{args.mode}]: {checked} requests checked against api/openapi.yaml")
    return 0


if __name__ == "__main__":
    sys.exit(main())