# SD card plugins

CrossInk can load plugin interfaces in its web portal and browse declarative
plugin catalogs on the reader. JavaScript runs in the connected browser;
`device.json` describes requests and lists that the firmware understands.
No JavaScript engine runs on the reader.

This integration supports browser interfaces and action jobs, JSON/XML catalogs,
authentication, book downloads, bundle installation, catalog details, firmware
events, and protected EPUB books with device-wrapped AES-128 or AES-256 keys.

## Installation and discovery

Enable **Settings → System → Plugins**, then open **Home → Plugins**. Clean installations start disabled. Upgrading with existing plugin folders enables plugins automatically; disabling leaves their files intact.

The optional **Plugin Hub** entry offers Download, Hide, and Cancel. Download installs the current Hub using the same recoverable bundle transaction as other plugin installs. Hide removes only this entry; enabling Plugins again restores it. Sideloaded plugins do not require the Hub. Manual downloads are available from [Plugin Hub releases](https://github.com/crosspoint-reader/crosspoint-plugin-hub/releases).

Copy a plugin folder onto the SD card under one of these roots, in priority order:

1. `/.crosspoint/plugins/<name>`
2. `/plugins/<name>`
3. `/.plugins/<name>`

An earlier root wins when folders have the same name. Use simple folder names
such as `my-library`; names cannot contain separators, parent references, FAT
short aliases, or leading/trailing spaces. Folder names are limited to 80 bytes.
The picker discovers up to 16 plugins while inspecting at most 128 entries.

A browser plugin contains `plugin.js`. Optional `manifest.json` supplies its
`title` and `mount` (`settings` by default, or `files`). Open Home → File Transfer
on the reader, join a network, and visit the reader's web portal from a browser.
The plugin appears on the matching Settings or Files page.

A device catalog contains `device.json`. Open **Plugins** on Home in any theme
(shown whenever Plugins is enabled) to select it. OPDS Browser is available under **Home → File Transfer**. Browser-only
and background-only manifests can appear in this list, but do not open a device
catalog. Read their setup instructions in the browser or on your computer.

Install plugins you trust: their browser scripts run with the web portal's
permissions. Plugin file operations cannot access CrossInk's Wi-Fi, OPDS, or
KOReader credential stores, including backups and legacy Wi-Fi credentials.
Showing hidden files does not expose those stores. Other plugin tokens and
configuration are ordinary files on the SD card.

## Browser interface

Register a renderer in `plugin.js`:

```js
CrossPoint.registerPlugin(async (container, api) => {
  const title = document.createElement('h2');
  title.textContent = 'My library';
  container.append(title);
  // api.name and api.dir identify this installation.
});
```

| API | Purpose |
| --- | --- |
| `api.relay(method, url, headers, body)` | Request HTTP(S) through the device; returns `{status, headers, body}`. |
| `api.fetchToSd(url, dest, headers)` | Download to SD in resumable segments, publishing only a complete file. |
| `api.writeFile(path, base64)` | Write a binary or text file through multipart upload. |
| `api.pluginFile(filename)` | URL for a file in this plugin's folder. |
| `api.registerAction(name, fn)` | Register a browser action callable through the job queue. |

Relay bodies are limited to 32 KiB; larger downloads use `fetchToSd`. File uploads
are limited to 256 KiB. Plugin requests reject HTTP framing headers and control
characters. Credentials are not forwarded to another origin on redirects or
through remote catalog links. Network requests use the existing SecureNet TLS
configuration; no new certificate trust policy is introduced by this feature.

Replacing a book clears its cached content. Reading preferences and progress
continue to use CrossInk's existing stores. Existing file-manager rename and move
operations keep their reader-state migration behavior.

## Device catalog example

```json
{
  "title": "My library",
  "description": "Download books",
  "config": { "file": "config.json" },
  "token": { "file": "token.json", "path": "access_token" },
  "browse": {
    "url": "https://library.example/books?page={page}&limit={limit}",
    "headers": { "Authorization": "Bearer {token}" },
    "items": "books",
    "fields": { "title": "title", "author": "author", "id": "id", "url": "url" },
    "page_size": 8
  },
  "download": {
    "url": "https://library.example/books/{id}/file",
    "headers": { "Authorization": "Bearer {token}" },
    "dest_dir": "/Books",
    "filename": "{title}.epub"
  }
}
```

Omit `token` for a public catalog. Token and configuration filenames relative to
the manifest live inside the plugin folder. Absolute paths must pass the same
credential protection as browser file operations.

Field selectors use dotted JSON paths, including array indexes such as
`authors.0.name`. `browse.items` is empty for a root array. Requests support
`method`, `headers`, and `body`. Templates support `{token}`, `{cfg.KEY}`, `{page}`,
`{offset}`, `{limit}`, `{query}` (URL encoded), and `{query_raw}`. Download and
item-details templates also support `{id}`, `{title}`, `{author}`, and `{url}`.
The `_enc` forms (`{id_enc}`, `{title_enc}`, `{author_enc}`, `{url_enc}`) encode
those values for a URL query value or path segment. This is advertised as
`catalog.encoded_item_fields`; existing raw forms remain unchanged.

Outgoing plugin URLs escape spaces, control bytes, and non-ASCII bytes, while
preserving URL separators and existing percent escapes. Use `_enc` when a field
contains separators such as `&`, `?`, `#`, or `/` that belong to the value itself.
Request-size, header, and destination protections still apply.

JSON pagination is selected from the URL, body, and header templates:

- `{page}` with `{limit}`: limit equals `page_size`; a full page enables Next.
  An exactly full final page can lead to one empty page.
- `{offset}`: offset equals `(page - 1) * page_size`, and limit equals
  `page_size + 1` for lookahead. Offset takes precedence over page.
- `{page}` without `{limit}`: each page needs an extra row to advertise Next.
- Neither: the firmware pages through the static response locally, counting
  compatible entries rather than letting incompatible entries consume slots.

Optional `browse.lists` contains named `{title, url, body}` views.
`browse.search` overrides `url` and/or `body` for search. A `download.url_path`
selects a URL from an API response; credentials used for that API request are
not sent to the returned file URL. `download.username` and `download.password`
provide Basic authentication for a direct file request.

For XML catalogs set `browse.format` to `xml`, `browse.item` to the repeated
element's local name, and field selectors to an element, `element@attribute`,
or `@attribute`. Optional `container_element`, `skip_self`, `resolve_urls`, and
`extensions` support directory listings. Directory links must keep the manifest's
origin, including HTTPS and port.

A flat config file can override the destination with `{"dest_dir":"/Books"}`.
`/` selects the SD root. Filenames retain CrossInk's normal sanitation.

## Sign-in

Plugins can save their token through the browser interface, or declare an `auth`
object. `type: "password"` uses `request` and `token_path` to mint a token from
configuration values. `type: "device_code"` uses `request` to obtain a code and
verification URL, then `poll` to await authorization. Response selectors include
`code_path`, `verify_url_path`, `device_code_path`, `interval_path`, `expires_path`,
`token_path`, and `error_path`. The reader shows the code and QR during sign-in.
Expired password tokens receive one refresh attempt. Failed token writes retain
the previous token.

## Bundles and resource limits

A download may declare `bundle: {"base":"base", "files":"files", "subdir":"{id}"}`.
The named item fields supply a base URL and array of relative file paths. The
subdirectory must be one valid folder name. A bundle can contain up to 64 files.

Updates are assembled in a hidden sibling folder, preserving existing local
configuration. After all files arrive, a journal records the folder swap. Boot
recovery retains either the prior installation or the completed replacement.
Copying the prior folder is limited to 128 entries and eight nested levels.
Keep downloaded books outside the plugin installation folder.

Manifests are limited to 8 KiB; picker titles and descriptions accept at most 128
and 256 bytes respectively. Oversized picker fields use the folder name or no
description. Token and configuration files are limited to 2 KiB. JSON pages contain at most 16 visible rows; XML
lists use the SDK's 200-row limit. A list requires enough free heap for its row
array plus 64 KiB of parser and UI headroom; otherwise it shows a memory error. Captured row text is limited to 24 KiB per
response. Catalog responses stream through an SD temporary file and are limited
to 1 MiB; oversized or malformed responses show a feed error. HTTP response
headers are limited to 64 fields and 8 KiB. These bounds keep catalog input from
growing memory without limit on button devices without PSRAM.

## Browser action jobs

An external client may POST `{plugin, action, args}` to `/api/plugin-jobs`, then
poll `/api/plugin-jobs/status?id=<id>`. Jobs run only while a browser hosts the
plugin on Files, Settings, or `/plugins-run`. The device has six job slots.
Running claims have leases; a lost browser may cause an action to run again, so
actions should tolerate retries. Jobs are not persisted across firmware restart.

## Dynamic catalogs and compatibility

`browse.lists_url` can return an object containing a `lists` array with `title`, `url`, and optional `body`. The response is limited to 16 KiB and the first 16 usable lists. Each list can include a `notice` with `title`, `message`, `confirm`, and `cancel`; the notice must be accepted before browsing.

Catalog requests support `{firmware_family}`, `{firmware_version}`, `{plugin_api}`, and `{plugin_capabilities}`. This integration advertises the `crosspoint` catalog family, the CrossInk version, API 1, and `catalog.lists`, `catalog.notices`, `catalog.bundle_install`, `catalog.version_tracking`, and `catalog.compatibility`. It also advertises `catalog.item_details`: catalogs may map `description`, `detail_author`, `repository`, `details_url`, and `details_format` in `browse.fields`. Version-tracked catalogs default to `description`, `author`, `repository`, and `details.url`/`details.format`. Text or HTML details are limited to 8 KiB; HTML is displayed as readable text with paragraph breaks. Details page before downloading, and Back cancels without downloading. Only the current page is wrapped in memory.

`browse.compatibility` can map `firmware_family`, `plugin_api_min`, and `requires` to JSON row fields. Missing restrictions allow a row; supplied family/API/capability restrictions must match. Filtering keeps the server's original pagination lookahead. Completed book downloads through catalogs or browser plugin APIs mark the Library for refresh. Bundles updating an existing plugin reuse its original root and preserve its configuration.

## Firmware events

A plugin's `device.json` can subscribe through an `events` object to `reader.open`,
`reader.exit`, `reader.session`, `book.downloaded`, and `sleep.enter`. The Plugins
list displays the subscribed events. Disabling Plugins stops collection and delivery.
Handlers use `request` or `download` objects with `{event.*}`, `{cfg.*}`, `{meta.*}`,
and `{token}` substitutions. Downloads use the same protected-path checks as catalog downloads.

Events are queued in each plugin's `events.jsonl` (4 KiB plus one bounded line).
Ordinary delivery is ordered and at least once: services should deduplicate `{event.id}`.
Failed requests retain the queue; successful delivery publishes the remaining tail
through a recoverable file swap. An overfull outbox drops old events to bound SD use.
Normal online delivery is throttled to one event every 30 seconds. Sleep subscriptions
imply a saved-network connection attempt, limited to 10 seconds, and a separate 10-second delivery budget for the newest sleep event of each subscribed plugin (up to eight plugins), before the sleep image is drawn. The same deadline also permits one queued non-sleep `connect: true` handler per plugin. Other events remain queued. Low-battery sleep leaves events queued. The delivery deadline is a soft limit on hardware: Arduino DNS resolution can block outside the TCP timeout.

Reader sessions count completed forward-page reading intervals (2–1800 seconds),
emit after at least 10 active seconds, and exclude idle exit time. EPUB, TXT and XTC
report progress on a 0–10000 scale. Timestamps use a persisted device clock floor;
RTC adoption and network time synchronization prevent ordinary clock rollback.

## Protected books

Plugins can use `/api/crypto` for the upstream crypto operations and post
`{path, key, expires}` to `/api/book-key`. `key` is a base64-encoded 16- or
32-byte content key; `expires` is UTC epoch seconds, or zero for no expiry.
The destination must be an EPUB path outside protected firmware stores.
`/api/status?plugin=<name>` returns a stable, per-plugin device ID.

Keys are authenticated and wrapped using a device secret stored in internal NVS,
then saved beside the book as `<book>.key`. Copying the SD card to another reader
does not transfer access. Erasing NVS requires fulfilling the books again. Key
replacement preserves the prior file through interrupted publication.

Encrypted resources are decrypted as the reader requests them. Access is checked
before loading existing book caches. Expired loans are rejected; loans with no
trusted clock require a Wi-Fi time sync. The persisted clock cannot move backwards,
but time spent powered off without a working RTC can delay expiry enforcement.
Expiry is checked when opening a book, not continuously during an open session.

The source EPUB remains encrypted. Existing page, CSS, image, cover, and reading
caches can contain decoded content; this integration does not encrypt all SD data.
Service enrollment and account credentials remain the plugin's responsibility;
real service enrollment requires that service's supported plugin and credentials.

Native simulator builds require `wolfssl` and `openssl` discoverable by
`pkg-config`. The simulator stores its device secret in `.crossink-device-secret`
outside `fs_`, or at `CROSSINK_DEVICE_SECRET_PATH`, with owner-only permissions.
Never copy that file when testing access from a second simulated device.

### Plugin file cache validation

`GET /plugin?name=<plugin>&file=<file>` returns a quoted content-MD5 `ETag` and
`Cache-Control: no-cache`. Matching `If-None-Match` requests receive `304` with
no body. Same-size file changes produce a new tag; read failures return `500`.
The digest is a cache validator, not an authenticity check.
