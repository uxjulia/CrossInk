---
title: EPUB Render Modes
nav_order: 18
---

# EPUB Render Modes

CrossInk normally tries to render EPUBs with the full CrossInk renderer. Some
EPUBs contain complex publisher styling, tables, or image rules that can use too
much memory for the reader. Render modes let you keep reading those books by
asking CrossInk to simplify the layout work for that book.

Render mode is saved per book. Changing it for one EPUB does not change the
default behavior for your other books.

## Which Mode Should I Use?

| Mode | Best for | What it keeps | What it simplifies |
| --- | --- | --- | --- |
| CrossInk Default | Most books | Full CrossInk styling, detailed CSS, table layout, image sizing, publisher spacing, Bionic Reading, Guide Dots | Nothing by default |
| Balanced | Books where CrossInk has fallen back automatically, or books you want to start in a lighter mode | Publisher spacing, image sizing, decorations, Bionic Reading, Guide Dots | Complex CSS lookups and table layout |
| Light | Books where Balanced is still too heavy, or books you want to start in the safest mode | Text content, hidden-content rules, basic formatting, Bionic Reading, Guide Dots | Complex CSS lookups, table layout, publisher spacing, image sizing, and decorative separators |

Most users can leave books on **CrossInk Default**. If a book runs out of memory
while building a section, CrossInk will automatically try **Balanced** and then
**Light**. Manual mode changes are mainly for choosing a lighter mode before
opening a known-problem book, or for changing the saved mode after CrossInk has
already fallen back.

## CrossInk Default

CrossInk Default is the normal renderer.

It tries to preserve the book's layout and publisher styling as much as
CrossInk supports, including:

- CSS rules that depend on surrounding elements
- Tables rendered as tables when possible
- Publisher margins, padding, and paragraph indents
- Publisher image sizing
- Horizontal rules and other visual separators
- Publisher page number markers, if enabled
- Bionic Reading and Guide Dots, if enabled

Use this unless a specific book is causing memory errors, very slow indexing, or
rendering problems.

## Balanced

Balanced keeps most publisher styling, but avoids the most expensive layout
work.

It keeps:

- Publisher margins, padding, and paragraph indents
- Publisher image sizing
- Horizontal rules and other visual separators
- Publisher page number markers, if enabled
- `display: none` hidden-content rules
- Bionic Reading and Guide Dots, if enabled

It simplifies:

- Complex CSS rules that depend on surrounding elements
- Tables, which are flattened into readable text blocks

Use Balanced when CrossInk has already fallen back to it, or when you want a
known-problem book to start with lighter rendering immediately. The page may not
match the publisher layout exactly, but it should still keep more of the book's
visual styling than Light.

## Light

Light is the safest mode for difficult EPUBs.

It keeps:

- Text content
- Basic bold, italic, underline, superscript, and subscript handling
- `display: none` hidden-content rules, so intentionally hidden content stays hidden
- Bionic Reading and Guide Dots, if enabled

It simplifies or removes:

- Complex CSS rules that depend on surrounding elements
- Tables, which are flattened into readable text blocks
- Publisher margins, padding, and paragraph indents
- Publisher image sizing
- Horizontal rules and decorative separators
- Publisher page number markers

Use Light when CrossInk has already fallen back to it, or when you want a
known-problem book to start in the safest mode immediately. Light may look less
like the publisher's original layout, but it gives CrossInk the best chance of
rendering the book on limited memory.

## Automatic Fallback

If a book starts in CrossInk Default and a section runs out of memory while
building, CrossInk automatically retries in this order:

1. CrossInk Default
2. Balanced
3. Light

If the book starts in Balanced, CrossInk can fall back to Light. If the book
starts in Light, CrossInk only tries Light.

When a fallback succeeds, CrossInk saves that successful mode for the book. The
next time you open the same book, it starts in the saved mode.

When a book opens in Balanced or Light, the reader briefly shows **Balanced
Mode** or **Light Mode** over the current page. The message disappears
automatically and does not change page layout, margins, or reading position.

## Changing A Book's Render Mode

You can change a book's render mode before opening it. This is optional because
CrossInk already falls back automatically, but it is useful when you know a book
is difficult and want it to start in a specific mode:

- Long-press an EPUB in **File Browser**
- Long-press an EPUB in **Recent Books**
- Choose **EPUB Render Mode**

You can also change it while reading:

1. Open the reader menu.
2. Choose **Book Options**.
3. Choose **EPUB Render Mode**.

Changing render mode rebuilds the affected book layout cache. Your reading
progress, bookmarks, clippings, reading stats, Bionic Reading setting, and Guide
Dots setting are preserved.
