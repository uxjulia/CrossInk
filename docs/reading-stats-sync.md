---
title: Reading Stats Sync
nav_order: 7
---

# Reading Stats Sync

CrossInk can share reading progress, reading stats, and clippings through a
sync server. You can also share all-time reading totals directly between two
nearby CrossInk readers, without an account.

## Server Sync Settings

1. Open **Settings > System > Sync Server**.
2. Enter your **Username** and **Password**. Choose **Sign Up** if you need an
   account, or **Authenticate** to check an existing one.
3. Use the same server, account, and **Document Matching** choice on each reader.
4. Under **What to Sync**, turn on **Reading Stats** and **Clippings** if wanted.
   Both are off until you enable them.

The default is CrossPoint Sync (`https://sync.crosspointreader.com`). You can
change **Sync Server URL** to use another server. For setup choices, see the
[User Guide](./user-guide.md#367-sync-server-quick-setup).

Progress can sync with a KOReader-compatible server. Reading stats and clippings
need CrossPoint Sync support. If the settings show **Requires CrossPoint Sync**,
that server supports progress only. On a custom server, **Authenticate** checks
which features are available.

**Send Document Metadata** controls whether book details such as title, author,
and filename are shared with the server.

## Ways to Sync

| Action | Where to find it | What it shares |
| --- | --- | --- |
| **Sync Book** | Reader Menu, frontlight panel where available, or a book's menu in Library / Browse Files | That book's progress, plus enabled stats and clippings |
| **Sync Folder** | Touch and hold a folder in Browse Files, or highlight it and hold Confirm | Books in that folder and its subfolders |
| **Sync Books** | Home > File Transfer > Sync Server | Books across the SD card |
| **Sync Reading Stats** | File Transfer > Sync Server, or the upload control on Reading Stats > This Device | Overall totals and saved book stats |
| **Sync Clippings** | File Transfer > Sync Server | Saved clippings |

The last two actions appear when their **What to Sync** setting is on and the
server supports them. They do not move your reading position.

EPUB books can share progress, stats, and clippings. XTC books share stats only.
TXT and Markdown books are skipped. Turning tracking off keeps saved history
but stops new stats from being recorded.

These actions run when you start them. They do not turn on automatic syncing
when you close a book or put the reader to sleep. A separately installed plugin
may have its own background behavior; see [Plugins](./sd-plugins.md).

## Sync Books

Choose **File Transfer > Sync Server > Sync Books**, confirm, and connect to
Wi-Fi if asked. CrossInk checks the SD card directly, so you do not need to
refresh Library first.

The first sync checks every book. Later syncs can be quicker on supported
CrossPoint Sync servers because only changed books need to be checked or sent.
Other servers may check each book every time.

Press **Back** to stop. Finished transfers stay applied; a later sync continues
or checks again as needed. Retrying does not add the same reading time twice.

### Later syncs and saved positions

After a full sync, you may see **Checking for updates...** followed by
**Uploading changes...**. Unchanged books do not need to be sent again.

When another device has a new position, CrossInk can remember it for later. If
you open that book before resolving it, you may be asked **Continue from 45%
(Phone)?** Choose **Continue** to use that position, or **Stay here** to keep
this reader's place and send it on the next sync.

Turning Reading Stats or Clippings back on, or changing account or server, can
make the next sync take longer while it catches up.

## Progress and Stats Together

Choose **Sync Behavior** in the server settings:

- **Smart sync** keeps the position further through the book. Equal positions
  are left alone.
- **Ask on conflicts** asks what to do when positions differ. **Apply Remote**
  uses the server's position; **Upload Local** keeps and sends this reader's
  position. During a folder or whole-card sync, conflicts are collected for you
  to review at the end.

Position, stats, and clippings can succeed or fail separately. Check each result
on the final screen. If one fails, retry the sync. Missing stats are skipped
rather than replacing saved server history with zeros.

Keep **Document Matching** consistent across devices. **Filename** matches book
names; **Binary** matches file contents. An optimized or edited EPUB counts as
a different book with Binary matching. Changing this setting asks for
confirmation and makes the next sync check every book again. Existing server
records remain; you may need to merge duplicates in the server's web app.

Use a trusted sync service and an HTTPS address. Plain HTTP does not protect
your login or reading data in transit. CrossInk does not verify the server's
identity, so avoid syncing on untrusted networks.

## Catch Up a Folder

1. In **Browse Files**, touch and hold a folder, or highlight it and hold
   **Confirm** on button devices.
2. Choose **Sync Folder** and confirm.
3. Connect to Wi-Fi if asked, then wait while the folder and its subfolders
   are checked.
4. Review any conflicts and the final results.

Unread books with no saved position are skipped for progress. Saved stats and
clippings can still be sent when enabled. A failure for one book usually does
not stop the others; the final screen lists failed books and the reason.
Unreadable folders or an overly deep folder structure can stop the sync. Try a
smaller folder if that happens.

Press **Back** to stop, including from a conflict question. Work already
completed stays applied.

## Clippings Upload

With **Clippings** on, book and folder syncs send saved clippings. Use **Sync
Clippings** to upload clippings without changing reading positions.

Clippings are upload-only. CrossInk does not download clippings from the server
or remove a server clipping when you delete its local copy. Existing server
notes and colors are kept, and clippings deleted on the server are not restored
by a retry.

Older clippings may have no known creation date. They are uploaded without an
invented date. Identical quotes with no date in the same book may appear as one
clipping on the server.

## Sync a Selected Book

Touch and hold an EPUB or XTC in **Library** or **Browse Files**, or highlight it
and hold **Confirm**, then choose **Sync Book**. EPUB sync opens that book when
finished. XTC sync sends stats and returns Home.

If account settings open instead, finish setup there, then return to the book's
menu and try **Sync Book** again.

## Nearby Stats Sync

Nearby Stats Sync shares all-time totals directly between nearby CrossInk
readers. It does not need a Wi-Fi network, server, or account.

### What Gets Synced

- Reading sessions
- Reading time
- Pages turned
- Completed books

It does not copy books, per-book positions, bookmarks, clippings, or settings.
For your place in a book, use [Nearby Position Sync](./nearby-position-sync.md).

Each reader keeps its own totals and a saved copy of the other reader's totals.
The combined view uses those copies until you sync again. Other readers' totals
are not uploaded as reading done on this reader.

### How Nearby Sync Works

1. Turn reading stats tracking on for both readers.
2. On both, open **File Transfer > Nearby Device > Nearby Stats Sync**.
3. Keep the readers close together and press **Sync** on one reader only.
4. Wait for completion. Reading Stats now includes both readers' totals.

Repeat the sync after more reading to update the combined totals. Avoid manually
copying stats files between readers, since this can count the same history twice.

To stop showing nearby totals on one reader, back up the SD card, then remove
only `/.crosspoint/synced_stats/` using a computer. This keeps that reader's own
stats. Opening Nearby Stats Sync creates the folder again.

To reset this reader's own totals, use **Settings > System > Reading Stats >
All-time Stats > Reset All-time Stats**. Deleting books from the SD card does not
delete their history from the sync server.
