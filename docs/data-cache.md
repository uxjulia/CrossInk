---
title: Data Cache
nav_order: 16
---

# Data Cache

A cache is a saved copy of work the reader has already done, so it does not have to repeat it. When you open a book for the first time, CrossInk works out where every page break goes and saves the result on the SD card. The next time, the book opens much faster.

Everything is kept in a hidden folder called `.crosspoint` on the SD card. It holds the caches and also your personal data: settings, reading progress, bookmarks, clippings, reading stats, and saved Wi-Fi networks. CrossInk does not change the contents of your book files.

## Clearing Cache Data

Deleting the whole `.crosspoint` folder resets everything in it: caches, settings, saved Wi-Fi and server details, bookmarks, recently opened books, reading progress, and reading stats. Only do this if you want a completely fresh start.

If a book looks wrong after an update, or you want to free up space, you can clear just the book caches without touching your settings or all-time stats:

**Settings > System > Files & Cache > Clear Reading Cache**

Books are laid out again the next time you open them, so the first open is slower.

## Moving books

Each book's cache is tied to the book's location on the SD card. If you move or rename a book on a computer, CrossInk sees it as a new book and it may start with fresh reading progress. Moving a book inside CrossInk (for example, to the Read folder or with the File Browser move actions) keeps its cache and bookmarks.

Reader settings such as font, layout, and reading aids normally come from the global Reader settings. If you change a setting while reading a book, only that setting is saved for that book. Every other setting still follows your global choices. A book's render mode is also saved per book, so a troublesome book can be switched to Balanced or Light rendering from the File Browser or Library long-press menu before you open it.

Clippings (highlights) are saved separately from the book cache, in `/.crosspoint/clippings/`, so clearing the reading cache does not remove them. CrossInk also adds each clipping to a readable text file, `/My Clippings.txt`, in the top folder of the SD card. That file only ever grows. If you delete a clipping on the reader, it is removed from the reader but stays in `/My Clippings.txt` until you edit that file yourself.

Deleting a book inside CrossInk also removes its cache. If you delete books from a computer, their old cache folders stay behind until you run **Clear Reading Cache**.

Backups of your all-time reading stats are kept outside `.crosspoint`, in:

```text
/.crossink-stats-backup/
```

## Library

The Library screen lists the books on your SD card. See the [User Guide](./user-guide.md#34-library-screen) for how to search, sort, and use it. This section covers what it saves.

To make the Library fast, CrossInk keeps an index of your books (titles, authors, file locations, and sort order) in `.crosspoint`. It checks the index against the SD card on the first visit after the reader starts, after you change files in the File Browser, and after a download or file transfer. Other visits reuse the saved index. If you changed files from a computer while the reader stayed on or asleep, choose **Refresh Library** to scan the card again.

- If a Library refresh is interrupted, run **Refresh Library** again to finish updating the list.
- **Library Settings > Use Book Metadata** chooses between the title and author inside each EPUB and the filename. TXT, Markdown, and XTC files always use the filename.
- **Date Added** uses the time the file arrived on the card. Some copy tools keep the original file's date, which can change the order.
- **Recently Opened** shows only books in your reading history (up to 18), newest first. Marking a book unfinished does not add it to the history. Opening it does.

## Copying a card to another reader

Reading stats belong to the reader that recorded them. Copying an SD card or a
book's cache folder keeps those stats on the card, but another reader starts its
own local counters and does not upload the copied history as new reading. Moving
the card back lets the original reader resume its counters. Book files and saved
reading positions can still be copied as usual.

Before copying a card from older firmware, update the original reader and open a
book once so CrossInk can record where the existing stats came from. Files copied
before that step have no origin information; the first updated reader to use them
will claim them. This change prevents future duplicates and does not automatically
correct totals already duplicated on the sync server.
