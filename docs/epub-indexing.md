---
title: EPUB Indexing Methods
nav_order: 8.5
---

# EPUB Indexing Methods

Most people never need to change this setting. If a book sits on
an **Indexing** message for a very long time before the first page appears,
switch that book to **Incremental**.

## What is indexing?

Before CrossInk can show a chapter, it has to work out how the text fits onto
pages for your font, size, and margins. This is called indexing. CrossInk saves
the result on the SD card (in the book's cache), so a chapter only has to be
indexed once. Changing the font or spacing means it has to be done again.

**Indexing Method** decides when that work happens:

| Method       | What happens | Good for |
| ------------ | ------------ | -------- |
| **Full Section** (default) | The whole chapter is prepared before you see it. | Most books. After a short wait at the start of a chapter, page turns within that chapter need no further preparation. |
| **Incremental** | Only the pages you need right now are prepared. The rest is prepared in small steps while you read. | Books with enormous chapters, where waiting for a whole chapter is too long. |

## Which should I choose?

- Leave it on **Full Section** if books open at a comfortable speed.
- Switch to **Incremental** if a book pauses for a long time on **Indexing**
  before showing its first page. This is most common with omnibus editions and
  EPUBs that were never split into proper chapters.

With **Incremental**, you may occasionally see **Indexing** pop up if you jump
far ahead, follow a link deep into a chapter, or turn pages faster than
CrossInk can prepare them. That is normal. It is just catching up.

## How to change it

For all books: open **Settings > Reader > Indexing Method** and choose
**Incremental** or **Full Section**.

For one book only: open the Reader Menu, choose the **Settings** tab, then
**Indexing Method**. The choice is saved with that book and does not affect
your other books.

Changing the method does not change the screen you are looking at. It applies
the next time CrossInk needs to index a chapter. It also never changes the EPUB
file itself, your reading progress, bookmarks, clippings, or reading stats.

## If a book is still slow or will not open

Indexing Method changes when the work happens. It does not make the work
smaller. If a difficult book is still slow, or CrossInk runs out of memory on
it, try a lighter [EPUB Render Mode](./epub-render-modes.md), or
[optimize](https://inky.crossink.dev) the EPUB before copying it to the device.
