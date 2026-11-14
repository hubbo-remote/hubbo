# The device catalog and the Hubbo site

Approved by the user on 2026-11-13. This phase makes hubbo-profiles the home of every device
profile and hubbo-site Hubbo's homepage. It also converts the Harmony archive into our own profile format.
Research: `research-device-catalog.md`.

## Two repos (user, 2026-11-13, supersedes the single-repo layout below)

- **`~/src/hubbo-profiles` is the open standard and the data.** It holds the spec, the schema and
  the validator as an open spec, documented so people can read it, learn it and build on it. It
  also holds every profile as committed data: the hand-written ones, plus the whole Harmony
  catalog in split form (a file per device, each codeset once). It's CC0. People contribute by
  editing a profile file and opening a pull request, and CI validates the change. Anyone can
  clone it, or download a release archive, without touching the site.
- **Nothing is removed.** The catalog is preservation, so no device is trimmed for age or kind.
- **The archive is imported once.** After the import, the committed data is the source of truth.
  The converter becomes the import tool, and the overrides fold into the data, because a fix is
  just an edit to the file. Spec changes later become migration scripts over the data.
- **`~/src/hubbo-site` is the site:** the homepage, the catalog and pages that present the spec,
  built from hubbo-profiles. The official spec is the one in hubbo-profiles; the site shows it.
- Both repos stay local until the user publishes them.
- **The final hubbo-profiles layout:**
  - `docs/` holds the spec (`spec.md`), the schemas (`schema/v<N>/`, kept forever), the changelog and the using guide.
  - `profiles/hubbo/` holds the hand-written profiles.
  - `profiles/harmony/<n//1000>/<n>.json` holds the Harmony devices, and `codesets/<xx>/<id>.json` the codesets.
  - `tools/` holds the validate, import, index, migrate and release tools.
  - The index is generated into `build/index/` and isn't committed.
  - The hub's `bin/hubbo profiles sync` reads `profiles/hubbo/` and `docs/schema/v1/{kinds,buttons}.json`.

## Decisions (user, 2026-11-13)

- **The spec, the profiles, the converter and the site** are split across the two repos above.
- **The Harmony archive is converted now,** into our spec. The spec isn't frozen: the conversion
  is the spec's IR pass, and the format changes wherever real devices don't fit. Nothing has
  shipped yet, so there's no compatibility work.
- **Site first, the hub later.** The web admin's Add device doesn't search the catalog in this
  phase.
- **Built and previewed locally.** Hosting and a domain come later.

## The site (hubbo-site)

- **A static site** built from hubbo-profiles, with one command: `bin/site build`.
  `bin/site serve` previews it locally. Both run in Docker.
- **Homepage:** what Hubbo is, how it works, what it needs (the board and the radio), and where to
  get it. The copy is light, in the web admin's style, and has no developer detail. It can't link
  to a download until Phase 6, so it says it's coming.
- **The catalog:**
  - browse by kind and by manufacturer;
  - search runs in the browser from the index files: typo-tolerant manufacturer matching, then
    the model, ignoring case and punctuation;
  - one page per device: its name, kind and platform, its commands, power, inputs, and where it
    came from (hand-written or Harmony);
  - each device page offers its profile JSON, which a later phase lets the web admin fetch.
- **Output** is a plain file tree in `build/` that any static host can serve, so it can move
  to Cloudflare as-is later.
- **Scale:** hundreds of thousands of pages is too many to prebuild comfortably. If so, device
  pages can be rendered in the browser from the profile JSON, by one template. That's the site
  agent's call (it renders them in the browser).
- **Stack:** small and boring, chosen by the site agent and stated in its notes. It needs no server
  at runtime and no client framework unless it earns its place.

## Out of scope

- Hosting, a domain, CI and publishing.
- The web admin searching the catalog, and the firmware's CSP change for it.
- IR sending on the hub.

## Hosting and publishing (user, 2026-11-13)

- **Storage:** each Harmony codeset is stored once, and device files reference it. The site joins
  a device with its codeset, so the hub still gets one complete profile.
- **Hosting, when it comes:** Cloudflare R2 behind a custom domain, served through the cache. The
  site build publishes the profiles, codesets and index from hubbo-profiles and uploads them.
- **Publishing the Harmony data is settled.** The user accepts the risk, with these as the rules:
  - the data is transformed into our format, never mirrored;
  - no Logitech branding on the site's product copy: "works with Harmony remotes". The exception
    (user, 2026-11-13) is that hubbo-profiles' README credits the Logitech Harmony IR archive and
    the projects it credits, and its License section follows that archive's: CC0 for our work, with
    no claim over the underlying codes, which originate with Logitech;
  - ~~a takedown contact on the site~~: dropped by the user on 2026-11-13, along with the footer's
    credits line. The credit lives in hubbo-profiles' README and on the Harmony device pages.
