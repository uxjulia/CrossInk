---
title: Dictionary
nav_order: 5
---

# Dictionary

> [!TIP]
> For the best experience, prepare your dictionary on a computer using CrossInk's web companion tool [Inky](https://inky.crossink.dev/#dictionary-tools). It prepares the files for faster lookups. A dictionary you have not prepared also works if its `.dict` and `.idx` files are uncompressed: the first lookup may take longer while the reader prepares it.

## Supported Format

The reader supports **StarDict** dictionaries. When searching for dictionaries online, look for "StarDict format" or files ending in `.dict`, `.idx`, and `.ifo`.

A dictionary folder typically contains:

- `.dict` -- definition data (required; compressed `.dict.dz` files must be prepared on a computer first)
- `.idx` -- word index (required)
- `.ifo` -- dictionary name and word count (recommended)
- `.syn` -- alternate forms and synonyms (optional, enhances lookup coverage)

Minimum requirement: one `.dict` and one `.idx` file in the same folder. Use matching names, such as `english.dict` and `english.idx`. Keep `.ifo` too when it is supplied so the dictionary name and formatting display correctly.

---

## Setting Up a Dictionary

Example folder structure: `SDCARD/.dictionaries/Cambridge/*.dict`. Make sure the dictionary folder is **NOT** nested like `/Cambridge/Cambridge/*.dict`

1. Copy your dictionary folder(s) to the SD card, inside `/.dictionaries/` or `/dictionaries/`. The reader uses only one of these: if both exist, `/.dictionaries/` is used and `/dictionaries/` is ignored.
2. If no dictionary has been selected before, the first dictionary in the alphabetical device list is selected automatically.
3. To choose between multiple dictionaries, open **Settings > Reader > Dictionary** on the device and select one from the list.
4. To use a different dictionary for one book, open the Reader Menu, go to the Settings tab (gear icon), and choose **Book Dictionary**. See [Per-Book Dictionary](#per-book-dictionary).

### Preparing Compressed or Large Dictionaries

If your download contains `.dict.dz` or `.syn.dz` files, prepare it with
[Inky](https://inky.crossink.dev/#dictionary-tools) before copying it to the SD
card. Preparation is also recommended for large dictionaries to make lookups
faster.

Uncompressed dictionaries can be used without preparation. The first lookup
may take longer; later lookups should be quicker. If you cancel that first
lookup, the reader can try preparing it again next time.

---

## Looking Up a Word

The **Look Up Word** option in the reader menu is only visible when a dictionary is active.

### Buttons

1. Open the reader menu and choose **Look Up Word**.
2. The page becomes a word-select overlay - one word is highlighted, initially near the centre of the page.
3. Use **Up/Down** to move between rows, **Left/Right** to move between words on the same row.
4. Press **Confirm** to look up the highlighted word.
5. Press **Back** to exit word-select without looking anything up.

### Touchscreen

In an EPUB reader with **Touch Reader Controls** enabled, touch and hold a
word for about one second. CrossInk opens word selection on that word; lift
your finger to show the floating **Look Up** and **Clip** toolbar.
Drag either selection handle to adjust the highlighted range. To select a phrase,
keep holding after selection opens, drag to the last word, then lift your
finger. Hold on the page's final word for about one second to continue onto
the next page. You do not need to open the reader menu first. **Clip**
is available without a dictionary; **Look Up** is disabled until one is active.
The toolbar sits near the selection and hides while you drag. Press **Back** to clear the range,
or tap outside the selection and toolbar to return to reading.

From a definition, touch and hold a word for about one second to start another
lookup. Keep holding and drag to select a phrase, then lift your finger to look
it up. Once word selection is open, touching a word starts the range
immediately.

### Quick Lookup

Set a shortcut to **Look Up Word** in **Settings > Controls**. Holding that shortcut in the EPUB reader then enters word-select directly, skipping the reader menu. Release and navigate to a word as usual.

### How Lookup Works

When you select a word, the reader searches for it in this order:

1. **Direct match** - the cleaned word is searched first, preserving pointed headwords, capitalization preference, and existing NFC normalization. If it is absent, Hebrew niqqud and cantillation marks and basic Arabic vowel marks (U+064B–U+0650, U+0652, U+0670) are removed from the query and it is retried. Arabic shaddah, hamza, and maddah remain significant. Exact pointed spellings win when both pointed and unpointed headwords exist. Dictionary indexes and their sort order are unchanged; this fallback does not merge differently pointed headwords.
2. **Stemming** - the reader automatically tries common word forms (plurals, verb conjugations, comparatives). For example, "running" finds "run".
3. **Alternate forms** - if the dictionary includes a synonym/alternate forms file and no match was found yet, a prompt appears. Press **Confirm** to search alternate forms, or **Back** to skip. Alternate forms also try the supplied spelling before the vowel-stripped fallback.
4. **Suggestions** - if nothing matched, a list of similar words from the dictionary is shown. Select one to view its definition.
5. **Not found** - if no matches or suggestions exist, a not-found message appears. Press **Back** to return to word-select, or **Confirm** to exit to the reader.

---

## The Definition Screen

When a word is found, the definition screen shows the headword at the top and the definition text below.

- **Page Forward** - next page (for long definitions)
- **Page Back** - previous page
- **Confirm** (labelled **Look Up Word**) - enter word-select mode on the definition text (see Chaining Lookups below)
- **Left** (labelled **Switch**) - choose another dictionary and repeat the current lookup
- **Back** (short press) - return to the previous screen
- **Back** (long press) - exit all the way back to the reader

---

## Chaining Lookups

From a definition screen, you can look up any word within the definition text without returning to the reader.

1. Press **Confirm** (**Look Up Word**) on the definition screen. On a touchscreen, touch and hold the word you want to look up instead.
2. A word in the definition becomes highlighted. Navigate to any word and press **Confirm**, or lift your finger after the touchscreen selection starts.
3. A new definition screen opens for that word.
4. You can chain further by pressing **Look Up Word** again from the new definition.
5. Short-press **Back** to exit word-select and return to the current definition.
6. Short-press **Back** again to go back through the chain (each press returns to the previous definition).
7. Long-press **Back** at any point to exit directly to the reader.

Going back returns you to each prior definition **on the page you were reading** when you chained away from it, not the first page.

**Chain depth limit:** the chain follows the 50 entries visible in lookup history. If you chain deeper than that, the oldest chain entries are dropped, so backing out eventually returns you to the reader.

---

## Phrase / Multi-word Lookup

In word-select mode, you can select a sequence of words to look up as a phrase.

1. Navigate to the first word of the phrase.
2. Long-press **Confirm** to anchor on that word.
3. Use the navigation buttons to extend (or shrink) the selection to cover the full phrase. All selected words are highlighted.
4. Short-press **Confirm** to look up the selected phrase.
5. Press **Back** to cancel and return to single-word select mode.

Multi-word select works in both the reader word-select and the definition word-select (chained lookup).

On a touchscreen, use a press-and-drag selection instead of the button
sequence: touch the first word, drag to the last word, and lift your finger.
In the EPUB reader, adjust the handles if needed and choose **Look Up** from
the floating toolbar. Inside a definition, lifting your finger starts the
lookup immediately. The first long hold on the reading page is only needed
to enter selection; once word selection is open, touching a word starts the
range immediately.

Touch selection in the EPUB reader can continue onto the next page in the
same chapter: drag to the final word and hold there for about one second.
Button selection and selection inside definitions stay on the current page.

---

## Per-Book Dictionary

Each book can have its own dictionary, independent of the global setting.

1. Open the reader menu, navigate to the 3rd tab (settings gear icon) and choose **Book Dictionary**.
2. Select a dictionary from the picker.
3. The per-book choice is saved and restored each time the book is opened.
4. To remove the override, open **Book Dictionary** and select **Use Global**. The picker shows the current global dictionary name in parentheses next to Use Global.

The global dictionary is automatically restored whenever the book is closed. Changing a book's dictionary does not affect the global setting or other books.

Using **Switch Dictionary** during a word lookup is temporary. It applies only to that lookup flow; when lookup closes, the reader returns to the book's dictionary override, or the global dictionary when the book has no override.

---

## Lookup History

Each book maintains its own lookup history, accessible from the reader menu.

1. Open the reader menu and choose **Lookup History**.
2. Each entry shows the searched word and a status indicator:
   - Square root (√) -- found directly
   - Tilde (~) -- resolved via stemming or alternate forms
   - Question mark (?) -- reached via the suggestions screen
   - X -- not found
3. Select any entry and press **Confirm** to look it up again. Press **Back** from the definition to return to the history screen.
4. To delete an entry, long-press **Confirm** on it. A confirmation popup appears -- press **Confirm** to delete, or **Back** to cancel.

The history screen shows the 50 most recent entries. You can go back through up to 50 linked lookups.

---

## IPA Phonetic Characters

Dictionary definitions use the active reader font and size by default. If at least one dictionary is installed, you can set the default dictionary font and size in **Settings > Reader > Font Options**. Books inherit those defaults unless you choose a different font or size in the reader menu's **Dictionary Font** option. Choose **Use Global** there to return a book to the global dictionary font
and size defaults. When the reader uses an SD-card font, you can choose a different dictionary size while keeping that same family; built-in reader fonts continue to use their active size. A saved size with no matching file uses the closest available size from the dictionary family.

The dictionary font is used while a definition is open. Closing it returns to your reading font. If a book's selected dictionary font is missing, the definition temporarily uses the global dictionary font. If that is also unavailable, it falls back to the reader font. The per-book selection is kept so it resumes automatically if you reinstall the family.

Built-in fonts can show some pronunciation symbols only approximately. If you see a filled diamond, choose an SD-card font that includes that character.

You can download CrossInk's SD card catalog of fonts with pronunciation symbols included from [Inky](https://inky.crossink.dev/#downloads).

---

## Dictionary Tools

### Inky

CrossInk's companion app, [Inky](https://inky.crossink.dev/#dictionary-tools), prepares one StarDict dictionary at a time and prepares its files for fast lookups. It accepts either:

- an uncompressed dictionary folder; or
- a `.zip`, `.tar.zst`, or `.rar` archive containing that folder or its dictionary files.

1. Open Inky's **Dictionary Tools** page.
2. Drag the dictionary archive or folder onto the upload area. Alternatively, click it and choose **Archive** or **Folder**.
3. Select **Prepare Dictionary**. The input must include matching `.ifo`, `.idx`, and `.dict` or `.dict.dz` files. `.syn` and `.syn.dz` are optional.
4. Download the prepared ZIP when the job finishes, then unzip it into `/.dictionaries/` or `/dictionaries/` on the SD card. Keep the resulting dictionary folder intact.

Keep all files from the prepared ZIP together. The prepared download is available
for 10 minutes.

## Where to find dictionaries

> credit to https://github.com/koreader/koreader/wiki/Dictionary-support for the list.

- The [reader.dict](https://www.reader-dict.com) (ex "BoboTiG/ebook-reader-dict") project provides StarDict version of daily dumps of [Wiktionary](https://www.wiktionary.org/) monolingual dictionaries for a variety of languages. It also provides [non-free multilingual](https://www.reader-dict.com) dictionaries.
- The [WikDict](https://www.wikdict.com) project provides free bilingual dictionaries based on [Wiktionary](https://www.wiktionary.org/) for a lot of language pairs. StarDict versions can be [downloaded from here](https://download.wikdict.com/dictionaries/stardict/).
- The [`Vuizur/Wiktionary-Dictionaries`](https://github.com/Vuizur/Wiktionary-Dictionaries) repository contains dictionaries based on [Wiktionary](https://www.wiktionary.org/) from many languages to English, including English-English.
- The [DictInfo](https://www.dictinfo.com/) website provides outdated monolingual dictionaries based on [Wiktionary](https://www.wiktionary.org/).
- The [Firedict site](https://tuxor1337.frama.io/firedict/dictionaries.html) contains a list of freely available dictionaries.
- [wiktionary_stardict](https://xxyzz.github.io/wiktionary_stardict/): update monthly.
- [Fictionaries](https://fictionary.gumroad.com/) provides dictionaries for various speculative fiction books and series.
- [World Factbooks Archive](https://github.com/MilkMp/CIA-World-Factbooks-Archive-1990-2025) provides 36 years of CIA's World Factbook dictionaries in StarDict format.
- [StarDict-Hebrew](https://github.com/Uri-Tauber/StarDict-Hebrew) Hebrew-English StarDict versions of Babylon dictionaries.
