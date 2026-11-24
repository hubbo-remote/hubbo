#!/usr/bin/env python3
"""Checks the dev board against firmware/budget.json (see firmware/README.md, "Board budget").

Usage: bin/hubbo board-check [ip] [--clients N]   (admin.json on stdin; bin/hubbo passes it)

Windows, each read with GET /debug/stats?reset=1 (which returns the window that's ending):
  idle    nothing going on
  loaded  an IR hold and N parallel HTTP clients
  flash   two settings saves (unchanged values; each is still a flash write)
  end     the last read: since-boot values (stack and heap minimums, sizes)

A rule names a dotted path into the stats (tasks by name: tasks.httpd.stack_free_min) and a
limit per window: {"max": {"idle": 4000, "flash": 12000}}, {"min": {"end": 2048}}, or
{"report": ["idle"]} to print without judging. "needs" skips a rule when that path is 0, and a
task that isn't running (tasks.<name> absent) is skipped, not failed.

read->HID latency comes from real presses during the run. The check never presses buttons or
switches activities. Admin credentials are read from stdin and never printed.
Exits 1 if any rule fails, a console command is refused, or the board reboots during the run.
The check only sleeps between reads, so its own requests don't load the board; a reboot shows as
uptime going backwards, and a board that stays unreachable counts as one.
"""
import argparse, http.cookiejar, json, os, sys, threading, time
import urllib.error, urllib.parse, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def get(base, path, timeout=10):
    with urllib.request.urlopen(base + path, timeout=timeout) as r:
        return r.read()


def dig(doc, path):
    for key in path.split("."):
        doc = doc.get(key) if isinstance(doc, dict) else None
    return doc


def fmt(v, unit):
    if v is None:
        return "missing"
    if unit == "us":
        return f"{v / 1000:.2f} ms"
    if unit == "bytes":
        return f"{v / 1024:.1f} KB" if v < 1 << 20 else f"{v / (1 << 20):.2f} MB"
    if unit == "pct_x100":
        return f"{v / 100:.1f}%"
    return str(v)


class Board:
    def __init__(self, base):
        self.base = base
        self.uptime = 0
        self.stalls = 0

    def read(self, reset=False):
        # A busy board (sockets still held by the load clients) can refuse a request or time
        # out without rebooting, so retry for a while; only uptime going backwards, or no
        # answer at all for 30 s, means it rebooted.
        deadline = time.time() + 30
        while True:
            try:
                doc = json.loads(get(self.base, "/debug/stats" + ("?reset=1" if reset else "")))
                break
            except urllib.error.HTTPError as e:
                if e.code != 503 or not reset:
                    raise
                # The loop didn't close the window in time: a failure in itself. The board
                # keeps that close pending, so the next reset still returns its window.
                print("  FAIL  the loop didn't answer a window close within 200 ms")
                self.stalls += 1
            except (OSError, ValueError):
                if time.time() > deadline:
                    self.rebooted()
                time.sleep(1)
        if doc["uptime_ms"] < self.uptime:
            self.rebooted()
        self.uptime = doc["uptime_ms"]
        doc["tasks"] = {t["name"]: t for t in doc["tasks"]}
        return doc

    def rebooted(self):
        print("FAIL: the board rebooted during the check")
        for _ in range(30):
            try:
                log = get(self.base, "/debug/log").decode(errors="replace")
            except OSError:
                time.sleep(2)
                continue
            for line in log.splitlines():
                if "previous run" in line or "reset reason" in line.lower():
                    print("  " + line.strip())
            break
        sys.exit(1)


def console(base, cmd):
    """Runs a dev console command; the board answers non-2xx when it refuses one."""
    try:
        get(base, "/debug/run?cmd=" + urllib.parse.quote_plus(cmd))
    except urllib.error.HTTPError as e:
        sys.exit(f"FAIL: the board refused `{cmd}`: {e.code} {e.read().decode(errors='replace').strip()}")


def load_clients(base, n, stop):
    paths = ["/api/v1/status", "/", "/api/v1/openapi.json"]

    def client(k):
        while not stop.is_set():
            try:
                get(base, paths[k % len(paths)])
            except OSError:
                stop.wait(0.2)  # don't hammer a struggling board with reconnects
            k += 1

    threads = [threading.Thread(target=client, args=(i,), daemon=True) for i in range(n)]
    for t in threads:
        t.start()
    return threads


def save_settings(base, cred):
    """Saves the hub's settings twice, unchanged: two real flash writes, nothing changes."""
    op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def call(method, path, body=None):
        req = urllib.request.Request(base + "/api/v1" + path, method=method,
                                     data=None if body is None else json.dumps(body).encode())
        req.add_header("Content-Type", "application/json")
        with op.open(req, timeout=15) as r:
            return json.loads(r.read() or b"{}")

    call("POST", "/auth/login", {"username": cred["username"], "password": cred["password"]})
    # The hub writes its settings record on every save, even an unchanged one.
    hub = call("GET", "/admin/hub")
    for _ in range(2):
        call("PUT", "/admin/hub", {"check_updates": hub["check_updates"]})


def judge(rules, reads):
    failed = 0
    for rule in rules:
        unit = rule.get("unit", "")
        for w in rule.get("report", []):
            print(f"  info  {rule['name']} ({w}): {fmt(dig(reads[w], rule['path']), unit)}")
        for op, limits in (("max", rule.get("max", {})), ("min", rule.get("min", {}))):
            for w, limit in limits.items():
                if "needs" in rule and not dig(reads[w], rule["needs"]):
                    print(f"  skip  {rule['name']} ({w}): no samples")
                    continue
                task = rule["path"].split(".")[1] if rule["path"].startswith("tasks.") else None
                if task and task not in reads[w]["tasks"]:
                    print(f"  skip  {rule['name']} ({w}): {task} isn't running")
                    continue
                v = dig(reads[w], rule["path"])
                ok = v is not None and (v <= limit if op == "max" else v >= limit)
                failed += not ok
                sign = "<=" if op == "max" else ">="
                print(f"  {'ok  ' if ok else 'FAIL'}  {rule['name']} ({w}): {fmt(v, unit)} {sign} {fmt(limit, unit)}")
    return failed


def main():
    cred = json.load(sys.stdin)
    budget = json.load(open(os.path.join(ROOT, "firmware", "budget.json")))
    load, windows = budget["load"], budget["windows"]
    ap = argparse.ArgumentParser()
    ap.add_argument("ip", nargs="?", default="192.168.1.196")
    ap.add_argument("--clients", type=int, default=load["clients"])
    args = ap.parse_args()
    base = f"http://{args.ip}"
    board = Board(base)

    doc = board.read()
    wait = budget["settle_ms"] - doc["uptime_ms"]
    if wait > 0:
        print(f"board-check: booted {doc['uptime_ms'] // 1000} s ago; settling {wait // 1000} s")
        time.sleep(wait / 1000)
    print(f"board-check: {doc['version']} at {args.ip}")
    reads = {}
    board.read(reset=True)  # starts the idle window

    print(f"  idle {windows['idle']} s")
    time.sleep(windows["idle"])
    reads["idle"] = board.read(reset=True)

    s = windows["loaded"]
    print(f"  loaded {s} s: IR hold, {args.clients} HTTP clients")
    stop = threading.Event()
    console(base, f"ir hold {s * 1000}")
    load_clients(base, args.clients, stop)
    time.sleep(s)
    stop.set()
    reads["loaded"] = board.read(reset=True)

    print(f"  flash {windows['flash']} s: two settings saves")
    save_settings(base, cred)
    time.sleep(windows["flash"])
    reads["flash"] = reads["end"] = board.read(reset=True)

    print()
    failed = judge(budget["rules"], reads) + board.stalls
    print("\nboard-check: " + (f"FAILED ({failed} over budget)" if failed else "passed"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
