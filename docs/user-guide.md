---
title: User Guide
nav_order: 1.5
---

# CrossInk User Guide

This guide covers day-to-day device use. Your books, settings, and reading progress are stored on the SD card, the small memory card inside the device. For focused reference material, see [Reader Features](./reader-features.md), [Controls](./controls.md), [SD Card Fonts](./sd-card-fonts.md), [File Transfer](./webserver.md), and [Troubleshooting](./troubleshooting.md). For Dashboard and Minimal Home-screen gestures, see [Touch Navigation](./touch-navigation.md).

- [CrossInk User Guide](#crossink-user-guide)
  - [1. Hardware Overview](#1-hardware-overview)
    - [Button Layout](#button-layout)
    - [Taking a Screenshot](#taking-a-screenshot)
  - [2. Power \& Startup](#2-power--startup)
    - [Power On / Off](#power-on--off)
    - [First Launch](#first-launch)
  - [3. Screens](#3-screens)
    - [3.1 Home Screen](#31-home-screen)
    - [3.2 Reading Mode](#32-reading-mode)
    - [3.3 Browse Files Screen](#33-browse-files-screen)
    - [3.4 Library Screen](#34-library-screen)
    - [3.5 File Transfer Screen](#35-file-transfer-screen)
    - [3.5.1 Calibre Wireless Transfers](#351-calibre-wireless-transfers)
    - [3.6 Settings](#36-settings)
      - [3.6.1 Display](#361-display)
      - [3.6.2 Reader](#362-reader)
      - [3.6.3 Controls](#363-controls)
      - [3.6.4 System](#364-system)
      - [3.6.5 OPDS Servers (Multiple Libraries)](#365-opds-servers-multiple-libraries)
      - [3.6.6 Web Settings (Wi-Fi + OPDS)](#366-web-settings-wi-fi--opds)
      - [3.6.7 Sync Server Quick Setup](#367-sync-server-quick-setup)
        - [Option A: CrossPoint Sync Server (`sync.crosspointreader.com`, default)](#option-a-crosspoint-sync-server-synccrosspointreadercom-default)
        - [Option B (advanced): Legacy Public KOReader Server (`sync.koreader.rocks`)](#option-b-advanced-legacy-public-koreader-server-synckoreaderrocks)
        - [Option C (advanced): Self-Hosted Server (Docker Compose)](#option-c-advanced-self-hosted-server-docker-compose)
    - [3.7 Sleep Screen](#37-sleep-screen)
      - [Cover settings](#cover-settings)
      - [Custom images](#custom-images)
    - [3.8 Boot Screen](#38-boot-screen)
    - [3.9 Custom Fonts (SD Card)](#39-custom-fonts-sd-card)
  - [4. Reading Mode](#4-reading-mode)
    - [Page Turning](#page-turning)
    - [Chapter Navigation](#chapter-navigation)
    - [Auto Page Turn](#auto-page-turn)
    - [Tilt Page Turn (X3 and Sticky)](#tilt-page-turn-x3-and-sticky)
    - [Touch Reader Controls](#touch-reader-controls)
    - [Slider Controls](#slider-controls)
    - [Footnote Navigation](#footnote-navigation)
    - [System Navigation](#system-navigation)
    - [Supported Languages](#supported-languages)
  - [5. Reader Menu](#5-reader-menu)
    - [5.1 Chapter Selection](#51-chapter-selection)
    - [5.2 Bookmarks](#52-bookmarks)
    - [5.3 Dictionary](#53-dictionary)
  - [6. Current Limitations & Roadmap](#6-current-limitations--roadmap)
  - [7. Troubleshooting Issues & Escaping Bootloop](#7-troubleshooting-issues--escaping-bootloop)
    - [Advanced: Serial Monitor Logs](#advanced-serial-monitor-logs)

## 1. Hardware Overview

### Button Layout

CrossInk supports many devices with various button layouts. For the X3/X4 devices with front buttons, by default they will use the following layout from left to right, and this guide will refer to them by those names.

<table>
  <thead>
    <tr>
      <th colspan="4"><center>Default Front Button Mapping</center></th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>Back</td>
      <td>Confirm</td>
      <td>Left</td>
      <td>Right</td>
    </tr>
  </tbody>
</table>

For devices with buttons on either side of the device, this guide may refer to the buttons as follows:

| Location       | Button Mapping     |
| -------------- | ------------------ |
| **Left Side**  | **Left**/**Up**    |
| **Right Side** | **Right**/**Down** |

Button layout can be customized in **Settings > Controls**.

### Taking a Screenshot

Press the **Power** and **Down** buttons at the same time to take a screenshot. It is saved as a `.bmp` image in the `screenshots` folder on the SD card.

You can also open the Reader Menu while reading, go to the **Bookmarks** tab, and choose **Take Screenshot**, or assign **Screenshot** to a Power button action.

---

## 2. Power & Startup

### Power On / Off

By default, **press and hold the Power button for about half a second** to put the device to sleep or wake it. In **Settings > Controls > Power Button** you can set the short-press and long-press actions separately. A short press wakes the device only when the short-press action is **Sleep/Wake** or **Wake**; otherwise, hold Power for about half a second to wake it. See [Controls](#363-controls) for details.

If the device freezes, or after a firmware update, restart it: press and release the **Reset** button, then quickly press and hold **Power** for a few seconds. If the device keeps restarting, see [Escaping a bootloop](#7-troubleshooting-issues--escaping-bootloop).

### First Launch

Upon turning the device on for the first time, you will be placed on the **[Home](#31-home-screen)** screen.

> [!NOTE]
> On subsequent restarts, the firmware will automatically reopen the last book you were reading.

---

## 3. Screens

### 3.1 Home Screen

The Home screen is the main menu. It shows the book you are reading and these entries: **[Browse Files](#33-browse-files-screen)**, **[Library](#34-library-screen)**, **OPDS Browser** (once you have added an OPDS server), **Reading Stats** and **Bookmarks**/**Clippings** (when there is something to show), **[File Transfer](#35-file-transfer-screen)**, and **[Settings](#36-settings)**. The layout depends on your **UI Theme**. To start reading, see **[Reading Mode](#4-reading-mode)**.

When Plugins is enabled in Settings → System, open **Plugins** from Home. In
its installed-plugin picker, hold **Confirm** for one second or long-press a
plugin row on a touch device. Choose **Uninstall** to remove that installed
plugin folder and all of its files, or **Cancel** to keep it.

### 3.2 Reading Mode

See [Reading Mode](#4-reading-mode) below for more information.

### 3.3 Browse Files Screen

The Browse Files screen lets you look through the folders on the SD card. The full path to the current folder is shown at the top of the screen. File extensions are shown next to each filename, and folders are shown in brackets (for example `[folder-name]`). Folders and files that start with a dot are hidden; to show them, turn on **Settings > System > Files & Cache > Show Hidden Files**.

- **Navigate List:** Use **Left** (or **Up**), or **Right** (or **Down**) to move the selection cursor up and down through folders and books. You can also long-press these buttons to scroll a full page up or down.
- **Open Selection:** Press **Confirm** to open a folder or start reading a selected book. Selecting a `.bmp` file will open the image viewer.
- **Delete Files or Folders:** Hold and release **Confirm** to open the selected file or folder action menu, then choose **Delete**. You will be given an option to either confirm or cancel. Folder deletion is limited to empty folders.
- **Book Actions:** EPUB and XTC files can also show options such as **Delete Cache**, **Mark Finished**, and **Track Reading Stats** from the same action menu. Turning tracking off for a book keeps its saved stats; it only stops new tracking for that book. Other actions in the menu include renaming and, for books, sending to another nearby reader.

### 3.4 Library Screen

**Library** lists the supported books across the whole SD card.
The first visit reads the card to build the list; later visits reuse it and
refresh after file changes or transfers.

- **Find a book:** Use **Search** to filter by title or author. With **Use Book
  Metadata** off, books are shown and searched by filename instead.
- **Sort:** Choose **Date Added**, **Title**, **Author (Last Name)**, **Author
  (First Name)**, **Recently Opened**, **Series**, or **Genre**. You can reverse
  the sort order. **Recently Opened** shows only your saved reading history,
  rather than every book on the card.
- **Buttons:** Use **Up/Down** to select a book and **Confirm** to open it. Hold
  **Confirm** for book actions. **Left** opens Sort; **Right** opens the menu
  containing **Search**, **Settings**, and **Refresh Library**.
- **Touch:** Tap a book to open it, hold it for book actions, and swipe to scroll.
  Use the search, settings, refresh, and sort controls at the top.
- **Library Settings:** Open Library's **Settings** to choose embedded EPUB
  metadata or filenames, compact or expanded rows, and a list or cover grid
  for **Recently Opened View**. You can also hide finished books, show series
  and genre, and filter which book formats appear.
- **Book actions:** EPUB and XTC books can offer **Reading Stats** without
  opening the book, when tracking is enabled for the device and that book.
  The action menu also includes book-specific controls such as **Mark Finished**
  and **Track Reading Stats** where supported.

If files were changed externally while the reader stayed on, use **Refresh
Library** to scan the card again. **Date Added** uses the file's creation time
when available, so a copy tool that preserves old timestamps may affect its
order.

### 3.5 File Transfer Screen

The File Transfer screen allows you to upload and manage files on the device.
Choose **Join a Network**, **Calibre Wireless**, or **Create Hotspot** to start
the web server for the selected mode.

See the [File Transfer guide](./webserver.md) for connection and upload details.

The web file manager can upload, download, rename, move, and delete files on the device.

The web interface also supports **WebDAV**, allowing you to mount the device as a network drive and manage files directly from your computer's file manager.

Download links for files already on the device are available in the web interface, so you can retrieve books or screenshots over Wi-Fi without connecting a cable.

A **Wi-Fi signal strength indicator** (dBm) is displayed on-screen during joined-network web server sessions.

The **Sync Server** section syncs with your sync server account (set up in
**Settings > System > Sync Server**): **Sync Books** syncs progress, reading
stats, and clippings for every book, while **Sync Reading Stats** and **Sync
Clippings** upload only that data. The last two appear when that data is turned
on under **What to Sync**. See [Reading Stats Sync](./reading-stats-sync.md#ways-to-sync).

Below it, the **Nearby Device** section has **Receive File**, which receives a supported
book or image directly from another nearby CrossInk reader without joining a
Wi-Fi network. See [Nearby File Transfer](./nearby-file-transfer.md) for the
complete sender and receiver workflow.

On X4 Pro, the screen also includes **USB Drive**. This exposes the reader's SD
card to a computer over USB for direct file management. See the
[USB Drive instructions](./installation.md#usb-drive) for the safe-eject and
disconnect behavior.

> [!TIP]
> Advanced users can manage files programmatically with the same HTTP endpoints
> used by the web interface. The browser interface is the supported path for
> normal file management.

### 3.5.1 Calibre Wireless Transfers

CrossInk supports sending books from Calibre using the CrossPoint Reader device plugin.

1. Download the current `crosspoint_reader` plugin ZIP from the
   [CrossPoint Reader plugin releases](https://github.com/crosspoint-reader/calibre-plugins/releases).
2. In Calibre, open **Preferences > Plugins > Load plugin from file** and select
   that ZIP. Restart Calibre if it asks you to.
3. On the device, open **File Transfer > Calibre Wireless** and join the same
   2.4 GHz Wi-Fi network as the computer.
4. Keep the Calibre Wireless screen open, then use Calibre's **Send to device**
   action. The device screen shows the transfer progress and completion notice.

### 3.6 Settings

The Settings screen groups options by purpose. The exact choices can vary by
device model and build.

#### 3.6.1 Display

- **Sleep Screen**: Opens a submenu with **Wallpaper** (the sleep screen mode), **Cover Mode**, **Cover Filter**, and **Quick Resume on Timeout**. Every mode and setting is explained in [Sleep Screen](#37-sleep-screen).

- **Frontlight** (devices with a frontlight): Opens **Restore Light on Wake** and, on devices with a real-time clock, a daily **Frontlight Schedule**.
  - **Restore Light on Wake**: When on, a frontlight that was on before sleep turns back on when the device wakes. If the light was off before sleep, a complete schedule can still decide whether it turns on.
  - **Frontlight Schedule**: Turn on the daily schedule, then set **Start** and **End** in local time, in one-minute steps. The schedule includes the Start time and excludes the End time, and it can cross midnight (for example, 9:00 PM to 7:00 AM). Start and End must both be set and different; otherwise the schedule does nothing.
  - The schedule is checked when the device starts or wakes, not while it is already awake. **Restore Light on Wake** takes precedence when the light was on before sleep. Set the device's time and UTC offset under **Settings > System > Device** so the schedule uses the right clock. An unset time shows as `--`; saved times are kept for when you turn the schedule back on.

- **Status Bar**: Choose what the bar at the top of menus and the Library shows. It has a left, center, and right slot. Clock and Date need a device with a real-time clock. **Battery** sets how the battery appears: "Icon + %" (default), "Icon Only", or "% Only", and **Status Bar Text Size** sets the text size. Reading bars are set separately under **Status Bars** in the Reader settings.

- **Screen Calibration**: If the edge of the screen looks cut off by the bezel, adjust each edge until the border is fully visible. **Reset to Defaults** undoes your changes.

- **Refresh Frequency**: How often the screen does a full refresh while reading, to reduce ghosting (faint leftovers of the previous page). Choose every 1, 5, 10, 15, or 30 pages. Devices with a frontlight also offer **Never**.

- **Dark Mode**: Show the screen with inverted colors (light text on a dark background).

- **UI Theme**: Set the look of the Home screen and menus (the default is "Lyra"):
  - "Classic" - The original CrossInk theme (button devices only)
  - "Minimal" - A minimal theme with a large book cover
  - "Dashboard" - A dashboard-style home layout
  - "Lyra" - A theme with simple icons featuring your current book
  - "Lyra Extended" - Lyra, but displays 3 books instead of 1 on the **[Home Screen](#31-home-screen)**
  - "Lyra Carousel" - A carousel-based Lyra home layout
  - "RoundedRaff" - A rounded theme with additional visual styling (button devices only)
  - "Cover Grid" (PSRAM devices, such as Sticky and X4 Pro) - Shows the current book and up to six additional book covers; tap a cover to open that book

  Themes that show reading stats hide those stats while **Track Reading Stats** is off.

- **Swap Library/File Browser** (Minimal and Dashboard themes): Swaps the places of **Library** and **Browse Files** in these themes.

- **UI Scale**: **Small** or **Large** menus and text.

- **Sunlight Fading Fix** (button devices): A software fix for white X4 models that may fade in direct sunlight. "OFF" (default) or "ON".

Library display and filtering options are in **Library > Settings**; see [Library Screen](#34-library-screen).

> [!NOTE]
> A battery charging indicator is shown on the battery icon whenever the device is actively charging.

#### 3.6.2 Reader

**Global settings and individual books:** Choices under **Settings > Reader**
are the defaults for your EPUB books. Layout choices such as font family, font
size, and margins can be saved for one book from the in-book **Font** and
**Layout** tabs. A saved book choice takes precedence over the matching global
setting; choices you have not changed for that book still follow the global defaults.
For example, a book with its own font size can still pick up a later change to
your global margins. If you open global settings through the pull-down panel
on a touch reader, inherited font and layout changes also apply to the open
book when you return. **Dark Mode** and **Stable Page Numbers** are global
settings.

If a global change does not affect one EPUB, open its Reader Menu **Settings**
tab and choose **Reset Book Reader Settings**, or use the book action menu in
**Browse Files** or **Library**. This removes that book's saved reader choices
so it follows the current global defaults again; it keeps reading progress,
bookmarks, clippings, and stats, and does not reset your other books or global
settings. Older saved books may need this reset before they can inherit
individual global changes. **Delete Book Cache** does not reset these choices.

- **Font Options**: Opens a submenu with the font and text spacing choices:
  - **Font Family**: The font used for reading. "Lexend Deca" (default) and "Bitter" are built in; fonts you add to the SD card appear here too (see [Custom Fonts](#39-custom-fonts-sd-card)).
  - **Font Size**: The sizes depend on your device and font. X3/X4 devices offer 10, 12, 14, and 16 pt. ESP32-S3 readers (Sticky and X4 Pro) use scalable fonts with every whole point size from **8-22 pt**; see [Scalable Fonts](./scalable-fonts.md). The default is 14 pt.
  - **TTF Rendering** (scalable-font devices): Fine-tune how TrueType fonts are drawn.
  - **Dictionary Font** and **Dictionary Font Size** (when a dictionary is installed): The font used in word lookups. The default is to use the reader font.
  - **Line Spacing**: The gap between lines, as a percentage from 70% to 200% (default 100%).
  - **Word Spacing**: In EPUB books, a slider from -4 (tighter) to +4 (wider) with a live preview; 0 is normal. You can also reach it in the Reader Menu under **Font > Line/Word Spacing**. Changing it reflows the current book, so page positions may change.
  - **Character Spacing**: A slider from -5 (tighter) to +5 (wider) with a live preview; 0 is normal. It is also in the Reader Menu under **Font > Reader Font**.
  - **Text Anti-Aliasing**: Smooths the edges of text using shades of gray.
  - **Download Fonts**: Download font families over Wi-Fi. **Download Font Size Range** sets which sizes are downloaded to save space (8-12pt, 10-16pt, 14-20pt, or 8-20pt).

- **Page Layout**: Opens a submenu with these choices:
  - **Orientation**: How the screen is turned while reading: "Portrait" (default), "Landscape CW", "Portrait 180°", or "Landscape CCW".
  - **Screen Margins**: Opens **Top/Bottom** and **Left/Right** sliders. Each goes from 5 to 150 pixels with a live preview (default 5).
  - **Paragraph Alignment**: "Justify" (default), "Left", "Center", "Right", or "Book's Style".
  - **Hyphenation**: Whether long words are split with a hyphen at the end of a line. The default is OFF. Korean text wraps at spaces when OFF; when ON, a Korean word may also split at a line end without a hyphen. **Hyphenation Packs** manages the per-language rules; see [SD language files](./languages.md).
  - **Extra Paragraph Spacing**: ON (default) adds vertical space between paragraphs. OFF removes it and shows the book's first-line indentation when it has one.
  - **Force Paragraph Indents**: Indents every paragraph, whatever the book's own formatting; see [Reader Features](./reader-features.md#force-paragraph-indents).

- **Publisher Page Numbers**: Show page numbers supplied by the EPUB when the book includes them. This reserves 5 pixels of left margin for the numbers, which is only noticeable when your left/right margin is 5. If a page has no publisher page number, your margins may look uneven.

- **Embedded Style**, **Images**, **Image Grayscale**, **Focus Reading**, **Guide Dots**, **Dictionary**, and **Indexing Method** are also in the Reader settings. See [Reader Features](./reader-features.md) for what they do, including the [Focus Reading](./reader-features.md#focus-reading) guide.

- **Disable Touchscreen** (touch devices): Block screen-touch input while a book is open, while leaving touch available in reader menus so you can turn it back on. To disable only touch page turns, use **Next Page** and **Previous Page** in **Settings > Controls > Taps & Gestures**.

- **Status Bars**: Set up the bars shown while reading. Open **Settings > Reader > Status Bars**, or the Reader Menu's **Settings** tab > **Status Bars**. A preview shows the result at its reading position.
  - **Top Status Bar** and **Bottom Status Bar** (EPUB and TXT): Each has three left slots, one center slot, and three right slots. Fill a slot with any of: Clock and Date (devices with a clock), Battery, Time Left (book or chapter), Chapter Page Count (for example 5/25; it changes with font size and margins), Stable Page Numbers (needs CrossInk reference metadata in the EPUB; see [Stable Page Numbers](./reader-features.md#stable-page-numbers)), Book Progress Percentage, Title (book or chapter), or Empty.
  - **Battery**: Show the battery as "Icon + %" (default), "Icon Only", or "% Only".
  - **Percentage Format**: Show book progress as `10%` (default), `10.1%`, or `10.12%`.
  - **Progress Bar**: Hide it, or show a progress bar for the **Book** or the **Chapter**. **Progress Bar Thickness** can be Thin, Medium, or Thick.
  - **Hide**: Turn a whole bar off.
  - **XTC Status Bar**: For XTC books, choose **Hide**, **Bottom**, **Top**, or **Both**.
  - **Status Bar Text Size**: **Small**, **Medium**, or **Large** for all bars.

On touchscreen readers, when **Tap to Hide Status Bar** is enabled (the default
in **Settings > Controls > Taps & Gestures**), tap the status-bar area while
reading to show or hide the entire status bar for the current reading session.
This tap is available while reader touch input is enabled. The quick
toggle does not change the page layout or page breaks; tap the same status-bar
region again to restore a hidden bar. Use **Status Bars** to choose
which status-bar items are shown.

#### 3.6.3 Controls

Which entries you see depends on your device. Button devices show **Front Buttons** and **Menu navigation**; touch devices show **Taps & Gestures**; X4 Pro also has **Home Button**; devices with a motion sensor show **Tilt Page Turn** and **Tilt Direction**. For the full reference, see [Controls](./controls.md).

- **Power Button**: Set what a short press and a long press (about 0.4 seconds) of the Power button do. On supported devices you can also set the **Power + Up Combo** shortcut. Available actions include:
  - "Ignore" (default short-press action) - Do nothing for this press length
  - "Sleep/Wake" (default long-press action) - Put the awake device to sleep; when selected as the short-press action, a short press also wakes it
  - "Sleep" - Put the awake device to sleep without enabling short-press wake
  - "Wake" - When selected as the short-press action, a short press wakes the device; does nothing while the device is awake
  - "Next Page" - Turn to the next page while reading
  - "Toggle Bookmark", "Reading Stats", "Mark Finished", "Refresh", "Change Font", "Guide Dots", "Focus Reading", "Auto Page Turn", "Sync Book", "File Transfer", "Calibre Wireless", "Join a Network", "Create Hotspot", "Screenshot", "Dark Mode", "Browse Files", "Library", or "Save Clipping" - Run the matching action
  - "Footnotes" - Select a footnote reference on the current page, with a list fallback when needed; see [Footnote Navigation](#footnote-navigation).

  **Quick-return from Footnotes** appears in this menu after you assign **Footnotes** to the short- or long-press Power action, or to **Long-press Back** or **Long-press Menu**. When it is on, a short Power press returns from a footnote page to the page you were reading.

- **Front Buttons** (devices with front buttons): **Remap Buttons** and **Remap Buttons (Reader)** let you choose which physical button does what. **Orientation Aware** can make the front buttons follow the screen orientation (**No**, **Nav Buttons**, or **All Buttons**). The other three set what holding a button does while you read:
  - **Long-press** (default **Off**): what holding a page-turn button does - **Chapter Skip** (next or previous chapter), **Change Font Size**, or **Orientation Change**.
  - **Long-press Back** (default **Browse Files**): the action for holding **Back**.
  - **Long-press Menu** (default **Off**): the action for holding **Confirm** (the menu button). Choose any of the shortcut actions above, such as **Sync Book** or **Library**.

- **Side Buttons**: Assign a **Short-press** and **Long-press** action separately for **Left/Up** and **Right/Down** while reading. Choose page or chapter turns, font-size changes, **Rotate Page CCW**, **Rotate Page CW**, **Rotate Page 180°**, or other shortcuts. Set an action to **Ignore** to leave that press unassigned. **Orientation Aware** and the **Up + Down Combo** shortcut are also here on supported devices. Rotation and font-size actions apply to EPUB and TXT text; they cannot rotate or resize pre-rendered XTC pages.

- **Quick Actions**: Pick a small menu of up to five actions and a shortcut that opens it; see [Quick Actions](./controls.md#quick-actions-triggers).

- **Taps & Gestures** (touchscreen devices): Configure the touch interactions
  available while reading from **Settings > Controls > Taps & Gestures**. The
  submenu includes:
  - **Next Page** and **Previous Page**: Choose which taps and swipes advance or
    go back one page. Each direction can be set to **Tap & Swipe** (default),
    **Tap Only**, **Swipe Only**, **Inverted Tap**, or **Disabled** independently.
  - **Pinch to Resize Font** (multi-touch devices): Enable or disable changing
    the font size with a two-finger pinch in EPUB and TXT readers.
  - **Two-finger Screen Rotation** (multi-touch devices): Enable or disable
    twisting two fingers to rotate the reading orientation in EPUB and TXT
    readers.
  - **Tap to Hide Status Bar**: Enable or disable tapping the visible status-bar
    area to show or hide it for the current reading session.
  - **Edge Gestures**: Assign upward and downward slides on the left and right
    screen edges; see [Edge Gestures](./controls.md#edge-gestures).
  - **Two-finger Swipe** (multi-touch devices): Assign an action to each
    two-finger swipe direction. The available actions depend on the device and
    reader format.
    The submenu is not shown on devices without a touchscreen, and the
    multi-touch entries appear only when the hardware supports them. See [Touch
    Reader Controls](#touch-reader-controls) for the gesture details.

#### 3.6.4 System

- **Device**: Opens a submenu with **Device Name**, **Time to Sleep**, **Custom Boot Screen**, **Language**, **Keyboard layouts**, and, on devices with a scalable-font reader, **Filename Fallback Font**. Devices with a real-time clock also show **Clock Format**, **Clock UTC Offset**, **Date Format**, **Date Separator**, and **Sync Date/Time**.
  - **Time to Sleep**: How long the device stays idle before it goes to sleep on its own, in minutes, or **Never**.
  - **Custom Boot Screen**: On by default. When it is off, CrossInk shows the standard logo at startup and keeps the current sleep screen visible when you wake it with the Power button, even if a custom image or boot-screen folder is set up. Turning it off does not delete your image or folders; turn it back on to use them again. See [Boot Screen](#38-boot-screen).
  - **Filename Fallback Font**: Pick a TrueType font used to show file names in scripts the built-in font does not cover. Put the font files in `/.crosspoint/languages/fonts` on the SD card (a regular style is required; bold is optional).

- **Files & Cache**: **Show Hidden Files**, **Hide File Extension**, **File Browser Display** (1 or 2 lines per file), **Clear Finished Books from History**, **Move Finished Books to Read Folder**, and **Clear Reading Cache** (frees space and fixes books that look wrong; your progress and bookmarks are kept).

- **Reading Stats**: Turn device-wide stats tracking on or off with **Track Reading Stats**. When tracking is on, you also get **Idle Time Threshold** and **All-time Stats** (**Automatic Backups** on devices with a clock, **Backup Now**, and **Reset All-time Stats**). Turning tracking off pauses new per-book and all-time stats but keeps the history and Time Left pace estimates already saved on the device. Stats entries and recorded-stat displays in menus and themes are hidden while tracking is off; the Dashboard can still show a saved Time Left estimate. Turn tracking back on to see the saved history and resume recording. Time and pages from the off period are not added later. See [Reader Features](./reader-features.md#turn-tracking-on-or-off) for per-book controls.

- **WiFi Networks**: Save the Wi-Fi networks the device uses for file transfers and firmware updates.

- **Sync Server**: Account and options for syncing book progress with CrossPoint Sync or a KOReader-compatible server, plus reading stats and clippings on CrossPoint Sync. The sync actions themselves are in [File Transfer](#35-file-transfer-screen). See [Reading Stats Sync](reading-stats-sync.md). **Smart sync** is the default for new setups and settles simple cases for you. Setups migrated from older versions keep **Ask on conflicts**; you can switch **Sync Behavior** at any time.

- **OPDS Servers**: Manage one or more OPDS [(Open Publication Distribution System)](https://en.wikipedia.org/wiki/Open_Publication_Distribution_System) libraries (online book catalogs) for browsing and downloading books. See [OPDS Servers (Multiple Libraries)](#365-opds-servers-multiple-libraries) below.

- **Check for Updates** and **SD Card Firmware Update**: Check for new firmware over Wi-Fi, or install a `firmware.bin` file you placed on the SD card.

- **About**: Shows the device model, firmware version, chip and display details, SD card transport and capacity, and memory figures. Use the buttons or swipe to scroll; **Back** returns to Settings. The figures are captured when the screen opens; reopen it for a fresh snapshot. See [Troubleshooting](./troubleshooting.md#device-information). You can also choose **Export** to save a support file (`/crossink-support.json`) to the SD card, with device and global settings alone or with the last opened EPUB's anonymous overrides. Nothing is uploaded. See [export coverage and privacy](./troubleshooting.md#export-support-information).

- **Language** (in **Device**): Set the menu language. CrossInk supports 28 languages: English, Spanish, French, German, Czech, Brazilian Portuguese, Russian, Swedish, Romanian, Catalan, Ukrainian, Belarusian, Italian, Polish, Finnish, Danish, Dutch, Turkish, Kazakh, Hungarian, Lithuanian, Slovenian, Valencian, Hebrew, Vietnamese, Slovak, Portuguese (Portugal), and Arabic. Only English is built into the standard firmware. For any other language, first copy its language files to the SD card; see [SD language files](./languages.md).

#### 3.6.5 OPDS Servers (Multiple Libraries)

CrossInk supports saving multiple OPDS servers and switching between them when browsing catalogs.

1. Open **Settings > System > OPDS Servers**.

2. Select **Add Server** to create a new entry, or select an existing server to edit it.

3. Configure these fields:
   - **Server Name**: Optional display name (for example, "Home Calibre" or "Public Catalog").

   - **OPDS Server URL**: Full catalog root URL (for Calibre Content Server, usually ends with `/opds`).

   - **Username / Password**: Optional credentials for authenticated servers.

4. Use **Delete Server** inside a server entry to remove it.

Behavior notes:

- You can store up to 8 OPDS servers.
- OPDS authentication supports HTTP Basic auth. If you use Calibre Content Server with authentication enabled, set it to Basic (not Digest).

You can also manage OPDS servers from the web interface while in File Transfer mode:

1. Connect to the device web UI.
2. Open `http://<device-ip>/settings`.
3. Use the **OPDS Servers** card to add, edit, or delete entries.

For web-based Wi-Fi network management, see [File Transfer](./webserver.md).

Each server also has a **Download Folder** setting:

- **Use default** follows **Default Folder** on the OPDS Servers screen. Existing servers use this automatically.
- **SD Root** downloads directly to the SD card root, even when the default points elsewhere.
- **Custom folder** opens the keyboard for a folder such as `/Books/Calibre`.

The web portal offers the same choices and previews the destination. Save the shared default with **Save default**, and each server with its **Save** button. Folder changes affect future downloads; existing books are not moved. Missing folders, including nested folders, are created when downloading. If the destination cannot be used, the download reports an error rather than saving elsewhere.

Folder paths may contain up to 63 UTF-8 bytes, including the leading slash. Relative input gains a leading slash; repeated/trailing slashes are removed. Parent-directory components (`..`), control characters, FAT-reserved characters and components ending in a dot or space are rejected.

OPDS catalogs may use Atom or OPDS 2 JSON. JSON navigation, groups, facets,
search, pagination, publication descriptions, and indirect EPUB links are
supported. The reader keeps up to 50 entries per feed and follows server page
links. Descriptions are bounded previews. Basic-auth credentials remain scoped
to the configured server. OAuth sign-in, purchases, and LCP service fulfillment
are not included.

#### 3.6.6 Web Settings (Wi-Fi + OPDS)

While in **File Transfer** mode, the web settings page includes management cards for both **Wi-Fi Networks** and **OPDS Servers**.

1. On device: open **File Transfer** and connect through **Join a Network** or **Create Hotspot**.
2. In a browser, open `http://<device-ip>/settings` or `http://crosspoint.local/settings`.
3. In **Wi-Fi Networks**, add, edit, or delete saved network entries (SSID + optional password).
4. In **OPDS Servers**, add, edit, or delete OPDS catalogs.

Behavior notes:

- Passwords are never shown back in the web UI after saving.
- Leaving Password blank while editing keeps the existing saved password unchanged.
- The web UI can save hidden-network SSIDs, but connecting to hidden networks still depends on the device-side Wi-Fi connection flow.

#### 3.6.7 Sync Server Quick Setup

CrossInk can sync your reading progress between devices using a sync server. It also works with KOReader apps and devices that use the same server and account.

**Most people only need Option A.** It is free, needs no extra software, and everything is done on the device. Options B and C are advanced: use B only if you already sync KOReader against the public KOReader server, and C only if you want to run your own server (this needs a computer or home server running Docker and a terminal).

**Server Type** controls optional progress fields, independently of the server URL:

- **CrossPoint**: standard KOReader sync plus rich book positions, including self-hosted CrossPoint servers.
- **KOSync**: standard progress without rich positions or extended metadata. Use this for `sync.koreader.rocks` and standard KOSync servers.
- **Other**: rich positions plus ISBN, ASIN, series, and series number for custom servers that accept these fields. Book metadata is only sent when **Send Metadata** is on.

Upgrades preserve the previous behavior: the default hosted server uses CrossPoint; existing custom URLs use KOSync. Choose the matching type after changing the URL. Stats and clippings still require separately detected server support and their own opt-in.

Percentage-only positions from compatible servers are supported as an approximate position in the local book, including optimized EPUBs. XPath positions still use the source-document map.

**Custom Header 1/2** accept `Header-Name: value` for authentication proxies, such as Cloudflare Access. Values are masked in settings and obfuscated on the SD card like passwords. Clear a row to remove it. In the web portal, replace the masked value with the complete header when editing. HTTP framing headers and built-in sync authentication headers cannot be overridden.

##### Option A: CrossPoint Sync Server (`sync.crosspointreader.com`, default)

When **Sync Server URL** is left empty, CrossInk uses the free CrossPoint sync server at `https://sync.crosspointreader.com`. It speaks the standard KOReader sync protocol (so KOReader apps can use it too) and additionally stores an exact spine/page position for lossless CrossInk-to-CrossInk sync.

1. On each CrossInk device:
   - Go to **Settings > System > Sync Server**.

   - Set **Username** and **Password** (enter the plain password; CrossInk computes MD5 internally, and use the same values on all devices).

   - Leave **Sync Server URL** empty (or set it to `https://sync.crosspointreader.com`).

   - On the first device, run **Sign Up** once to create the account directly from the device. On every other device, just run **Authenticate**.

Accounts are per server. Existing `sync.koreader.rocks` credentials do not exist on the CrossPoint server; either sign up again with the same username/password or use Option B to keep using the legacy server.

##### Option B (advanced): Legacy Public KOReader Server (`sync.koreader.rocks`)

Use this if you already sync KOReader devices against the official public server.

1. On each CrossInk device:
   - Go to **Settings > System > Sync Server**.

   - Set **Sync Server URL** to `https://sync.koreader.rocks` (required; an empty URL now points at the CrossPoint server instead).

   - Set **Username** and **Password** to your existing KOReader Sync credentials.

   - Run **Authenticate**.

2. If you do not have an account yet, run **Sign Up** on the device, or register once with curl:

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "https://sync.koreader.rocks/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

When this returns `HTTP 402` with `{"code":2002,"message":"Username is already registered."}`, pick a different username or use that existing account.

##### Option C (advanced): Self-Hosted Server (Docker Compose)

This option is for people comfortable with the command line.

1. Start a sync server:

```bash
mkdir -p kosync-quickstart
cd kosync-quickstart

cat > compose.yaml <<'YAML'
services:
  kosync:
    image: koreader/kosync:latest
    ports:
      - "7200:7200"
      - "17200:17200"
    volumes:
      - ./data/redis:/var/lib/redis
    environment:
      - ENABLE_USER_REGISTRATION=true
    restart: unless-stopped
YAML

# Docker
docker compose up -d

# Podman (alternative)
podman compose up -d
```

> [!NOTE]
> `ENABLE_USER_REGISTRATION=true` is convenient for first setup. After creating your users, set it to `false` (or remove it) to avoid unexpected registrations.

2. Verify the server:

```bash
curl -H "Accept: application/vnd.koreader.v1+json" "http://<server-ip>:17200/healthcheck"
# Expected: {"state":"OK"}
```

3. Register a user once.
   CrossInk authenticates against KOReader Sync (`koreader/kosync`) using an MD5 key, so register using the MD5 of your password:

> [!WARNING]
> Sending a reusable MD5-derived password over plain HTTP is insecure.
> Create unique sync-only credentials and do not reuse main account passwords.
> Prefer `https://<server-ip>:7200` whenever traffic leaves a fully trusted LAN or when using untrusted networks.
> Use `curl -k` only for self-signed certificate testing.

```bash
USERNAME="user"
PASSWORD="pass"
PASSWORD_MD5="$(printf '%s' "$PASSWORD" | openssl md5 | awk '{print $2}')"

curl -i "http://<server-ip>:17200/users/create" \
  -H "Accept: application/vnd.koreader.v1+json" \
  -H "Content-Type: application/json" \
  --data "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD_MD5\"}"
```

If this returns `HTTP 402` with `{"code":2002,"message":"Username is already registered."}`, the account already exists.

4. On each device:
   - Go to **Settings > System > Sync Server**.

   - Set **Username** and **Password** (enter the plain password; CrossInk computes MD5 internally, and use the same values on all devices).

   - Set **Sync Server URL** to `http://<server-ip>:17200`.

   - Run **Authenticate**.

If you use the HTTPS listener, use `https://<server-ip>:7200` (`curl -k` only for self-signed certificate testing).

##### Syncing While Reading

Once any of the options above is set up, press **Confirm** while reading to open the Reader Menu and select **Sync Book** (on button devices it is in the **Bookmarks** tab). You can also set **Settings > Controls > Front Buttons > Long-press Menu** to **Sync Book** (or assign **Sync Book** to a Power button action) and hold **Confirm** to start a sync directly.

- With **Sync Behavior** set to **Ask on conflicts**, CrossInk asks only when the server's position differs from yours: choose **Apply Remote** to jump to remote progress or **Upload Local** to push current progress. With no remote position it uploads without asking, and matching positions show **Already synced**.
- With **Sync Behavior** set to **Smart sync**, CrossInk auto-resolves simple cases: upload when no remote progress exists, confirm and leave both unchanged when local and remote progress are already synchronized, upload when local progress is further ahead, or apply remote when remote progress is further ahead.

### 3.7 Sleep Screen

The sleep screen is what the display shows while the device sleeps. To change it, open **Settings > Display > Sleep Screen**. The **Wallpaper** row picks the mode:

| Mode               | Behavior                                                                                                                                               |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **None**           | A blank screen.                                                                                                                                        |
| **Dark** (default) | The CrossInk logo on a dark background.                                                                                                                |
| **Light**          | The CrossInk logo on a white background.                                                                                                               |
| **Custom**         | A custom image from the SD card (see below). Falls back to **Dark** if no custom image is found.                                                       |
| **Cover**          | The cover of the currently open book. Falls back to **Dark** if no book is open.                                                                       |
| **Cover + Custom** | The cover of the currently open book, shown only while actively reading. Falls back to **Custom** behavior when not reading.                           |
| **Cover Overlay** | Draws a transparent sleep image over the current book cover. Uses the cover alone if no overlay is available, or a white background if no cover is available. |
| **Page Overlay**   | Keeps the current reader page visible and draws a sleep wallpaper over it. If no wallpaper is available, the page remains visible while reading.       |
| **Reading Stats**  | Recent reading stats. Falls back to **Minimal** while tracking is off for the device or recent book. Hidden while **Track Reading Stats** is off.      |
| **Minimal**        | A compact sleep screen based on the Minimal home layout.                                                                                               |
| **Minimal Stats**  | A compact sleep screen with recent reading stats, on supported devices; falls back to **Minimal** while tracking is off for the device or recent book. |
| **Dashboard**      | A dashboard-style sleep screen based on the Dashboard theme.                                                                                           |
| **Quick Resume**   | Keeps whatever was on the screen (a page or a menu) visible while the device sleeps.                                                                   |

The same screen also has **Quick Resume on Timeout**. When it is on, the device uses **Quick Resume** whenever it falls asleep from inactivity (see **Time to Sleep** in [Settings > System > Device](#364-system)), whatever mode you chose above. Pressing Power to sleep still uses your chosen mode.

#### Cover settings

When using **Cover**, **Cover + Custom**, or **Cover Overlay**, two more settings apply:

- **Cover Mode**: **Fit** (default; scales the cover to fit, with white borders) or **Crop** (scales and crops the cover to fill the screen).
- **Cover Filter**: **None** (default; grayscale), **Contrast** (black and white), or **Inverted** (inverted black and white).

#### Custom images

To use custom sleep images, set the sleep screen to **Custom**, **Cover + Custom**, **Page Overlay**, or **Cover Overlay**, then place images on the SD card:

- **Multiple Images (recommended):** Create a `.sleep` directory in the root of the SD card and place any number of `.bmp` images inside. In **Page Overlay** and **Cover Overlay** modes, `.png` images are also supported. One image will be randomly selected each time the device sleeps. (A directory named `sleep` is also accepted as a fallback.)
- **Single Image:** Place a file named `sleep.bmp` in the root directory. In **Page Overlay** and **Cover Overlay** modes, `sleep.png` is also supported. These files are used as fallbacks if no valid image is found in the `.sleep`/`sleep` directory.

In **Page Overlay** mode, white BMP pixels and transparent PNG pixels let the current reader page show through; the remaining wallpaper pixels are drawn over the page. **Cover Overlay** uses the same image selection over the current book cover. PNG alpha transparency preserves cover grayscale on supported panels; BMP white pixels are transparent and other pixels form a black mask.

> [!TIP]
> For best results:
>
> - Use uncompressed BMP files, preferably with 4-bit or 8-bit indexed BMP.
> - X4: Use a resolution of 480x800 pixels to match the device's screen resolution.
> - X3: Use a resolution of 528x792 pixels to match the device's screen resolution.

> [!TIP]
> You can set an image as the sleep screen cover from the image viewer in **[Browse Files](#33-browse-files-screen)**. The viewer asks for confirmation before saving your selection.

---

### 3.8 Boot Screen

The **Custom Boot Screen** toggle is in **Settings > System > Device** and is enabled by
default. When it is on, you can replace the default CrossInk logo shown during
a cold boot with BMP images stored on the SD card. A configured custom boot
screen is also shown after a power-button wake. This is separate from the
sleep screen.

CrossInk supports two ways to choose a custom boot screen:

- **One fixed image:** In **[Browse Files](#33-browse-files-screen)**, open a
  BMP image from any folder, open its context menu, and choose **Set Boot
  Screen**. This selected image always takes priority over a boot-screen
  folder. To stop using it, open the same image and choose **Clear Boot Screen**.

- **Rotating images:** Create a `/.bootscreen` folder in the root of the SD card
  and place BMP files directly inside it. CrossInk randomly selects an image
  each time it starts and avoids recently used images where possible. A
  `/bootscreen` folder is also supported. Folder names are case-insensitive; if
  both folders exist, `/.bootscreen` takes priority.

CrossInk caches folder contents for faster startup. If you add BMP files
directly to an already-used folder while the device is off, the new files may
not be selected until the index is rebuilt. Upload them through the **File
Transfer** web file manager or **Nearby File Transfer** to invalidate the
index automatically.

The selection and fallback order is:

1. A selected BMP set with **Set Boot Screen**.
2. A usable BMP selected from the active root-level boot-screen folder.
3. The standard CrossInk logo.

On a cold boot, if a selected image or the active folder is missing, unreadable,
empty, or contains no usable BMP, CrossInk falls back to the next option in the
order above. If both folder names exist, `/.bootscreen` masks `/bootscreen`
even when the hidden folder is empty or unusable; remove or rename it to use
`/bootscreen` instead.

On a power-button wake, an existing selected BMP or boot-screen folder (even
an empty or unusable folder) runs the boot-screen flow and then falls back to
the standard logo if no image can be shown. If the selected file was removed
and no boot-screen folder exists, the wake is splashless and keeps the current
sleep screen visible. With no selected image or boot-screen folder, a
power-button wake is also splashless.

> [!TIP]
> Use an uncompressed BMP at your device's screen resolution for the best result: 480x800 pixels on X4 or 528x792 pixels on X3. Images with other dimensions are centered and scaled down as needed.

---

### 3.9 Custom Fonts (SD Card)

CrossInk can load extra fonts from the SD card, beyond the built-in Lexend Deca and Bitter families. Custom fonts can cover more scripts, including CJK (Chinese, Japanese, Korean).

There are three ways to install fonts:

1. **Manual SD card copy (recommended):** Download font files from [Inky](https://inky.crossink.dev/#downloads) and copy them to `/.fonts/` (preferred) or `/fonts/` on your SD card.
2. **Download from device:** Go to **Settings > Reader > Font Options > Download Fonts**, browse the available font families, and select one to download over Wi-Fi. Note that downloading over Wi-Fi can be unstable. If you see download errors, use one of the other methods.
3. **Upload via web interface:** While in **File Transfer** mode, open the web UI in a browser and navigate to the **Fonts** tab to upload `.cpfont` files. For supported devices, `.ttf` font files can also be uploaded.

Once installed, custom fonts appear in **Settings > Reader > Font Options > Font Family** alongside the built-in fonts.

See [SD Card Fonts](./sd-card-fonts.md) for full installation details and SD card folder structure.

---

## 4. Reading Mode

Once you have opened a book, the button layout changes to facilitate reading.

### Page Turning

| Action            | Buttons                       |
| ----------------- | ----------------------------- |
| **Previous Page** | Press **Left** _or_ **Up**    |
| **Next Page**     | Press **Right** _or_ **Down** |

Change the individual side-button actions in **Settings > Controls > Side Buttons**.

If the Power button's short-press action is set to "Next Page", you can also turn to the next page by briefly pressing the Power button.

### Chapter Navigation

- **Next Chapter:** Press and **hold** the **Right** (or **Down**) side button briefly, then release.
- **Previous Chapter:** Press and **hold** the **Left** (or **Up**) side button briefly, then release.

These are the default side-button holds. Front buttons do nothing on a long press until you set **Long-press** to **Chapter Skip** in **Settings > Controls > Front Buttons**. You can also assign each side-button hold separately in **Settings > Controls > Side Buttons**.

### Auto Page Turn

Auto Page Turn automatically advances pages at a set interval, useful for hands-free reading. This feature can be enabled and configured from the **[Reader Menu](#5-reader-menu)** while reading an EPUB.

### Tilt Page Turn (X3 and Sticky)

On devices with a motion sensor, currently the **Xteink X3** and **Sticky**, you can turn pages by tilting the device. Turn it on and choose the tilt direction (**Left-Right** or **Forward-Back**) in **Settings > Controls**.

### Touch Reader Controls

On supported touchscreen devices, touch page turns are enabled by default.
**Next Page** and **Previous Page**, in **Settings > Controls > Taps & Gestures**,
are configured independently and both default to **Tap & Swipe**:

| Option           | Taps                             | Swipes   |
| ---------------- | -------------------------------- | -------- |
| **Tap & Swipe**  | Enabled                          | Enabled  |
| **Tap Only**     | Enabled                          | Disabled |
| **Swipe Only**   | Disabled                         | Enabled  |
| **Inverted Tap** | Enabled, with reversed tap zones | Disabled |
| **Disabled**     | Disabled                         | Disabled |

Swipe left for the next page when **Next Page** allows swipes, or swipe right
for the previous page when **Previous Page** allows swipes. In right-to-left
EPUBs, normal tap zones and horizontal swipe directions follow the book's
reading direction instead. In left-to-right books, when both directions
allow taps, the normal zones are the left third for the previous page and the
right two-thirds for the next page. If either of those settings is **Inverted
Tap**, the shared zones become the left two-thirds for the next page and the
right third for the previous page. If only one direction allows taps, taps
across the page turn in that direction. The top and bottom gesture bands are
reserved for vertical gestures, so taps in those bands do not turn pages.

For **EPUB readers**, vertical gestures depend on the device:

- On **Sticky**, swipe up to open the reader menu. Swipe down to open the
  reader-details/frontlight panel; use that panel's header to return Home.
- On **X4 Pro**, swipe up to open the reader menu and swipe down to open the
  frontlight panel, which also shows chapter pages and book progress while
  reading. The capacitive Home key returns Home on a short press and
  opens the reader menu on a long press by default. Configure these actions, or
  disable the key while reading, in **Settings > Controls > Home Button**.
- On other touchscreen devices, swipe down to open the reader menu and swipe up
  to return Home.

**XTC** and **TXT** readers use narrower, format-specific vertical routing. For
example, Sticky TXT has the down-swipe reader-details/frontlight panel but no
swipe-to-menu or swipe-to-Home action; on X4 Pro, XTC keeps the swipe-up reader
menu while its down-swipe frontlight panel is top-edge only, and TXT has no
swipe-to-menu action. These vertical gestures are separate from the page-turn
settings above.

Choose **Disabled** in **Next Page** or **Previous Page** to stop touch page
turns in that direction without disabling the touchscreen's vertical reader-menu
or frontlight gestures. **Disable Touchscreen**, in **Settings > Reader**,
prevents screen-touch input while a book is open but keeps it available in
reader menus. For the different touch selection gestures used by [dictionary
lookup](./dictionary.md#looking-up-a-word) and [clippings](./reader-features.md#clippings-and-highlights), see those feature guides.

On devices with multi-touch support, you can also assign actions to two-finger
swipes from **Settings > Controls > Taps & Gestures > Two-finger Swipe**. Set an
action for **Swipe Up**, **Swipe Down**, **Swipe Left**, or **Swipe Right**, then
move two fingers together in that direction while reading. Available actions
are **Not Set**, **Increase Brightness**, **Decrease Brightness**, **Increase
Warmth**, **Decrease Warmth**, **Select Chapter**, **Next Chapter**, **Previous Chapter**, **Increase
Font Size**, and **Decrease Font Size**. Brightness and warmth options appear
only when the hardware supports them; Select Chapter opens the chapter list in EPUB and XTC books, Next/Previous Chapter apply to EPUBs, and font
size options apply to EPUB and TXT books. Each direction starts as **Not Set**,
and each action can be assigned to only one direction; choosing it again moves
it to the new direction. On image-based XTC books, Next/Previous Chapter and font-size actions
are consumed but cannot change the pre-rendered pages. See [Two-finger Swipe
Actions](./controls.md#two-finger-swipe-actions) for the complete list and
reader-specific limitations.

On supported multi-touch devices, enable **Pinch to Resize Font** in the same
menu, then move two fingers apart to increase the font or together to decrease
it. Each completed pinch changes one available font-size step. Pinch resizing
works in EPUB and TXT readers; XTC pages are pre-rendered and cannot be
resized. Pinch input also requires reader touch input to remain enabled.

### Slider Controls

For sliders with five-unit increments in the Reader Menu and Settings, tapping
the slider track rounds the selected value to the nearest multiple of five.

### Footnote Navigation

When reading an EPUB that contains footnotes, tap a reference or choose
**Footnotes** from the Reader Menu's **More** tab (or an assigned shortcut).
If there is one footnote, it opens directly. With several visible references,
CrossInk highlights them on the reading page: use the direction buttons to
choose one and **Confirm** to open it, or tap the reference. Links without a
visible target use a list instead. Press **Back** to return to your original
reading position.

After following an in-book link to a full chapter, sleeping or closing the book reopens on the last page read. Back retains the three most recent link origins across reopening and KOReader sync. If you close a transient footnote preview, the book resumes on the page that opened that preview, with any earlier link origins still available through Back.

### System Navigation

- **Return to Home:** Press the **Back** button to close the book and return to the **[Home](#31-home-screen)** screen.
- **Return to Browse Files:** Press and hold the **Back** button to close the book and return to the **[Browse Files](#33-browse-files-screen)** screen.
- **Reader Menu:** Press **Confirm** to open the **[Reader Menu](#5-reader-menu)**, which includes chapter navigation, reading options, and more. On touch devices you can also open it with a swipe; see [Touch Reader Controls](#touch-reader-controls).

### Supported Languages

CrossInk renders text using the following Unicode character blocks, enabling support for a wide range of languages:

- **Latin Script (Basic, Supplement, Extended-A/B):** Covers English, German, French, Spanish, Portuguese, Italian, Dutch, Swedish, Norwegian, Danish, Finnish, Polish, Czech, Hungarian, Romanian, Slovak, Slovenian, Turkish, Catalan, and others.
- **Cyrillic Script (Standard and Extended):** Covers Russian, Ukrainian, Belarusian, Bulgarian, Serbian, Macedonian, Kazakh, Kyrgyz, Mongolian, and others.
- **Vietnamese:** Supported via extended Latin glyph coverage in the built-in reader fonts.

What is not supported with built-in reader fonts: Chinese, Japanese, Korean, Arabic, Greek, Hebrew, and Farsi. However, **CJK, Hebrew, Greek, and other extended scripts can be enabled by installing custom SD card fonts**; see [Custom Fonts (SD Card)](#39-custom-fonts-sd-card).

---

## 5. Reader Menu

Press **Confirm** while reading to open the Reader Menu. From here you can access reading utilities and navigation options without leaving the book.

EPUB books use the same five icon tabs on touch and button devices:

| Tab (icon)               | Main options                                                                                                                                                    |
| ------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Font** (letters)       | Reader Font (family, size, character spacing), Dictionary Font, Line/Word Spacing, Text Anti-Aliasing, Focus Reading, Guide Dots                                |
| **Layout** (text lines)  | Margins, Orientation, Alignment, Images, Hyphenation, Publisher Page Numbers, paragraph spacing and publisher styling                                           |
| **More** (three dots)    | Word lookup, Select Chapter, Go to %, Go to Stable Page when available, Auto Page Turn, Footnotes; Reading Stats on button devices when tracking is enabled     |
| **Bookmarks** (bookmark) | Add/remove and view bookmarks, create/view clippings, screenshot, position QR; Sync Book, Nearby Position Sync, and Send Nearby Book on button devices          |
| **Settings** (gear)      | Status Bars, Controls, Book Dictionary, EPUB Render Mode, Indexing Method, finished status, Track Reading Stats, cache/stats resets, Reset Book Reader Settings |

Some actions appear only when the book or device supports them. On touch
screens, tap a tab and then an option. On button devices, **Left/Right** switch
tabs, **Up/Down** select rows, and **Confirm** opens the selected option. Font,
spacing, and margin changes have a live preview. Global settings use the same
Left/Right tab and Up/Down row navigation.

**Reset Book Reader Settings** restores the current EPUB's inherited global
reader defaults without deleting progress, bookmarks, clippings, or stats.
**Delete Book Cache** rebuilds cached book data and keeps those reader choices.

TXT and XTC books have their own menus and a smaller set of options; the
five-tab layout above is for EPUBs. For stats tracking, use **Track Reading
Stats** in the EPUB **Settings** tab or the XTC Reader Menu. For format-specific
features, see [Reader Features](./reader-features.md).

Press **Back** at any time to close the menu and return to your current page.

### 5.1 Chapter Selection

In EPUB books, open the Reader Menu's **More** tab and select **Select Chapter**.

1. Use **Left** (or **Up**), or **Right** (or **Down**) to highlight the desired chapter.
2. Press **Confirm** to jump to that chapter.
3. _Alternatively, press **Back** to cancel and return to your current page._

---

### 5.2 Bookmarks

Bookmarks save your place in an EPUB so you can return to it later.

To add or remove a bookmark: press **Confirm** to open the Reader Menu, open the **Bookmarks** tab, and choose **Add Bookmark** (or **Remove Bookmark** if the page already has one). A short popup confirms the change. You can also assign **Toggle Bookmark** to a button; see [Controls](#363-controls).

To go to a bookmark: open the Reader Menu, open the **Bookmarks** tab, choose **View Bookmarks**, select a bookmark, and press **Confirm**. To delete all of a book's bookmarks, choose **Clear Bookmark List** in the same tab; you will be asked to confirm.

Each book's bookmarks are saved on the SD card in the `.crosspoint/bookmarks` folder.

### 5.3 Dictionary

Dictionary lookup supports word selection, recent per-book history, chained lookups from definitions, and per-book dictionary overrides. See the [Dictionary guide](./dictionary.md) for installation and preparation instructions.

## 6. Current Limitations & Roadmap

CrossInk is in active development. These are the current limits:

- **Cover images:** Large cover images embedded in an EPUB can take several
  seconds to convert for the sleep screen and Home-screen thumbnail. If a book
  is slow to open, use the built-in [EPUB optimization](./webserver.md#epub-optimization)
  before uploading it.
- **Image formats:** Most JPG, PNG, and non-interlaced GIF images in EPUBs display correctly. GIF covers and thumbnails are supported; animated GIFs show their first frame. Damaged, oversized, interlaced GIFs and progressive JPEGs are not supported. Existing EPUB page caches rebuild automatically after this update.
- **Built-in reader fonts:** Chinese, Japanese, Korean, Arabic, Greek, Hebrew, and Farsi need a custom SD card font; see [Supported Languages](#supported-languages).

---

## 7. Troubleshooting Issues & Escaping Bootloop

For network, upload, and settings problems, see [Troubleshooting](./troubleshooting.md).

**Reporting a bug or crash:** If something goes wrong, [open an issue](https://github.com/uxjulia/CrossInk/issues) and describe what you were doing. After a crash, CrossInk shows a **System Crash** screen and saves a report to the SD card. Look for `crash_report.txt` in the top (root) folder of the SD card and attach it to your report. Press **Back** (or tap the screen) to leave the crash screen. You can also open **Settings > System > About** and choose **Export** to save a settings summary you can share; nothing is uploaded. See [Export Support Information](./troubleshooting.md#export-support-information).

**If the device keeps restarting (a "bootloop"):**

1. Press and release the **Reset** button.
2. Right after, press and hold the **Back** button and the **Power** button. This skips reopening your last book and goes to the **Home** screen.

**If the Home screen also fails**, the saved settings or cache may be damaged. Turn the device off, put the SD card in a computer, and delete the `.crosspoint` folder on the SD card. This resets settings, reading progress, bookmarks, and caches (see [Data and Cache](./data-cache.md)). To lose less, delete only `crossink-settings.json`, `settings.json`, `state.json`, or the `epub_*` folders inside `.crosspoint/`.

**If a firmware update went wrong:** Put a working `firmware.bin` on the root of the SD card, turn the device off, then hold **Power** and **Up** while turning it on (use **Down** instead of **Up** on X4 Pro and X4 Classic). CrossInk opens a screen where you pick the `.bin` file to install. You can also reflash over USB; see [Installation](./installation.md).

### Advanced: Serial Monitor Logs

This is for developers and advanced users who need detailed logs. Connect the device to a computer with a USB cable and run the debugging monitor script from the CrossInk source code. It needs Python 3 with `pyserial`, `colorama`, and `matplotlib` (install with `pip3 install pyserial colorama matplotlib`):

```
python3 scripts/debugging_monitor.py
```

The script finds the serial port automatically. You can also give it one:

```
python3 scripts/debugging_monitor.py /dev/ttyACM0        # Linux
python3 scripts/debugging_monitor.py /dev/tty.usbmodem1  # macOS
python3 scripts/debugging_monitor.py COM7                # Windows
```

**Features:**

- Color-coded log output by category (errors, memory, display, EPUB parsing, etc.)
- Live memory usage graph updated every second
- Interactive command prompt: type a command and press Enter to send it to the device
- Screenshot capture: saves the current display to `screenshot.bmp` when triggered by the device

**Options:**

| Option               | Description                                               |
| -------------------- | --------------------------------------------------------- |
| `--baud RATE`        | Baud rate (default: 115200)                               |
| `--filter KEYWORD`   | Show only lines containing the keyword (case-insensitive) |
| `--suppress KEYWORD` | Hide lines containing the keyword (case-insensitive)      |

**Examples:**

```
# Show only memory-related log lines
python3 scripts/debugging_monitor.py --filter MEM

# Hide noisy SD card log lines
python3 scripts/debugging_monitor.py --suppress "[SD]"
```

Press **Ctrl-C** or close the graph window to exit.
