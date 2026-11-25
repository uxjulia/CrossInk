---
title: Nearby Position Sync
nav_order: 8
---

# Nearby Position Sync

CrossInk can share the current EPUB reading position between two nearby readers running CrossInk. The sync is direct reader-to-reader over a direct connection; it does not need a Wi-Fi network, a server, an account, or KOReader's online sync service.

Nearby Position Sync is separate from [Reading Stats Sync](./reading-stats-sync.md). Stats Sync shares all-time reading totals. Nearby Position Sync shares one in-book location for the currently open EPUB.

## What Gets Synced

Nearby Position Sync shares your place in the open EPUB. The receiving reader
finds the corresponding page, even if its font size or layout is different.

It does not sync files, bookmarks, clippings, reading stats, KOReader server data, Wi-Fi settings, OPDS servers, or reader options.

The receiving reader only applies the position after you confirm it.

## Requirements

- Both readers must be running CrossInk.
- Both readers must have the same EPUB open.
- Both readers must be near each other.

CrossInk checks that both readers have the same book using the **Document Matching** setting from **Settings > System > Sync Server**. With **Filename** matching it compares file names. With **Binary** matching it compares the contents of the EPUB file.

## How To Use It

1. Open the same EPUB on both readers.
2. On the reader with the position you want to copy, go to that page.
3. On both readers, open **Nearby Position Sync**. On an X3, X4, or X4 Classic, open the Reader Menu and look in the tab with the bookmark and sync options. You can also assign **Nearby Position Sync** to a shortcut or Quick Action in **Settings > Controls**.
4. Press **Share** on the reader that is already at the correct page.
5. The other reader shows the nearby position beside its current local position.
6. On the receiving reader, press **Select** to apply the nearby position, or **Back** to leave the current position unchanged.

Only one reader needs to press **Share**. The other reader should stay on the Nearby Position Sync screen and wait for the shared position.

## Common Messages

**Different book. Position not synced**

The two readers are not opened to the same EPUB according to the current document matching method. Open the same book on both readers and try again.

**Version mismatch. Position not synced**

The other reader is using an incompatible version of Nearby Position Sync. Update both readers to the same CrossInk version.

**No nearby reader found** or **Position sync timed out**

The readers did not find each other within the sync window. Keep both readers close together, leave both on the Nearby Position Sync screen, and press **Share** on only the reader that should send its current position.
