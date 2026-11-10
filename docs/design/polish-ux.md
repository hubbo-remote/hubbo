# Polish: copy and layout

The web admin works. Now it has to feel like a product. This phase is the first round of the polish
pass in `IMPLEMENTATION_PLAN.md`, and it runs before Phase 6, which comes last (user decision,
2026-11-10). It changes only the web admin. The API, the firmware and the mock's behaviour stay as
they are.

## 1. Copy

The user's direction (2026-11-10): copy should read like a regular app, very light and simple.

- **Short.** One line where a paragraph is now. Most fields need only their label. Most pages need
  only their title.
- **No labels you don't need.** A control that is obvious from its value or icon gets none. Prefer
  placeholders and clear button text over helper text.
- **Don't explain everything.** Say what matters in the control and its state: a button's word, an
  empty state, an error. Drop notes that teach how the hub works inside.
- **No implementation or development detail.** No sequences, slots, NVS, sockets, bonds, endpoints,
  sessions, tokens in RAM, the simulator, the CLI, the serial console, OTA tooling, "the hub
  answers", etc. Keep a technical word only where the user must act on it, for example an API
  token or an IP address.
- **Not our setup.** Don't assume an Apple TV, Home Assistant, a particular remote or a
  particular room. Bluetooth is for any Bluetooth device. Examples and placeholders are generic.
- **Errors** say what happened and what to do, in a few words.
- **Destructive actions** keep their typed confirmations, with one short line on what's lost.

Sweep every page, dialog, toast, empty state, error and the setup wizard. Accessible names follow
the same rules: short, no explanations.

## 2. Navigation

| Before | After |
|---|---|
| Dashboard (home), Activities, Devices, Integrations, Settings | **Activities (home)**, Devices, Integrations, **Status**, Settings |

- **Activities is the home page,** at `/`. Its job is to switch activities fast: each activity is
  one tap to start, the running one is plain to see, and Off is always at hand. Editing, duplicating
  and deleting stay on the page but come second. Global buttons stay reachable from it.
- **Status** is the old Dashboard, renamed. It sits under Integrations in the nav. It's for checking
  on the hub and fixing things, so it's not first. It has tabs:
  - **Overview:** the old Dashboard's health tiles (the activity switcher moves to Activities);
  - **Backups:** download and restore;
  - **Firmware:** the version and the update;
  - **Diagnostics:** moved from Settings.
- **Settings** keeps Account, API tokens, Remote, Network and the two resets (Reset configuration
  and Factory reset). Appearance is removed from Settings: the theme switch is already in the nav.
- No redirects: the product has never shipped. Links point at the new places, and the old routes
  are gone (user, 2026-11-10).

## 3. Out of scope this round

Visual redesign, new features and the API's own error text. If an API message reads badly in the
UI, list it in your notes, and the lead takes it to the API agent.

## 4. Before and after

The user wants to see the change. Before any edits, screenshot every page, every tab and the main
dialogs on the mock at desktop (1280×800) and phone (390×844) sizes, in the light theme. Do it
again afterwards with the same names, so each pair can be shown side by side. Include the setup
wizard steps.

## Gate

`bin/hubbo gate web`, and the user reviews the before and after page. The board isn't needed: the
dev server proxies to a local hub-sim.
