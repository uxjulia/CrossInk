---
title: Languages
nav_order: 11
---

# Languages

CrossInk includes English. You can download other menu languages on the reader
or install them from your SD card. You can also edit a translation or create your
own.

Hyphenation packs help split long words at the end of a line. They follow the
book's language, independently of your menu language.

SD language files and hyphenation packs are new in CrossInk v2.0.0.

## Download a language on the reader

1. Open **Settings > System > Device > Download Languages**.
2. Select a language marked **Install**.
3. Follow the Wi-Fi screen to download and activate it. The reader restarts to
   Home; reopen your book to continue reading.

English is marked **Built in**. Other languages may be marked **Current** (in
use), **Installed** (already on the reader or SD card), or **Install**.

Selecting an installed language offers **Activate**, **Update**, and **Cancel**.
The current language offers **Update** and **Cancel**. **Update** downloads a
fresh copy. Cancelling the Wi-Fi screen returns Home without changing your
language or reading position.

After a firmware update, **What's New** offers **Update** when your active
language pack is older than the firmware. You can also open this screen through
**Settings > System > Release Notes**.

If you see **Downloads unavailable**, use the SD-card instructions below. If no
compatible download is available for your firmware, update the firmware or
install the language manually.

If you have edited the SD copy, downloading an update asks before replacing it.
Keep a separate copy of any translation you want to save.

## Install a language from SD

1. Download `crossink-languages-<version>.zip` for your firmware from the
   [releases page](https://github.com/uxjulia/CrossInk/releases).
2. Unzip it and copy the `.crossink/languages` folder to the root of your SD
   card. You only need the language files you want. Turn on **Show hidden files**
   in your file manager if you cannot see the `.crossink` folder.
3. On the reader, open **Settings > System > Device > Language** and select the
   language. The reader prepares it, saves your choice, and restarts to Home.

If you edit a language file later, select it again to apply your changes. Editing
or removing the file alone does not change the installed language. Keep the file
if you want to reapply or reinstall it again.

Good to know:

- Missing translations appear in English.
- A file with an error is rejected; your current language stays in use.
- Keep only one file for each language code. Two files with the same code cannot
  be selected until you remove the duplicate or give it a different code.
- After a firmware update, new or changed menu text may appear in English until
  you update or reapply your translation.
- Translations change text. They do not add fonts, keyboards, or support for new
  writing systems.
- Reapplying an unchanged translation may return without restarting.

## Hyphenation packs

English hyphenation is included. Packs are available for French, German,
Russian, Spanish, Italian, Polish, Portuguese, Swedish, and Ukrainian.

A book uses the pack for its own language, regardless of your menu language.
If a needed pack is missing, CrossInk offers once per reading session to open
**Hyphenation Packs**. You can cancel and keep reading.

### Download and install

1. Open **Settings > Reader > Page Layout > Hyphenation Packs**.
2. Select a pack marked **Install**. If its file is already on SD, the reader
   installs that copy. Otherwise, follow the Wi-Fi screen to download it.
3. After the reader restarts to Home, reopen your book.

Installed packs offer **Update**, **Delete**, and **Cancel**. **Update** downloads
and reinstalls the pack. If you edited its SD file, you are asked before it is
replaced. English is always available and cannot be removed.

### Install from SD

1. Copy the `.crossink/hyphenation` folder from the language ZIP to the root
   of your SD card.
2. Open **Settings > Reader > Page Layout > Hyphenation Packs**.
3. Select the pack marked **Install**. The reader installs it and restarts to
   Home; reopen your book afterward.

Deleting an installed pack also restarts the reader. Its SD file stays available
for reinstallation. Removing or replacing an SD file alone does not change the
installed pack. To replace a pack from SD without Wi-Fi, delete the installed
copy first, then select **Install** to install the replacement file.

Turn **Hyphenation** on under **Settings > Reader > Page Layout** to use automatic
hyphenation. Turning it off does not remove your packs. Hyphens already present
in a book's text still appear.

Books adjust automatically after a pack changes. You do not need to clear the
book cache, and your bookmarks and reading position are kept.

## Create or edit a translation

1. Open [Inky's Language Tools](https://inky.crossink.dev/#language-tools).
2. Choose a language to edit, or start a new translation.
3. Edit the menu text and download the language file. Keep any placeholders
   shown by the tool, since the reader fills these with names or numbers.
4. Copy the file into `.crossink/languages` on the SD card.
5. Choose it in **Settings > System > Device > Language** to apply it.

Select the language again after each edit. Keep a separate copy of your changes
before downloading an update. Missing translations appear in English.

<a id="add-missing-characters-with-ui-font"></a>

## Add missing characters with UI Fallback Font

On supported devices such as the X4 Classic, X4 Pro, and Sticky, **UI Fallback Font**
can fill in characters missing from the built-in font, such as Chinese characters
in menu text or book titles.
It also applies to filenames, dialogs, keyboards, chapter lists, bookmarks, and
other interface text. It keeps characters the built-in font already supports
and does not change your reading font. This option is unavailable on the X3 and
X4.

1. Copy a static `.ttf` or `.otf` font into a family folder, for example
   `/.crossink/languages/fonts/Noto Sans SC/`.
2. Include one regular face and, optionally, one bold face. Each file can be up
   to **32 MiB**. Italic faces, variable fonts, font collections, and bitmap
   `.cpfont` files are not supported here.
3. Open **Settings > System > Device > UI Fallback Font** and select the folder's name.
   Select **None** to turn this fallback off.

The picker also lists families it cannot load. Highlight a family to see the
reason, such as insufficient font memory, unsupported font data, or a folder
with no regular face or duplicate styles. Selecting it retries the load; if it
still fails, your previous choice is kept. Empty folders and folders containing
only bitmap fonts or macOS metadata files are not listed.

Some large fonts may still be too demanding to open. If the picker reports
insufficient font memory, try a smaller or prepared static font.

Keep fonts for book text in `/fonts` or `/.fonts`. A UI fallback font's first use can take
longer than later redraws. If a font is missing or unreadable, CrossInk uses its
built-in font while keeping your saved choice. After changing font files on a
computer, select the family again or restart the reader.

File Transfer and USB Drive temporarily use built-in fonts. Your selected UI
font returns when you leave those screens.

## SD cards and recovery

Some readers, including X4 Pro devices with their factory storage layout, keep
installed languages and hyphenation packs in `/.crosspoint/asset-cache.bin` on
SD. Keep this file on the card. You do not need to erase the reader to use language packs.

The original YAML and hyphenation source files can be removed after installation,
but a replacement SD card needs its own installed packs. If installed packs stop
loading, CrossInk falls back to English and built-in hyphenation.

To recover a damaged installed-pack cache, remove only
`/.crosspoint/asset-cache.bin`, then reinstall your language and hyphenation packs.
This resets both kinds of installed packs without removing your books.

Normal firmware updates preserve installed packs. A full device erase, switching
firmware, or replacing the SD card may require reinstalling them.
