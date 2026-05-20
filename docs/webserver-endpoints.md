---
title: Webserver Endpoints
nav_order: 4
---

# Webserver Endpoints

This document describes the HTTP and WebSocket endpoints exposed while CrossInk File Transfer is running.

Examples use `crosspoint.local`. If mDNS does not resolve, use the IP address shown on the device screen, for example `http://192.168.1.102/`.

## Overview

- HTTP server: port 80
- WebSocket upload server: port 81
- Hostname: `crosspoint.local` when mDNS is available
- Network modes: STA (`Join Network`) or AP (`Create Hotspot`)

The web UI also initializes a WebDAV handler for WebDAV clients. This page documents the first-party HTTP and WebSocket API surface used by CrossInk's web pages.

## Pages and Static Assets

| Method | Path | Description |
| --- | --- | --- |
| `GET` | `/` | Home/status page |
| `GET` | `/files` | File manager page |
| `GET` | `/settings` | Settings, OPDS, and WiFi management page |
| `GET` | `/fonts` | SD-card font management page |
| `GET` | `/js/jszip.min.js` | Bundled JSZip asset used by the file page |

## Status

### `GET /api/status`

Returns current device/network status.

```sh
curl http://crosspoint.local/api/status
```

```json
{
  "version": "1.3.0",
  "ip": "192.168.1.102",
  "mode": "STA",
  "rssi": -45,
  "freeHeap": 123456,
  "uptime": 3600
}
```

`mode` is `STA` for joined WiFi and `AP` for hotspot mode. `rssi` is `0` in AP mode.

## Files

### `GET /api/files`

Lists files and folders in a directory.

```sh
curl "http://crosspoint.local/api/files?path=/Books"
```

Query parameters:

| Parameter | Required | Default | Description |
| --- | --- | --- | --- |
| `path` | No | `/` | Directory path to list |

Response:

```json
[
  {"name":"MyBook.epub","size":1234567,"isDirectory":false,"isEpub":true},
  {"name":"Notes","size":0,"isDirectory":true,"isEpub":false}
]
```

Dot-files are hidden unless `showHiddenFiles` is enabled. Protected system items such as `System Volume Information` and `XTCache` are hidden either way.

### `GET /download`

Downloads one file.

```sh
curl -OJ "http://crosspoint.local/download?path=/Books/MyBook.epub"
```

Query parameters:

| Parameter | Required | Description |
| --- | --- | --- |
| `path` | Yes | File path to download |

Common errors include `Missing path`, `Invalid path`, `Access denied to protected path`, `Item not found`, `Failed to open file`, and `Path is a directory`.

### `POST /upload`

Uploads one file using multipart form data.

```sh
curl -X POST -F "file=@mybook.epub" "http://crosspoint.local/upload?path=/Books"
```

Query parameters:

| Parameter | Required | Default | Description |
| --- | --- | --- | --- |
| `path` | No | `/` | Target directory |

Successful response:

```text
File uploaded successfully: mybook.epub
```

Common errors include `Access denied to protected path`, `Failed to create file on SD card`, `Failed to write to SD card - disk may be full`, `Failed to write final data to SD card`, `Upload aborted`, and `Unknown error during upload`.

Existing files with the same sanitized filename are overwritten. EPUB uploads clear the affected EPUB cache.

### `POST /mkdir`

Creates a folder.

```sh
curl -X POST -d "name=NewFolder&path=/" http://crosspoint.local/mkdir
```

Form parameters:

| Parameter | Required | Default | Description |
| --- | --- | --- | --- |
| `name` | Yes | - | Folder name |
| `path` | No | `/` | Parent directory |

Successful response:

```text
Folder created: NewFolder
```

Common errors include `Missing folder name`, `Invalid folder name`, `Access denied to protected path`, `Folder already exists`, and `Failed to create folder`.

### `POST /rename`

Renames one file.

```sh
curl -X POST -d "path=/Books/old.epub&name=new.epub" http://crosspoint.local/rename
```

Form parameters:

| Parameter | Required | Description |
| --- | --- | --- |
| `path` | Yes | Existing file path |
| `name` | Yes | New filename, not a full path |

Only files can be renamed through this endpoint. On success, it returns `Renamed successfully`. Renaming an EPUB clears the old path's EPUB cache.

### `POST /move`

Moves one file into an existing folder.

```sh
curl -X POST -d "path=/Books/mybook.epub&dest=/Archive" http://crosspoint.local/move
```

Form parameters:

| Parameter | Required | Description |
| --- | --- | --- |
| `path` | Yes | Existing file path |
| `dest` | Yes | Existing destination folder |

Only files can be moved through this endpoint. On success, it returns `Moved successfully`. Moving an EPUB clears the old path's EPUB cache.

### `POST /delete`

Deletes one file, one empty folder, or a batch of items.

```sh
# Single item
curl -X POST -d "path=/Books/mybook.epub" http://crosspoint.local/delete

# Batch
curl -X POST --data-urlencode 'paths=["/Books/a.epub","/Books/b.epub"]' http://crosspoint.local/delete
```

Form parameters:

| Parameter | Required | Description |
| --- | --- | --- |
| `path` | Yes, unless `paths` is sent | Single path to delete |
| `paths` | Yes, unless `path` is sent | JSON array of paths |

Send either `path` or `paths`, not both. Folders must be empty before deletion. On full success, it returns `All items deleted successfully`; partial failures return `Failed to delete some items: ...`.

Common errors include:

| Status | Body | Cause |
| --- | --- | --- |
| 400 | `Missing "path" or "paths" argument` | Neither parameter was provided |
| 400 | `Provide either 'path' or 'paths', not both` | Both delete parameters were sent |
| 400 | `Invalid paths format` | `paths` was not valid JSON |
| 400 | `No paths provided` | `paths` was an empty JSON array |
| 500 | `Failed to delete some items: ...` | One or more paths could not be deleted |

## Settings

### `GET /api/settings`

Returns settings that are editable through the web UI. The response is a JSON array with each setting's `key`, translated `name`, `category`, `type`, current `value`, and any type-specific fields such as `options`, `min`, `max`, or `step`.

```sh
curl http://crosspoint.local/api/settings
```

### `POST /api/settings`

Applies settings by key and saves them to SD.

```sh
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"bionicReadingEnabled":1}' \
  http://crosspoint.local/api/settings
```

Successful response:

```text
Applied 1 setting(s)
```

## Fonts

### `GET /api/fonts`

Lists installed SD-card font families.

```sh
curl http://crosspoint.local/api/fonts
```

Response shape:

```json
{
  "families": [
    {
      "name": "Literata",
      "sizes": [12, 14, 16, 18],
      "files": [{"name": "Literata_14.cpfont", "size": 12345}]
    }
  ],
  "maxFamilies": 128
}
```

### `POST /api/fonts/upload`

Uploads one `.cpfont` file into a named font family.

```sh
curl -X POST \
  -F "file=@Literata_14.cpfont" \
  "http://crosspoint.local/api/fonts/upload?family=Literata"
```

The server validates the family name, filename, and `.cpfont` magic bytes. Successful response:

```json
{"ok":true}
```

### `POST /api/fonts/delete`

Deletes an installed SD-card font family.

```sh
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"family":"Literata"}' \
  http://crosspoint.local/api/fonts/delete
```

Successful response:

```json
{"ok":true}
```

## OPDS Servers

### `GET /api/opds`

Lists saved OPDS servers. Passwords are not returned; `hasPassword` indicates whether one is saved.

```sh
curl http://crosspoint.local/api/opds
```

### `POST /api/opds`

Adds or updates an OPDS server.

```sh
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"name":"Catalog","url":"https://example.com/opds","username":"user","password":"secret"}' \
  http://crosspoint.local/api/opds
```

Include `index` to update an existing entry. If updating and `password` is omitted, the existing password is preserved.

### `POST /api/opds/delete`

Deletes a saved OPDS server by index.

```sh
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"index":0}' \
  http://crosspoint.local/api/opds/delete
```

## WiFi Credentials

### `GET /api/wifi`

Lists saved WiFi credentials. Passwords are not returned; `hasPassword` indicates whether one is saved.

```sh
curl http://crosspoint.local/api/wifi
```

### `POST /api/wifi`

Adds or updates a saved WiFi network.

```sh
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"ssid":"MyNetwork","password":"secret"}' \
  http://crosspoint.local/api/wifi
```

Include `index` to update an existing entry. If updating and `password` is omitted, the existing password is preserved.

### `POST /api/wifi/delete`

Deletes a saved WiFi network by index.

```sh
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"index":0}' \
  http://crosspoint.local/api/wifi/delete
```

## WebSocket Upload

### Port 81

The file page uses a WebSocket endpoint for fast binary uploads.

Connection:

```text
ws://crosspoint.local:81/
```

Protocol:

1. Client sends text: `START:<filename>:<size>:<path>`
2. Server responds: `READY`
3. Client sends binary chunks
4. Server sends progress as `PROGRESS:<received>:<total>`
5. Server sends `DONE` or `ERROR:<message>`

Error messages include:

| Message | Cause |
| --- | --- |
| `ERROR:Upload already in progress` | Another WebSocket upload is active |
| `ERROR:Invalid START format` | Malformed `START` message or invalid size |
| `ERROR:Access denied to protected path` | Target path is protected |
| `ERROR:Failed to create file` | File could not be opened for writing |
| `ERROR:No upload in progress` | Binary data arrived without a valid upload |
| `ERROR:Upload overflow` | Client sent more bytes than declared |
| `ERROR:Write failed - disk full?` | SD write failed |

Successful EPUB uploads clear the affected EPUB cache.

## Path Rules

- Paths are normalized to start with `/`.
- Trailing slashes are stripped except for root `/`.
- Protected paths cannot be listed, uploaded into, downloaded, moved, renamed, or deleted.
- Protected items include dot paths, `System Volume Information`, and `XTCache`.
