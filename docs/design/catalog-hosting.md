# The device catalog and Add device search

The catalog is every device profile Hubbo knows: a few hand-written ones plus the whole Harmony
device archive, converted into Hubbo's open profile format. It's published at
[hubbo.tv/devices](https://hubbo.tv/devices/), and the web admin's Add device searches it: type the
manufacturer, pick it, type the model, pick it, name it, install it.

## Where it lives

- **[hubbo-profiles](https://github.com/hubbo-remote/hubbo-profiles)** is the open standard and the
  data: the spec, its schema and validator, and every profile as committed data (CC0). The Harmony
  archive was imported once; since then the committed data is the source of truth, and a fix is an
  edit to a profile file and a pull request, validated by CI. Nothing is trimmed for age or kind:
  the catalog is preservation too.
- **Storage:** each Harmony codeset is stored once, and device files reference it. Whatever serves a
  profile joins the device with its codeset, so the hub always gets one complete profile.
- **hubbo-site** builds the site from it: the
  homepage, the catalog (browse, search, a page per device with its downloadable profile) and the
  spec's pages. It's a static site; the device pages render in the browser from the profile JSON,
  since hundreds of thousands of prebuilt pages is too many.
- **Hosting:** the site on Cloudflare Pages at `hubbo.tv`, the catalog data (index, profiles,
  codesets) on Cloudflare R2 at `https://catalog.hubbo.tv/`. The bucket's CORS allows `GET` from
  any origin, since hubs live at LAN addresses nobody can list.
- The Harmony data is transformed into Hubbo's format, never mirrored, and the site's copy says
  "works with Harmony remotes". hubbo-profiles' README credits the archive it came from.

## Search runs in the browser

The web admin fetches the index and the profile from `catalog.hubbo.tv` and installs it through
the ordinary profile import (`POST /admin/devices` with the profile). The hub makes no outbound
request for it: no TLS client, no flash or memory cost, no new API. The only firmware change is the
web admin's CSP: `connect-src 'self' https://catalog.hubbo.tv`.

1. **Manufacturer:** filter `index/manufacturers.json` (~7k, fetched once per dialog) as you type;
   most devices first.
2. **Model:** filter that brand's `index/m/<slug>.json`, with the kind and an optional kind filter.
3. **Confirm:** manufacturer, model and kind; name it; Install. It then goes the import's way (an
   IR device is ready; a Bluetooth one pairs).

- **Plain matching**, the same rule as the site: normalized (lowercase, no accents, letters and
  digits). Manufacturers match prefix then substring. Models match only from the start of the whole
  model or of one of its space-separated words, never mid-token, no typo tolerance ("kd85" and
  "KD-85" match "KD-85X85J"; "85" matches neither it nor "XBR-55X850").
- **The join is done on text,** exactly as the site does it: the stored `"codeset": "<id>"` line
  becomes `"commands": ` plus the codeset file's commands text. Parsing to an object would reorder
  digit command names. The request body embeds that text.
- Paths come from the profile id, by the site's rule.
- **One catalog base URL** in the web build (`VITE_HUBBO_CATALOG_URL` overrides it). The mock and
  e2e use a small fixture catalog served locally; tests never reach the internet.
- Errors in product words: the catalog can't be reached, a profile the hub refuses (the import's
  error), a newer profile format. File import stays as the offline path.

## Not done

A hub setting for the catalog address; search across all brands; typo tolerance.
