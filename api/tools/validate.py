#!/usr/bin/env python3
"""Lint api/openapi.yaml and validate api/fixtures against it.

Run via api/lint.sh (Docker). Checks:
  1. The spec is valid OpenAPI 3.1.
  2. Every operation has an operationId and x-hubbo-role; public operations
     declare `security: []`, the rest don't.
  3. The spec agrees with the C++ it mirrors: the policy table in
     core/src/auth/policy.cpp (role, method, body limit and type), the button and
     event names (core/src/remote/), and the forbidden param names
     (core/src/ha/capabilities.cpp).
  4. Every fixture in fixtures/index.json validates against its operation's
     request or response schema; malicious fixtures must fail.
  5. Cross-references: actions in fixtures name fixture devices of the right
     integration, existing command IDs, and params that satisfy the command's
     ParamSpec list.
  6. Secrets: secret-named properties are writeOnly in the spec, and no
     response fixture contains one.
"""
import json
import math
import os
import re
import sys

import yaml
from jsonschema import Draft202012Validator
from openapi_spec_validator import validate as validate_spec
from referencing import Registry
from referencing.jsonschema import DRAFT202012

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SPEC_PATH = os.path.join(ROOT, "api", "openapi.yaml")
FIXTURES = os.path.join(ROOT, "api", "fixtures")
CORE_SRC = os.path.join(ROOT, "core", "src")
SPEC_URI = "urn:hubbo:openapi"

SECRET_KEYS = {"password", "current_password", "new_password", "access_token"}
METHODS = ("get", "put", "post", "delete", "patch")
ROLES = {"public", "token", "admin"}
TRANSITIONS = {"activate", "off"}

# Which fixtures carry activities, by operation (kind is ignored).
ACTIVITY_OPS = {"getActivity", "createActivity", "replaceActivity", "validateActivity"}
ACTIVITY_LIST_OPS = {"listActivities", "downloadBackup", "restoreBackup"}

errors = []


def fail(msg):
    errors.append(msg)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def resolve(spec, ref):
    node = spec
    for part in ref.lstrip("#/").split("/"):
        node = node[part.replace("~1", "/").replace("~0", "~")]
    return node


def operations(spec):
    for path, item in spec["paths"].items():
        for method in METHODS:
            if method in item:
                yield path, method, item[method]


# ---------------------------------------------------------------- spec

def check_roles(spec):
    """Returns {operationId: operation}."""
    ops = {}
    for path, method, op in operations(spec):
        where = f"{method.upper()} {path}"
        op_id = op.get("operationId")
        role = op.get("x-hubbo-role")
        if not op_id:
            fail(f"{where}: missing operationId")
            continue
        if role not in ROLES:
            fail(f"{where}: x-hubbo-role must be one of {sorted(ROLES)}")
        anonymous = op.get("security") == []
        if role == "public" and not anonymous:
            fail(f"{where}: role {role} needs `security: []`")
        if role in ("token", "admin") and anonymous:
            fail(f"{where}: role {role} must not set `security: []`")
        if path.startswith("/admin/") and role != "admin":
            fail(f"{where}: /admin paths must be admin-only")
        ops[op_id] = dict(op, _method=method)
    return ops


DEFAULT_MAX_BODY = 16384  # the spec's default for a JSON body without x-hubbo-max-body


def max_body(op):
    """(limit, kind): a JSON body defaults to 16 KiB; a binary one must declare its limit."""
    body = op.get("requestBody", {}).get("content", {})
    if "application/json" in body:
        return op.get("x-hubbo-max-body", DEFAULT_MAX_BODY), "Json"
    if "application/octet-stream" in body:
        if "x-hubbo-max-body" not in op:
            fail(f"{op['operationId']}: a binary body needs x-hubbo-max-body")
        return op.get("x-hubbo-max-body", 0), "Binary"
    return 0, "Json"


def cpp_string_array(name, text):
    """The strings of `name[] = {"a", "b", ...}` in `text`."""
    return re.findall(r'"(\w+)"', re.search(name + r'\[\] = \{([^}]*)\}', text).group(1))


def query_param(spec, op_id, name):
    """Operation `op_id`'s query parameter `name`."""
    op = next(op for _, _, op in operations(spec) if op.get("operationId") == op_id)
    return next(p for p in op.get("parameters", []) if p.get("name") == name)


def cpp_constant(name, sources):
    """Value of `constexpr ... name = <integer arithmetic over other constants>;` in `sources`."""
    for text in sources:
        m = re.search(r'constexpr\s+[\w:]+\s+' + name + r'\s*=\s*([^;]+);', text)
        if m:
            expr = re.sub(r'(?:\w+::)*(k\w+)', lambda k: str(cpp_constant(k.group(1), sources)), m.group(1))
            if not re.fullmatch(r'[\d\s+*()]+', expr):
                fail(f"can't evaluate {name} = {m.group(1)}")
                return -1
            return int(eval(expr, {"__builtins__": {}}))  # digits, +, * and parentheses only
    fail(f"constant {name} not found")
    return -1


def policy_limit(limit, policy_source):
    """A policy row's max body: a literal or a constant (policy.cpp, config/json.h)."""
    if limit.isdigit():
        return int(limit)
    json_h = read(os.path.join(CORE_SRC, "..", "include", "hubbo", "config", "json.h"))
    return cpp_constant(limit, [policy_source, json_h])


def check_policy_table(ops):
    source = read(os.path.join(CORE_SRC, "auth", "policy.cpp"))
    rows = re.findall(
        r'\{\s*Operation::\w+\s*,\s*"(\w+)"\s*,\s*Method::(\w+)\s*,\s*Requirement::(\w+)\s*,'
        r'\s*(\w+)\s*(?:,\s*Body::(\w+)\s*)?\}', source)
    table = {op_id: (method.lower(), req, (policy_limit(limit, source), body or "Json"))
             for op_id, method, req, limit, body in rows}
    req_to_role = {"Public": "public", "TokenOrAdmin": "token", "AdminOnly": "admin"}
    for op_id, op in ops.items():
        if op_id not in table:
            fail(f"policy.cpp: no entry for {op_id}")
            continue
        method, req, limit = table[op_id]
        if req_to_role.get(req) != op["x-hubbo-role"]:
            fail(f"policy.cpp: {op_id} is {req}, spec says {op['x-hubbo-role']}")
        if method != op["_method"]:
            fail(f"policy.cpp: {op_id} method {method}, spec says {op['_method']}")
        if limit != max_body(op):
            fail(f"policy.cpp: {op_id} body (limit, type) {limit}, spec says {max_body(op)}")
    for op_id in table:
        if op_id not in ops:
            fail(f"policy.cpp: {op_id} is not an operation in the spec")


def check_not_implemented(ops):
    """Every operation has a handler now; a placeholder needs a 501 table in core/src/api/ again."""
    for op_id, op in ops.items():
        if op.get("x-hubbo-not-implemented"):
            fail(f"{op_id}: x-hubbo-not-implemented, but core has no 501 table")


def check_core_names(spec):
    schemas = spec["components"]["schemas"]
    buttons = re.findall(r'\{Button::\w+, kGroup\w+, 0x[0-9A-Fa-f]+, "(\w+)"\}',
                         read(os.path.join(CORE_SRC, "remote", "button.cpp")))
    events = cpp_string_array("kEventKindNames", read(os.path.join(CORE_SRC, "remote", "input.cpp")))
    if schemas["Button"]["enum"] != buttons:
        fail("Button enum differs from core/src/remote/button.cpp")
    if schemas["EventKind"]["enum"] != events:
        fail("EventKind enum differs from core/src/remote/input.cpp")
    trigger = f"^({'|'.join(buttons)}):({'|'.join(events)})$"
    if schemas["Trigger"]["pattern"] != trigger:
        fail(f"Trigger pattern should be {trigger}")
    forbidden = cpp_string_array("kForbiddenNames", read(os.path.join(CORE_SRC, "ha", "capabilities.cpp")))
    if sorted(schemas["ParamName"]["not"]["enum"]) != sorted(forbidden):
        fail("ParamName.not.enum differs from kForbiddenNames in core/src/ha/capabilities.cpp")
    domains = cpp_string_array("kEntityDomains",
                               read(os.path.join(ROOT, "core", "include", "hubbo", "ha", "capabilities.h")))
    if query_param(spec, "getDiscovery", "domain")["schema"]["enum"] != domains:
        fail("getDiscovery domain enum differs from kEntityDomains in core/include/hubbo/ha/capabilities.h")
    kinds = [k["id"] for k in json.loads(read(os.path.join(ROOT, "profiles", "kinds.json")))["kinds"]]
    if schemas["DeviceKind"]["enum"] != kinds:
        fail("DeviceKind enum differs from profiles/kinds.json")


def check_secret_props(spec, node, path="#"):
    if isinstance(node, dict):
        for key, sub in (node.get("properties") or {}).items():
            if not isinstance(sub, dict):  # a boolean schema (`false`: the property is forbidden)
                continue
            target = resolve(spec, sub["$ref"]) if "$ref" in sub else sub
            if key in SECRET_KEYS and not target.get("writeOnly"):
                fail(f"{path}/properties/{key}: secret property must be writeOnly")
        for k, v in node.items():
            check_secret_props(spec, v, f"{path}/{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            check_secret_props(spec, v, f"{path}/{i}")


# ---------------------------------------------------------------- fixtures

def schema_ref(spec, op, kind):
    if kind.startswith("request"):
        content = op["requestBody"]["content"]
    else:
        resp = op["responses"][kind]
        if "$ref" in resp:
            resp = resolve(spec, resp["$ref"])
        content = resp["content"]
    return content["application/json"]["schema"]["$ref"]


def find_secrets(value, path=""):
    if isinstance(value, dict):
        for k, v in value.items():
            if k in SECRET_KEYS:
                yield f"{path}/{k}"
            yield from find_secrets(v, f"{path}/{k}")
    elif isinstance(value, list):
        for i, v in enumerate(value):
            yield from find_secrets(v, f"{path}/{i}")


class SpecValidator:
    """Validates bodies against an operation's request/response schema."""

    def __init__(self, spec):
        self.spec = spec
        self.ops = {op["operationId"]: op for _, _, op in operations(spec)}
        self.registry = Registry().with_resource(SPEC_URI, DRAFT202012.create_resource(spec))
        self.validators = {}

    def errors(self, op_id, kind, data):
        """Schema problems for `data` as op_id's request ("request") or a response status ("200")."""
        op = self.ops.get(op_id)
        if op is None:
            return [f"unknown operation {op_id}"]
        try:
            ref = schema_ref(self.spec, op, kind)
        except KeyError:
            return [f"{op_id} declares no JSON body for {kind}"]
        return self.ref_errors(ref, data)

    def ref_errors(self, ref, data):
        """Schema problems for `data` against a `#/components/...` reference."""
        if ref not in self.validators:
            self.validators[ref] = Draft202012Validator(
                {"$ref": SPEC_URI + ref}, registry=self.registry,
                format_checker=Draft202012Validator.FORMAT_CHECKER)
        problems = sorted(self.validators[ref].iter_errors(data), key=lambda e: list(e.absolute_path))
        return [f"/{'/'.join(map(str, p.absolute_path))}: {p.message}" for p in problems]


def validate_fixtures(spec, index):
    """Schema-checks every fixture. Returns [(entry, data)]."""
    checker = SpecValidator(spec)
    loaded = []
    for entry in index:
        name, op_id, kind = entry["file"], entry["operation"], entry["kind"]
        data = json.loads(read(os.path.join(FIXTURES, name)))
        loaded.append((entry, data))
        problems = checker.errors(op_id, "request" if kind.startswith("request") else kind, data)
        if kind == "request-invalid":
            if not problems:
                fail(f"{name}: malicious request unexpectedly passes the {op_id} schema")
            continue
        for p in problems[:5]:
            fail(f"{name}: {p}")
        if not kind.startswith("request"):
            for path in find_secrets(data):
                fail(f"{name}: response contains secret field {path}")
    return loaded


def check_params(where, params, spec_params, activity_ids):
    by_name = {p["name"]: p for p in spec_params}
    for name, value in params.items():
        p = by_name.get(name)
        if p is None:
            fail(f"{where}: unknown param {name}")
            continue
        t = p["type"]
        if t == "activity_ref":
            if value not in activity_ids:
                fail(f"{where}: {name}={value!r} is not a fixture activity")
        elif t == "bool" and not isinstance(value, bool):
            fail(f"{where}: {name} must be boolean")
        elif t in ("number", "int"):
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                fail(f"{where}: {name} must be a number")
                continue
            if t == "int" and value != int(value):
                fail(f"{where}: {name} must be an integer")
            if not p["minimum"] <= value <= p["maximum"]:
                fail(f"{where}: {name}={value} outside {p['minimum']}..{p['maximum']}")
        elif t == "enum" and value not in p["options"]:
            fail(f"{where}: {name}={value!r} not in options")
    for p in spec_params:
        if p["required"] and p["name"] not in params:
            fail(f"{where}: missing required param {p['name']}")


class CrossRefs:
    """Fixture devices, their commands and activities, for action checks."""

    def __init__(self, files):
        self.devices = {d["id"]: d for d in files["devices/list.json"]["devices"]}
        self.commands = {}  # device id (None = system) -> {command_id: command}
        for name, data in files.items():
            if name.endswith(".capabilities.json"):
                self.commands[data["device_id"]] = {c["command_id"]: c for c in data["commands"]}
        system = next(i for i in files["integrations/list.json"]["integrations"] if i["id"] == "system")
        self.commands[None] = {c["command_id"]: c for c in system.get("commands", [])}
        self.activity_ids = {a["id"] for a in files["activities/list.json"]["activities"]}
        for dev_id in self.devices:
            if dev_id not in self.commands:
                fail(f"no capabilities fixture for {dev_id}")

    def check_action(self, where, action):
        dev_id = action["device_id"]
        expected = "system" if dev_id is None else self.devices.get(dev_id, {}).get("integration_id")
        if expected is None:
            fail(f"{where}: unknown device {dev_id}")
            return
        if expected != action["integration_id"]:
            fail(f"{where}: {dev_id} belongs to {expected}, not {action['integration_id']}")
        command = self.commands.get(dev_id, {}).get(action["command_id"])
        if command is None:
            fail(f"{where}: unknown command {action['command_id']}")
            return
        check_params(where, action["params"], command["params"], self.activity_ids)

    def check_activity(self, name, act):
        """Startup/shutdown never switch activity; a binding may only as its last action."""
        steps = [(f"/{seq}/{i}", a, False) for seq in ("startup", "shutdown")
                 for i, a in enumerate(act.get(seq, []))]
        steps += [(f"/bindings/{t}/{i}", a, i == len(actions) - 1)
                  for t, actions in act.get("bindings", {}).items() for i, a in enumerate(actions)]
        for path, action, may_switch in steps:
            where = f"{name}[{act.get('id', 'new')}] {path}"
            self.check_action(where, action)
            if action["device_id"] is None and action["command_id"] in TRANSITIONS and not may_switch:
                fail(f"{where}: activate/off only as the last action of a binding")


def activities_in(entry, data):
    op = entry["operation"]
    if entry["kind"] == "request-invalid":
        return []
    if op in ACTIVITY_OPS:
        return [data]
    if op == "getGlobalBindings" or op == "replaceGlobalBindings":
        return [dict(data, id="global")]
    if op in ACTIVITY_LIST_OPS and isinstance(data.get("activities"), list):
        acts = list(data["activities"])
        if "global_bindings" in data:
            acts.append(dict(data["global_bindings"], id="global"))
        return acts
    return []


class StrictLoader(yaml.SafeLoader):
    """safe_load that reports a duplicated mapping key (PyYAML keeps the last
    one silently; js-yaml, which the web build uses, refuses the file)."""

    def construct_mapping(self, node, deep=False):
        seen = set()
        for key_node, _ in node.value:
            key = self.construct_object(key_node, deep=deep)
            if key in seen:
                fail(f"{self.name} line {key_node.start_mark.line + 1}: duplicated key {key!r}")
            seen.add(key)
        return super().construct_mapping(node, deep)


def load_spec():
    """api/openapi.yaml, parsed; a duplicated key is a lint failure."""
    with open(SPEC_PATH, encoding="utf-8") as f:
        return yaml.load(f, Loader=StrictLoader)  # noqa: S506 (a SafeLoader)


def main():
    spec = load_spec()
    try:
        validate_spec(spec)
    except Exception as e:  # noqa: BLE001
        fail(f"openapi.yaml is not valid OpenAPI 3.1: {e}")
    ops = check_roles(spec)
    check_policy_table(ops)
    check_not_implemented(ops)
    check_core_names(spec)
    check_secret_props(spec, spec["components"])

    index = json.loads(read(os.path.join(FIXTURES, "index.json")))["fixtures"]
    loaded = validate_fixtures(spec, index)
    refs = CrossRefs({entry["file"]: data for entry, data in loaded})
    for entry, data in loaded:
        for act in activities_in(entry, data):
            refs.check_activity(entry["file"], act)

    if errors:
        print(f"FAILED ({len(errors)} problems):")
        for e in errors:
            print("  " + e)
        return 1
    print(f"OK: spec valid and matches core, {len(ops)} operations, {len(loaded)} fixtures validated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
