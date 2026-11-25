# Catalog hosting and Add device search

Started 2026-11-24 (user). The catalog goes online at hubbo.tv, and the web admin's Add device
searches it: type the manufacturer, pick it, type the model, pick it, name it, install it.
Follows `catalog-site.md` ("Hosting and publishing") and `ir.md` (profile import).

## Decisions (user, 2026-11-24)

- **Cloudflare, on the user's own domain:** the site at **`hubbo.tv`** (Cloudflare Pages, project
  `hubbo-site`); the catalog data at **`https://catalog.hubbo.tv/`** (R2 bucket `hubbo`, custom
  domain connected; `pub-78d01a13bf8845568e2765dea4d3e46b.r2.dev` is the same bucket, unused).
- **The search runs in the browser.** The web admin fetches the index and the profile from
  `catalog.hubbo.tv`, joins it, and installs it through the existing import
  (`POST /api/v1/admin/devices {profile, name?, kind?}`). The hub makes no outbound request: no
  TLS client, no flash or memory cost, no new API.
- File import stays. Bluetooth profiles (`hubbo:*`) come from the same search, then pair as usual.

## Publishing (hubbo-site)

- **`bin/site publish`** builds, then uploads, in Docker:
  - **Catalog → R2 bucket `hubbo`, at the bucket's root:** `index/` (hubbo-profiles'
    `build/index/`, so `tools/bin/index` must have run), `profiles/`, `codesets/`, and the spec
    files the pages read (kinds.json). Same tree the local `serve.py` serves under `/catalog/`.
  - **Site → Pages project `hubbo-site`:** the pages, static files and spec pages, without the
    catalog data. Its `data_base` is `https://catalog.hubbo.tv/` in a published build and stays
    `/catalog/` locally.
  - Only changed files upload (the first upload is ~310k files, ~2 GB). Objects no longer in the
    build are deleted.
  - JSON gets `Content-Type: application/json` and a `Cache-Control` (short for `index/`, longer
    for profiles and codesets).
  - The bucket's CORS allows `GET` from any origin: hubs live at LAN addresses we can't list.
- **Credentials:** `~/.config/hubbo/cloudflare.json` (mode 600: `account_id`,
  `r2_access_key_id`, `r2_secret_access_key`, `pages_token`). Read-only mount or stdin; never in
  argv, logs, the repo or the image.
- Cloudflare's bot protection answers the default Python user agent with 403; send a normal one.
- **The user attaches `hubbo.tv`** to the Pages project once (Custom domains) after the first
  deploy.

## Add device search (hubbo-frontend)

- **Add device → Search the catalog** (the default), next to Import a profile file.
  1. **Manufacturer:** filter `index/manufacturers.json` (~7k, 0.5 MB, fetched once per dialog)
     as you type; most devices first. Pick one.
  2. **Model:** filter that brand's `index/m/<slug>.json` (Samsung, the largest, is 1.4 MB); the
     kind and an optional kind filter. Pick one.
  3. **Confirm:** manufacturer, model and kind; name it; Install. It then goes the import's way
     (an IR device is ready; a Bluetooth one pairs).
- Plain matching: normalized (lowercase, no accents, letters and digits). Manufacturers match
  prefix then substring. Models match only from the start of the whole model or of one of its
  space-separated words, never mid-token, no typo tolerance ("kd85" and "KD-85" match "KD-85X85J";
  "85" matches neither it nor "XBR-55X850"). Same rule as the site (user, 2026-11-25).
- **The join is done on text,** exactly as the site does it (`catalog.js` `profile()`): the
  stored `"codeset": "<id>"` line becomes `"commands": ` plus the codeset file's commands text.
  Parsing to an object reorders digit command names. The request body embeds that text.
- Paths come from the id (the site's `profilePath` rule).
- **One catalog base URL** in the web build, `https://catalog.hubbo.tv/`. The mock and e2e use a
  small fixture catalog served locally (a few brands, an IR profile with its codeset, a
  Bluetooth profile); tests never reach the internet.
- Errors in product words: the catalog can't be reached, a profile the hub refuses (the import's
  error), a newer profile format.

## Firmware (hubbo-firmware)

- The web admin's CSP: `connect-src 'self' https://catalog.hubbo.tv`
  (`firmware/scripts/web_assets.py`). Nothing else.

## Not in this phase

- A hub setting for the catalog address; search across all brands; typo tolerance.
- Making the GitHub repos public (the site's "View on GitHub" links point at hubbo-profiles).

## Board test (dev board, .196)

1. Add device → search → install a real IR device; send a command.
2. A Bluetooth profile installs and pairs.
3. The catalog unreachable (offline browser) shows the error; file import still works.
4. `bin/hubbo board-check` passes.
