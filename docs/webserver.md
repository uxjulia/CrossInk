# Web Server Guide

This guide explains how to use CrossInk's built-in web server. It lets you
manage your reader from a phone, tablet, or computer browser: copy books over,
change settings, and add fonts. No cables or SD card swapping needed.

## Overview

The web server runs while the reader is in **Join a Network**, **Create
Hotspot**, or **Calibre Wireless** mode (all started from **File Transfer**).
From a browser it can:

- Upload, download, rename, move, and delete files on the SD card
- Create folders
- Edit many device settings
- Manage saved Wi-Fi networks and OPDS servers (online book catalogs)
- Upload and delete SD-card font families
- Accept WebDAV clients and Calibre wireless uploads

The server does not require a password. Use it only on a private network you
trust, or in hotspot mode when you control who is connected.

## Starting File Transfer

1. From the Home screen, select **File Transfer**.
2. Choose one of the available modes:

| Mode                 | Use when                                                               |
| -------------------- | ---------------------------------------------------------------------- |
| **Join a Network**   | You want the reader to join your home Wi-Fi network.                   |
| **Calibre Wireless** | You want to receive books from the CrossPoint Calibre plugin workflow. |
| **Create Hotspot**   | You want the reader to create its own open Wi-Fi network.              |

The File Transfer screen also lists **USB Drive** (X4 Classic and X4 Pro only),
the **Sync Server** actions, and **Nearby Device** options. Those do not use the
web server. See [Reading Stats Sync](./reading-stats-sync.md) and
[Nearby File Transfer](./nearby-file-transfer.md).

## Join a Network Mode

1. Select **Join a Network**.
2. If you have saved Wi-Fi credentials, CrossInk first tries the last
   connected network, then other visible saved networks in signal-strength
   order. Press **Back** to cancel or **Confirm** to stop auto-connect and show
   the network list.
3. If the network list is shown, pick a 2.4 GHz Wi-Fi network from the scan
   results.
4. Enter the password if prompted.
5. Save the password if you want the reader to reconnect automatically next time.

After connection, the reader shows:

- The connected Wi-Fi network name
- A QR code for the web URL
- The direct IP address, for example `http://192.168.1.102/`
- A name-based address, usually `http://crosspoint.local/`

Type either address into the browser of a phone, tablet, or computer on the same
Wi-Fi network, or scan the QR code.

## Create Hotspot Mode

1. Select **Create Hotspot**.
2. Connect your phone or computer to the open Wi-Fi network:

```text
CrossPoint-Reader
```

3. Open the URL shown on the reader. `http://crosspoint.local/` is preferred
   when supported; the fallback IP is typically `http://192.168.4.1/`.

The reader shows one QR code for joining the hotspot and another for opening the
web interface. The hotspot has no password, so turn it off (leave File Transfer)
when you are done.

## Calibre Wireless Mode

Calibre Wireless joins your Wi-Fi network like **Join a Network** does, then shows
setup instructions and upload progress on the reader. Keep the screen open while
you send books from Calibre. Use this mode with the CrossPoint Calibre plugin to send books from your computer.

For Calibre OPDS browsing, add `/opds` to the catalog URL when configuring an
OPDS server.

## Web Interface

The browser page has four tabs along the top: **Home**, **File Manager**,
**Settings**, and **Fonts**.

### Home

The Home tab shows the reader's serial number, firmware version, Wi-Fi status,
IP address, and free memory.

### File Manager

The File Manager tab can:

- Browse SD-card folders
- Upload files by dragging them onto the page or choosing them, with a
  **Retry All Failed Uploads** button if some fail
- Optimize EPUB files before upload
- Create folders
- Download files, and preview images
- Rename files
- Move files into existing folders
- Delete one or more selected files or empty folders

Uploading a file with the same name as an existing one replaces it. When EPUB
files are replaced, moved, renamed, or deleted through the web server, the
matching book cache (saved page layout and cover data) is cleared so old
information is not reused.

#### EPUB Optimization

When uploading an EPUB, the upload dialog can optimize the file in the browser
before sending it to the reader. This is useful for image-heavy books that are
too large or memory-sensitive for the device.

To use it, tick **Optimize EPUB** in the upload dialog. The default settings
convert images for e-ink reading, limit them to the target device size, save them
as JPEG at 85% quality, and apply basic EPUB repairs such as safer SVG handling.

**Advanced Mode** (the arrow next to **Optimize EPUB**) lets you pick the target
device, JPEG quality, image split or rotation handling, split overlap, whether to
keep the cover in color, whether to rename the file from the book's title and
author, and whether large EPUB sections are split into smaller reader sections.

Optimization changes the EPUB file contents before upload. If you sync with
**Document Matching** set to **Binary** (which identifies books by their file
contents), an optimized copy counts as a different book. Use **Filename**
matching to stay compatible. If optimization fails, the uploader sends the
original file instead.

### Settings

The Settings tab shows many of the reader's settings in the browser. Change
what you want, then press **Save Settings**. It also has sections for:

- Status bars
- Saved Wi-Fi networks
- OPDS servers

Passwords can be typed in when adding or editing entries, but saved passwords
are never shown again.

### Plugins

Enabled browser plugins appear on **Settings** or **File Manager**, depending on
the plugin. See [Plugins](./sd-plugins.md) for installation and sign-in help.

### Fonts

The Fonts tab lists installed SD-card font families and lets you upload new ones.
Choose a folder holding one font family at a time. `.cpfont` files work on every
reader. Static `.ttf` and `.otf` files (up to 2 MiB each) are accepted only on
X4 Classic, X4 Pro, and Sticky; on other readers the page says so. The reader
checks the family name, file names, and file contents before accepting an upload.
See [SD Card Fonts](./sd-card-fonts.md) and [Scalable Fonts](./scalable-fonts.md).

Installed fonts appear in **Settings > Reader > Font Options > Font Family**.
If one is missing, leave and reopen Settings, or restart the reader.

## Security Notes

- There is no password. Anyone on the same network can open the web interface
  while it is running.
- The server stops when you exit File Transfer or Calibre Wireless mode.
- Hotspot mode creates an open network; disconnect when done.

## Tips

1. Use **Create Hotspot** when no trusted network is available.
2. Prefer `crosspoint.local` when it works, but keep the displayed IP address as a fallback.
3. Move closer to the router if upload progress stalls in **Join a Network** mode.
4. Upload custom fonts through the Fonts tab or copy them to `/.fonts/` or `/fonts/` on the SD card.
5. Exit File Transfer mode when finished to conserve battery.
