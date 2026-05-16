---
title: Common Issues
nav_order: 5
---

# Common Issues

This guide covers common CrossInk problems, what they usually mean, and the safest first fix to try. CrossInk uses a small ESP32-C3 with no PSRAM, so many problems are caused by stale cache files, SD card writes, weak WiFi, or running out of memory while a book is being prepared.

- [Web Server and WiFi](#web-server-and-wifi)
- [Books and Reading](#books-and-reading)
- [Covers and Thumbnails](#covers-and-thumbnails)
- [Reading Stats and Progress](#reading-stats-and-progress)
- [Updates](#updates)
- [When to Report a Bug](#when-to-report-a-bug)

## Web Server and WiFi

### Browser cannot reach the device

**What you see:** Your browser says "Cannot connect", "Site cannot be reached", or the page keeps loading.

**Try this:**

1. Make sure your computer or phone is on the same WiFi network as the reader.
2. Type the full address shown on the reader, including `http://`.
3. Disable VPN or private relay features temporarily.
4. Move closer to the router and check that the reader still shows a connected WiFi screen.
5. Try another private 2.4 GHz network if the current network blocks device-to-device traffic.

The web server only runs while the WiFi screen is active. If you leave that screen, WiFi shuts down to save battery.

### Upload fails or stops partway through

**What you see:** A file upload fails, stalls, or the web page reports a write error.

**Try this:**

1. Check that the SD card has free space.
2. Try a smaller EPUB first to confirm the connection works.
3. Refresh the browser page and upload again.
4. Keep the reader awake and close to the router during the upload.
5. If the same file fails repeatedly, copy it to the SD card from a computer and check whether the reader can open it.

### Saved WiFi password no longer works

**What you see:** The reader tries a saved network but fails to connect.

**Try this:**

1. When prompted, choose **Forget Network**.
2. Reconnect to the network.
3. Re-enter the password carefully.
4. Save the password again if this is a trusted network.

### Settings page reboots or fails while using hotspot or WiFi

**What you see:** The web Settings page fails to load, returns an error, or the device restarts.

**Try this:**

1. Install the newest CrossInk release for your build variant.
2. Restart the reader before opening WiFi again.
3. Close other browser tabs connected to the reader.
4. If the page fails to load, leave WiFi mode, re-enter it, and try again.

If it still reboots, report the exact build variant, what page you opened, and whether the issue happened on a hotspot or normal WiFi. On production builds, some debug logs are intentionally reduced, so the exact action sequence matters.

## Books and Reading

### A book shows "Indexing..." the first time it opens

**What you see:** A new EPUB opens slowly and shows an "Indexing..." message.

**This is usually normal.** CrossInk prepares book metadata and page layout data the first time a book opens.

**Try this:**

1. Let indexing finish.
2. Keep the reader awake until the book opens.
3. If the same book always indexes from scratch, use **Delete Book Cache** for that book, reopen it, and let indexing finish once.

Very large EPUBs, books with many tiny chapters, image-heavy books, or books with complex CSS can take longer.

### A book starts indexing again after sleep or after changing reader settings

**What you see:** You were reading a book, put the device to sleep, woke it, and indexing appears again. This can also happen after changing font, spacing, image, bionic-reading, or style settings.

**Why it happens:** CrossInk stores two main kinds of EPUB cache:

- `book.bin` for metadata like title, author, spine, and table of contents.
- `sections/*.bin` for the laid-out pages of each chapter.

Reader settings affect `sections/*.bin`, so changing layout-related settings can require rebuilding the current chapter even if the book itself is already known.

**Try this:**

1. Let the indexing finish once.
2. If it repeats every wake for the same book, use **Delete Book Cache** on that book.
3. Reopen the book and let indexing finish.
4. Sleep and wake again to confirm it stays fixed.
5. If it still repeats, try changing **Embedded Style** once. That setting is part of the section-cache signature, so changing it forces the current chapter layout to rebuild.
6. Delete the book cache again.
7. Reopen the book and let indexing finish.
8. Sleep and wake again to test the behavior.

### A book says it is corrupted or badly formatted

**What you see:** The reader exits the book or shows an error saying the EPUB is likely corrupted or badly formatted.

**Try this:**

1. Optimize the EPUB using the Web UI's **Optimize EPUB** option, or repair it with an EPUB tool on your computer.
2. Delete that book's cache and try again.
3. If the same file still fails, re-download or re-copy it to the SD card and try again.

This message usually means CrossInk hit invalid book data or could not safely rebuild the chapter on the device. It is better for the reader to exit safely than to keep reopening the same failing book state.

### Reader settings changed but the current book still looks wrong

**What you see:** Alignment, paragraph spacing, focus reading, images, or style changes do not appear immediately, or the page looks stale.

**Try this:**

1. Exit and reopen the book.
2. Use **Delete Book Cache** for that book.
3. Reopen the book and let indexing finish.
4. If the problem is only on the home screen, recent books, or sleep cover, clear the generated cover or thumbnail cache by deleting the book cache.

Live reading pages, book metadata, and cover thumbnails are different cached files. Clearing the specific book cache is usually safer than deleting the entire `.crosspoint` folder.

### Should I delete `.crosspoint`?

Only delete the full `.crosspoint` folder as a last resort.

The `.crosspoint` folder contains more than render cache. It also stores settings, saved servers, recent books, bookmarks, reading progress, and reading stats. For normal book problems, prefer one of these first:

1. **Delete Book Cache** from the reader menu or file browser for one affected book.
2. **Settings > System > Clear Reading Cache** for all EPUB/XTC cache folders. This can remove per-book progress and per-book stats, but leaves settings and global stats in place.
3. Manual deletion of only the affected `/.crosspoint/epub_<hash>/` folder if a developer asks for that.

## Covers and Thumbnails

### A cover or thumbnail is missing

**What you see:** A book opens, but Home, Recent Books, OPDS, or sleep mode has no cover image.

**Try this:**

1. Open the book once from the file browser.
2. Let any indexing or cover generation finish.
3. Return to Home or Recent Books.
4. If the cover is still missing, delete the book cache and open the book again.

CrossInk generates covers from image references inside the EPUB itself. OPDS downloads, web uploads, and manually copied books all use the same local cover-generation path once the EPUB is saved to the SD card.

### Covers look stale after replacing a book file

**What you see:** You replaced an EPUB but still see the old cover or old thumbnail.

**Try this:**

1. Delete the book cache for that file.
2. Open the book again.
3. Return to the screen that showed the stale cover.

Moving or renaming a book can create a new cache location because CrossInk keys many caches from the file path.

## Reading Stats and Progress

### Progress changes after moving or renaming a book

**What you see:** A moved or renamed book opens like a new book, or its previous position is missing.

**Why it happens:** Per-book cache is tied to the book path. Moving a file can make CrossInk treat it like a different book.

**Try this:**

1. Move books as little as possible after you start reading them. Note: The `Move books to Read folder` feature automatically migrates the book's previous cache so this should be safe.
2. If you need to reorganize, do it before opening the book for the first time.
3. If the old `/epub_<hash>` folder still exists, you can copy the contents to the new folder.

### Reading stats look reset after changing firmware versions

**What you see:** All-time reading stats or completed-book counts look wrong after flashing another build, downgrading, or switching between experimental firmware versions.

**Try this:**

1. Avoid downgrading after using a newer build that changed reading-stats storage.
2. If you must downgrade, back up the SD card first.
3. Report the old version, new version, and whether you opened a book after downgrading.

Newer firmware can sometimes read older stats, but older firmware may not understand newer stats files. Opening a book on the older firmware can save over the newer stats format.

## Updates

### OTA update check fails

**What you see:** The device cannot check for updates or reports a network/TLS error.

**Try this:**

1. Confirm WiFi works by opening the web server from a browser.
2. Restart the reader and try again.
3. Make sure the network allows HTTPS traffic to GitHub.
4. If the device has just booted, wait briefly and retry.

### OTA downloads but does not install

**What you see:** The update appears to download, but validation fails or the device keeps the old firmware.

**Try this:**

1. Download the correct `firmware-*.bin` for your build variant from the release page.
2. Flash it manually using the web installer.
3. Report the build variant, current version, target version, and the exact error text.

If a file downloads fully but validation fails, that is different from a WiFi problem. The release asset or installed build variant may not match what the device expects.

## When to Report a Bug

Please include:

1. Device model, such as X4 or X3.
2. CrossInk version and build variant, such as `tiny`, `no_emoji`, or `xlarge`.
3. The exact steps that triggered the issue.
4. Whether the issue happens with one book or every book.
5. Whether the book came from OPDS, web upload, USB/SD copy, or another source.
6. Whether the book was optimized or not.
7. The visible error message.
8. Whether deleting the affected book cache changed anything.

If you can capture serial logs (use [https://www.serialmonitor.org/](https://www.serialmonitor.org/) with the `Baud Rate` set to `115200`) include the lines around the failure. For repeated EPUB indexing or cache rebuild issues, logs containing `EBP`, `CSS`, `SCT`, and `ERS` are especially useful.
