# Hubbo: working agreements

## The agent team

Hubbo is always built by a lead session and its agents. This holds in every session, and after
compaction. Nobody needs to ask for it or explain it.

- **The lead** (the main session) plans, writes the design docs, relays interfaces between agents,
  works the board through `bin/hubbo ota`, and talks to me. It owns `docs/design/`,
  `ARCHITECTURE.md`, `IMPLEMENTATION_PLAN.md`, this file, `.claude/`, `bin/`, `docker/` and
  `docker-compose.yml`. The lead delegates implementation to the agents and doesn't write their
  code.
- **The agents** are defined in `.claude/agents/`. Spawn each one by its type, and give it the same
  name so messages reach it:

  | Agent | Owns |
  |---|---|
  | `hubbo-core` | the engine, domain, config, storage, remote input and integrations (HA client/adapter, transport port), `sim/`, the CMake build |
  | `hubbo-api` | the HTTP API, auth, `ha/` capabilities, `api/` (spec, fixtures, contract), `docs/security.md`, `docs/agent-api.md`, `docs/ha-capabilities.md` |
  | `hubbo-firmware` | `firmware/`, `docs/hardware/`, the dev board |
  | `hubbo-frontend` | `web/` (the web admin, its mock and e2e) |
  | `hubbo-spec` | the device profile spec, its schema and the profiles, in `~/src/hubbo-devices` |

- At the start of a session, or when work begins, the lead spawns the agents the work needs. If an
  agent from this session is still alive, the lead messages it instead.
- Each agent keeps `docs/team/<agent>.md` current. That file holds its working notes: the state of
  its area, decisions, gotchas and open items. A fresh agent reads it first, so nothing is lost
  between sessions.
- Agents only edit their own files. A change needed in another area goes through the lead, who
  relays it to the owning agent.

## Testing without waiting around

- **While working, run only what a change touches:** one test filter (`bin/hubbo test -R Name`),
  one e2e spec, one contract mode.
- **Full gates run once per phase,** after `/simplify` and the review fixes and before the board
  test: each agent runs `bin/hubbo gate <its area>` once (core, api, web or fw), as one command.
  If a long run moves to the background, wait for its completion notice; don't split it into
  chunks. After the board test, only an agent whose files changed reruns its gate.
- `bin/hubbo web e2e --sim [--auth]` starts and stops its own hub-sim; don't run one by hand.

## Rules for everyone

- Docker only. All builds and tests go through `bin/hubbo`, and nothing is installed on the host.
- Run servers, then tests, in the foreground. Never wait on a background server, and never use
  background wait loops (`pgrep -f` matches its own command line). Stop anything you started
  before reporting.
- Secrets never show in chat, logs, argv or test output. Secrets are the OTA password, the admin
  password and the HA token, kept in `~/.config/hubbo/` (mode 600). Only the lead provisions
  them.
- Board:
  - The dev board is 192.168.1.196. Update it only through `bin/hubbo ota`.
  - Never touch 192.168.1.56, the family room Harmonizer.
  - Never weaken the watchdog.
  - Physical actions are mine.
- Hobby scale: one home, a trusted LAN, one admin. Keep things simple and don't harden for scale.
- When fixing review findings, look for a shared root cause, and consider a better design rather
  than patching each one.

## Commits by agents

The lead session delegates areas (core, API, firmware, frontend) to agents, and I want agents to
commit their own work.

- Commits happen only at the end of a phase. Each agent runs `/simplify` on its own work, the
  lead runs one independent review, the gates pass, and then I'm asked.
- A commit instruction from the lead is my explicit request to commit. This satisfies the "never
  commit without being asked" rule in `~/src/CLAUDE.md`.
- Commit with the `/commits` skill, only your own area's files, staged by explicit path (never
  `git add -A`), on the current branch, one agent at a time.
- Never commit or push on your own initiative. Pushing always needs me to ask directly.
