---
title: EPUB Render Modes
nav_order: 9
---

# EPUB Render Modes

You almost never need to touch this. CrossInk shows each book with
all of its styling. If a book is too complicated for the reader's memory,
CrossInk automatically switches that book to a simpler look and remembers it. You
only need this page if a book will not open or is very slow, and you want to
pick a lighter mode yourself.

## What is a render mode?

EPUBs carry their own design: fonts, spacing, tables, decorations, and image
sizes. Handling all of that takes memory, and a few books are too heavy for an
e-reader. A render mode tells CrossInk how much of that design to keep. Lighter
modes drop some of the fancy parts so the book can still be read.

The mode is saved per book. Changing it for one book does not affect your other
books.

## Which mode should I use?

| Mode | Use it when | What you get |
| ---- | ----------- | ------------ |
| **CrossInk Default** | Almost always. | The book as the publisher designed it, as far as CrossInk supports. |
| **Balanced** | A book is slow or runs out of memory in the default mode. | Nearly the same look, but complicated style rules are simplified and tables become plain text blocks. |
| **Light** | Balanced is still too heavy. | Plain, readable text with basic bold and italic. Publisher spacing, image sizing, and decorations are dropped. |

There is also **Safe Mode**, which CrossInk uses automatically as a last resort.
It shows simpler text and turns off publisher styling, Focus Reading, and Guide
Dots for that book. It cannot be selected manually.

If a book runs out of memory while CrossInk is preparing a chapter, CrossInk
tries **Balanced**, then **Light**, then **Safe Mode** by itself. When one works,
it saves that mode for the book, so it opens the same way next time. You will see
a brief **Balanced Mode** or **Light Mode** message over the page. It goes away
on its own and does not change your layout or your place in the book.

## How to change a book's render mode

Usually you do not need to. But if you already know a book is difficult, you can
start it in a lighter mode.

Before opening the book:

1. Long-press the EPUB in **Browse Files** or **Library** (touch and hold on a
   touchscreen).
2. Choose **EPUB Render Mode**.
3. Pick a mode.

While reading:

1. Open the Reader Menu.
2. Choose the **Settings** tab.
3. Choose **EPUB Render Mode**.

Changing the mode rebuilds that book's saved layout, so give it a moment. Your
reading progress, bookmarks, clippings, reading stats, and your Focus Reading
and Guide Dots settings are kept.

## If a book still will not open

If even Safe Mode cannot open the book, long-press it in **Browse Files** or
**Library** and choose **Reset Book Reader Settings**. This clears only that
book's reader settings, including a saved Safe Mode or render mode. It keeps
your reading progress, bookmarks, clippings, and reading stats. Then try
[optimizing](https://inky.crossink.dev) the EPUB or choosing a different
[Indexing Method](./epub-indexing.md).
