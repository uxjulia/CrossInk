---
title: User Guide
nav_order: 1.5
---

# CrossInk User Guide

This guide covers day-to-day device use. For focused reference material, see [Reader Features](./reader-features.md), [Controls](./controls.md), [SD Card Fonts](./sd-card-fonts.md), [File Transfer](./webserver.md), and [Troubleshooting](./troubleshooting.md). For Dashboard and Minimal Home-screen gestures, see [Touch Navigation](./touch-navigation.md).

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
    - [3.4 Recent Books Screen](#34-recent-books-screen)
    - [3.5 File Transfer Screen](#35-file-transfer-screen)
    - [3.5.1 Calibre Wireless Transfers](#351-calibre-wireless-transfers)
    - [3.6 Settings](#36-settings)
      - [3.6.1 Display](#361-display)
      - [3.6.2 Reader](#362-reader)
      - [3.6.3 Controls](#363-controls)
      - [3.6.4 System](#364-system)
      - [3.6.5 OPDS Servers (Multiple Libraries)](#365-opds-servers-multiple-libraries)
      - [3.6.6 Web Settings (Wi-Fi + OPDS)](#366-web-settings-wi-fi--opds)
      - [3.6.7 KOReader Sync Quick Setup](#367-koreader-sync-quick-setup)
        - [Option A: CrossPoint Sync Server (`sync.crosspointreader.com`, default)](#option-a-crosspoint-sync-server-synccrosspointreadercom-default)
        - [Option B: Legacy Public KOReader Server (`sync.koreader.rocks`)](#option-b-legacy-public-koreader-server-synckoreaderrocks)
        - [Option C: Self-Hosted Server (Docker Compose)](#option-c-self-hosted-server-docker-compose)
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
    - [Footnote Navigation](#footnote-navigation)
    - [System Navigation](#system-navigation)
    - [Supported Languages](#supported-languages)
  - [5. Reader Menu](#5-reader-menu)
    - [5.1 Chapter Selection](#51-chapter-selection)
    - [5.2 Bookmarks](#52-bookmarks)
  - [6. Current Limitations & Roadmap](#6-current-limitations--roadmap)
  - [7. Troubleshooting Issues & Escaping Bootloop](#7-troubleshooting-issues--escaping-bootloop)

## 1. Hardware Overview

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

When the Power Button and Down button are pressed at the same time, it will take a screenshot and save it in the folder `screenshots/`.

Alternatively, while reading a book, press the **Confirm** button to open the reader menu and select **Take screenshot**.

---

## 2. Power & Startup

### Power On / Off

To turn the device on or off, **press and hold the Power button for approximately half a second**.
In **Settings > Controls > Power Button** you can configure the power button to turn the device off with a short press instead of a long one.

To reboot the device (for example after a firmware update or if it's frozen), press and release the Reset button, and then quickly press and hold the Power button for a few seconds.

### First Launch

Upon turning the device on for the first time, you will be placed on the **[Home](#31-home-screen)** screen.

> [!NOTE]
> On subsequent restarts, the firmware will automatically reopen the last book you were reading.

---

## 3. Screens

### 3.1 Home Screen

The Home screen is the main entry point to the firmware. From here you can navigate to **[Reading Mode](#4-reading-mode)** with the most recently read book, the **[Browse Files](#33-browse-files-screen)** screen, the **[Recent Books](#34-recent-books-screen)** screen, the **[File Transfer](#35-file-transfer-screen)** screen, or **[Settings](#36-settings)**.

### 3.2 Reading Mode

See [Reading Mode](#4-reading-mode) below for more information.

### 3.3 Browse Files Screen

The Browse Files screen acts as a file and folder browser. The full path to the current directory is shown at the top of the screen. File extensions are displayed alongside each filename, and directories are shown with brackets (e.g. `[folder-name]`). Hidden directories can be shown from settings.

- **Navigate List:** Use **Left** (or **Up**), or **Right** (or **Down**) to move the selection cursor up and down through folders and books. You can also long-press these buttons to scroll a full page up or down.
- **Open Selection:** Press **Confirm** to open a folder or start reading a selected book. Selecting a `.bmp` file will open the image viewer.
- **Delete Files or Folders:** Hold and release **Confirm** to open the selected file or folder action menu, then choose **Delete**. You will be given an option to either confirm or cancel. Folder deletion is limited to empty folders.
- **Book Actions:** EPUB and XTC files can also show options such as **Delete Cache** or **Mark Finished** from the same action menu.

### 3.4 Recent Books Screen

The Recent Books screen lists the most recently opened books in a chronological view, displaying title and author.

### 3.5 File Transfer Screen

The File Transfer screen allows you to upload and manage files on the device.
Choose **Join a Network**, **Calibre Wireless**, or **Create Hotspot** to start
the web server for the selected mode.

See the [File Transfer guide](./webserver.md) for connection and upload details.

The web file manager can upload, download, rename, move, and delete files on the device.

The web interface also supports **WebDAV**, allowing you to mount the device as a network drive and manage files directly from your computer's file manager.

Download links for files already on the device are available in the web interface, so you can retrieve books or screenshots over Wi-Fi without connecting a cable.

A **Wi-Fi signal strength indicator** (dBm) is displayed on-screen during joined-network web server sessions.

The same screen also has **Receive File**, which receives a supported
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

- **Sleep Screen**: Which sleep screen to display when the device sleeps:
  - "Dark" (default) - The default dark CrossInk logo sleep screen
  - "Light" - The same default sleep screen, on a white background
  - "Custom" - Custom images from the SD card; see [Sleep Screen](#37-sleep-screen) below for more information
  - "Cover" - The book cover image (Note: this is experimental and may not work as expected)
  - "None" - A blank screen
  - "Cover + Custom" - The book cover image while actively reading, falls back to "Custom" behavior otherwise
  - "Page Overlay" - Uses an image to overlay on top of the current page. Best used with transparent `.png` files or black and white `.bmp` files.
  - "Reading Stats" - Recent reading stats on the sleep screen
  - "Minimal" - A minimal sleep screen
  - "Minimal Stats" - A minimal stats sleep screen on supported devices
  - "Dashboard" - A dashboard-style sleep screen based on the Dashboard theme
  - "Quick Resume" - Keeps the current content visible while sleeping

- **Sleep Screen Cover Mode**: How to display the book cover when "Cover" sleep screen is selected:
  - "Fit" (default) - Scale the image down to fit centered on the screen, padding with white borders as necessary
  - "Crop" - Scale the image down and crop as necessary to try to fill the screen (Note: this is experimental and may not work as expected)

- **Sleep Screen Cover Filter**: What filter will be applied to the book cover when "Cover" sleep screen is selected:
  - "None" (default) - The cover image will be converted to a grayscale image and displayed as it is
  - "Contrast" - The image will be displayed as a black & white image without grayscale conversion
  - "Inverted" - The image will be inverted as in white & black and will be displayed without grayscale conversion

- **Quick Resume on Timeout**: Whether to enable the "Quick Resume" sleep screen when the device goes to sleep due to inactivity (System > Time to Sleep). This is useful for quickly resuming reading without waiting for the device to fully wake up and load the book. This overwrites the Sleep Screen Cover Mode when enabled.

- **Hide Battery %**: Configure where to suppress the battery percentage display in the status bar; the battery icon will still be shown:
  - "Never" (default) - Always show battery percentage
  - "In Reader" - Show battery percentage everywhere except in reading mode
  - "Always" - Always hide battery percentage

- **Hide Clock**: On devices with a real-time clock, choose whether the clock is
  shown everywhere, hidden only in the reader, or always hidden.

- **Refresh Frequency**: Set how often the screen does a full refresh while reading to reduce ghosting; options are every 1, 5, 10, 15, or 30 pages.

- **UI Theme**: Set which UI theme to use:
  - "Classic" - The original CrossInk theme
  - "Minimal" - A minimal theme with a large book cover
  - "Dashboard" - A dashboard-style home layout
  - "Lyra" - A theme with simple icons featuring your current book
  - "Lyra Extended" - Lyra, but displays 3 books instead of 1 on the **[Home Screen](#31-home-screen)**
  - "Lyra Carousel" - A carousel-based Lyra home layout
  - "RoundedRaff" - A rounded theme with additional visual styling

- **Recent Books View**: Choose whether the Recent Books screen uses a list or grid layout.

- **Sunlight Fading Fix**: Configure whether to enable a software-fix for the issue where white X4 models may fade when used in direct sunlight:
  - "OFF" (default) - Disable the fix
  - "ON" - Enable the fix

- **Frontlight and wake schedule** (on supported devices with a frontlight and
  real-time clock): Open **Settings > Display > Frontlight**.
  - **Restore Light on Wake**: When enabled, a frontlight that was on before
    sleep is turned back on when the device wakes. If the light was off before
    sleep, a complete schedule can still decide whether it should turn on.
  - **Frontlight Schedule**: Enable the daily wake schedule, then set **Start**
    and **End** in local time. Times use one-minute increments. The schedule
    includes the Start time and excludes the End time, and it can cross
    midnight (for example, 9:00 PM to 7:00 AM). Start and End must both be set
    and different; otherwise the schedule is inactive.
  - The schedule is checked when the device boots or wakes, not continuously
    while it is already awake. Restore on Wake takes precedence when the light
    was on before sleep. Set the device's local time and UTC offset under
    **Settings > System > Device** so the schedule uses the expected clock.
    When the schedule is disabled or an endpoint is unset, the endpoint value
    is shown as `--`; saved endpoint times are retained for later re-enabling.

> [!NOTE]
> A battery charging indicator is shown on the battery icon whenever the device is actively charging.

#### 3.6.2 Reader

- **Reader Font Family**: Choose the font used for reading:
  - "Lexend Deca" (default)
  - "Bitter"

- **Reader Font Size**: Adjust the text size for reading, built-in font sizes include: 10, 12, 14, and 16 pt.

- **Reader Line Spacing**: Adjust the line height as a percentage.

- **Word Spacing**: In EPUB books, choose **Normal** or one of four wider
  spacing levels between words. Open the reader menu, then select **Reader
  Options > Font Options > Word Spacing**. Changing it reflows the current
  book, so page positions may change; it is not available for TXT books.

- **Reader Screen Margin**: Controls the screen margins in Reading Mode between 5 and 40 pixels in 5-pixel increments.

- **Reader Paragraph Alignment**: Set the alignment of paragraphs; options are "Justified" (default), "Left", "Center", "Right", or "Book's Style".

- **Publisher Page Numbers**: Show page numbers supplied by the EPUB when the
  book includes them.

- **Hyphenation**: Whether to hyphenate text in Reading Mode; options are "ON" or "OFF".

- **Reading Orientation**: Set the screen orientation for reading EPUB files:
  - "Portrait" (default) - Standard portrait orientation
  - "Landscape CW" - Landscape, rotated clockwise
  - "Portrait 180" - Portrait, upside down
  - "Landscape CCW" - Landscape, rotated counter-clockwise

- **Extra Paragraph Spacing**: Set how to handle paragraph breaks:
  - "ON" - Vertical space will be added between paragraphs in Reading Mode
  - "OFF" - Paragraphs will not have vertical space added, but will have first-line indentation

- **Reader Dark Mode**, **Embedded Style**, **Images**, **Bionic Reading**, and
  **Guide Dots** are directly available from the Reader settings. See
  [Reader Features](./reader-features.md) for their behavior, including the
  [Bionic Reading](./reader-features.md#bionic-reading) guide.

- **Touch Reader Controls**: Enable or disable touchscreen page turns and
  reader gestures on supported devices. **Disable Touchscreen** blocks touch
  input while a book is open, while leaving touch available in reader menus so
  you can turn it back on.

- **Customize Status Bar**: Configure the status bar displayed while reading:
  - Chapter Page Count - Show/Hide the current page in the chapter (ex: 5/25). Page count may change based on the font size and margins set.
  - Book Progress Percentage - Show/Hide the current percent progress in the book.
  - Progress Bar - Show/Hide a progress bar for either the book or chapter.
  - Progress Bar Thickness - Set the thickness of the progress bar
  - Title - Display the chapter or book title
  - Time Left - Display the estimated reading time left for the book or chapter
  - Battery - Show/Hide the battery indicator
  - XTC Status Bar - Show/Hide a status bar for XTC files

On touchscreen readers, tap the status-bar area while reading to show or hide
the entire status bar for the current reading session. This quick toggle does
not change the page layout or page breaks; use **Customize Status Bar** to
choose which status-bar items are shown.

#### 3.6.3 Controls

- **Power Button**: Configure short-press and long-press power button actions.

- **Front Buttons**: Configure front-button remapping, orientation awareness,
  reader-only long-press behavior, Back action, and Menu action.

- **Side Buttons**: Configure side-button layout, orientation awareness, and side-button long-press behavior.

- **Side Button Layout (reader)**: Swap the order of the up and down buttons from "Prev/Next" (default) to "Next/Prev" or "Next/Next". You can also disable them entirely. This change is only in effect when reading.

- **Long-press Behavior**: Set whether long-pressing front page-turn buttons does nothing, skips to the next/previous chapter, or changes reader orientation.

- **Side Button Long-press Action**: Set whether long-pressing side buttons does nothing, skips chapters, changes font size, or changes orientation.

- **Short-press Action / Long-press Action**: Controls the effect of a short or long press of the power button. Available actions include:
  - "Ignore" (default) - Require a long press to turn off the device
  - "Sleep" - A short press puts the device into sleep mode
  - "Page Turn" - A short press in reading mode turns to the next page; a long press turns the device off
  - "Toggle Bookmark", "Reading Stats", "Mark Finished", "Refresh", "Change Font", "Guide Dots", "Bionic Reading", "Auto Page Turn", "Sync Progress", "File Transfer", "Calibre Wireless", "Join a Network", "Create Hotspot", "Screenshot", "Dark Mode", "Browse Files", or "Save Clipping" - Run the matching action
  - "Footnotes" - A short press in reading mode opens the footnotes submenu; if only one footnote is present on the page, the referenced page is opened directly. The short press on the power button can be used to select the footnote in the submenu, and to go back to the original page after finish reading the footnote (like the back button).

- **Quick-return from footnotes**: Toggles on and off the quick return functionality from the footnotes. When the functionality it's active, a short press of the power button will act as the back button from the footnotes page.

#### 3.6.4 System

- **Time to Sleep**: Set the duration of inactivity before the device automatically goes to sleep. Values are in minutes, with a "Never" option at the end of the range.

- **Device**: Set the device name and time-to-sleep timeout. Devices with a
  real-time clock also expose clock format, UTC offset, and a sync action.

- **Files & Cache**: Configure hidden files, file extensions, file-browser view,
  finished-book behavior, and clear the reading cache.

- **Reading Stats**: Configure stats tracking and idle-time filtering, and
  access all-time stats backup/reset actions.

- **Wi-Fi Networks**: Connect to Wi-Fi networks for file transfers and firmware updates.

- **KOReader Sync**: Options for setting up KOReader for syncing book progress. **Smart sync** is the default for new configurations and auto-resolves simple push/pull decisions. Existing credential files retain **Ask every time** when migrated; you can switch Sync Behavior at any time if you prefer manual confirmation.

- **OPDS Servers**: Manage one or more OPDS [(Open Publication Distribution System)](https://en.wikipedia.org/wiki/Open_Publication_Distribution_System) libraries for browsing and downloading books. See [OPDS Servers (Multiple Libraries)](#365-opds-servers-multiple-libraries) below.

- **Check for Updates** and **SD Firmware Update**: Check for firmware updates
  over Wi-Fi or install a `firmware.bin` placed on the SD card.

- **Language**: Set the UI language. CrossInk supports 28 languages: English,
  Spanish, French, German, Czech, Brazilian Portuguese, Russian, Swedish,
  Romanian, Catalan, Ukrainian, Belarusian, Italian, Polish, Finnish, Danish,
  Dutch, Turkish, Kazakh, Hungarian, Lithuanian, Slovenian, Valencian, Hebrew,
  Vietnamese, Slovak, Portuguese (Portugal), and Arabic.

#### 3.6.5 OPDS Servers (Multiple Libraries)

CrossInk supports saving multiple OPDS servers and switching between them when browsing catalogs.

1. Open **Settings -> System -> OPDS Servers**.

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

#### 3.6.7 KOReader Sync Quick Setup

CrossInk can sync reading progress with KOReader-compatible sync servers.
It also interoperates with KOReader apps/devices when they use the same server and credentials.

##### Option A: CrossPoint Sync Server (`sync.crosspointreader.com`, default)

When **Sync Server URL** is left empty, CrossInk uses the free CrossPoint sync server at `https://sync.crosspointreader.com`. It speaks the standard KOReader sync protocol (so KOReader apps can use it too) and additionally stores an exact spine/page position for lossless CrossInk-to-CrossInk sync.

1. On each CrossInk device:
   - Go to **Settings -> System -> KOReader Sync**.

   - Set **Username** and **Password** (enter the plain password; CrossInk computes MD5 internally, and use the same values on all devices).

   - Leave **Sync Server URL** empty (or set it to `https://sync.crosspointreader.com`).

   - On the first device, run **Sign Up** once to create the account directly from the device. On every other device, just run **Authenticate**.

Accounts are per server. Existing `sync.koreader.rocks` credentials do not exist on the CrossPoint server; either sign up again with the same username/password or use Option B to keep using the legacy server.

##### Option B: Legacy Public KOReader Server (`sync.koreader.rocks`)

Use this if you already sync KOReader devices against the official public server.

1. On each CrossInk device:
   - Go to **Settings -> System -> KOReader Sync**.

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

##### Option C: Self-Hosted Server (Docker Compose)

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
   - Go to **Settings -> System -> KOReader Sync**.

   - Set **Username** and **Password** (enter the plain password; CrossInk computes MD5 internally, and use the same values on all devices).

   - Set **Sync Server URL** to `http://<server-ip>:17200`.

   - Run **Authenticate**.

If you use the HTTPS listener, use `https://<server-ip>:7200` (`curl -k` only for self-signed certificate testing).

##### Syncing While Reading

Once any of the options above is set up, press **Confirm** while reading to open the reader menu, then select **Sync Progress**. Alternatively, set **Settings -> Controls -> Long-press Menu** to **KOSync** and hold Confirm to launch sync directly.

- With **Sync Behavior** set to **Ask every time**, choose **Apply Remote** to jump to remote progress or **Upload Local** to push current progress.
- With **Sync Behavior** set to **Smart sync**, CrossInk auto-resolves simple cases: upload when no remote progress exists, confirm and leave both unchanged when local and remote progress are already synchronized, upload when local progress is further ahead, or apply remote when remote progress is further ahead.

### 3.7 Sleep Screen

The **Sleep Screen** setting controls what is displayed when the device goes to sleep:

| Mode               | Behavior                                                                                                                                         |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| **Dark** (default) | The CrossInk logo on a dark background.                                                                                                          |
| **Light**          | The CrossInk logo on a white background.                                                                                                         |
| **Custom**         | A custom image from the SD card (see below). Falls back to **Dark** if no custom image is found.                                                 |
| **Cover**          | The cover of the currently open book. Falls back to **Dark** if no book is open.                                                                 |
| **Cover + Custom** | The cover of the currently open book, shown only while actively reading. Falls back to **Custom** behavior when not reading.                     |
| **Page Overlay**   | Keeps the current reader page visible and draws a sleep wallpaper over it. If no wallpaper is available, the page remains visible while reading. |
| **Minimal**        | A compact sleep screen based on the Minimal home layout.                                                                                         |
| **Minimal Stats**  | A compact sleep screen with recent reading stats, on supported devices.                                                                          |
| **None**           | A blank screen.                                                                                                                                  |

#### Cover settings

When using **Cover** or **Cover + Custom**, two additional settings apply:

- **Sleep Screen Cover Mode**: **Fit** (scale to fit, white borders) or **Crop** (scale and crop to fill the screen).
- **Sleep Screen Cover Filter**: **None** (grayscale), **Contrast** (black & white), or **Inverted** (inverted black & white).

#### Custom images

To use custom sleep images, set the sleep screen mode to **Custom**, **Cover + Custom**, or **Page Overlay**, then place images on the SD card:

- **Multiple Images (recommended):** Create a `.sleep` directory in the root of the SD card and place any number of `.bmp` images inside. In **Page Overlay** mode, `.png` images are also supported. One image will be randomly selected each time the device sleeps. (A directory named `sleep` is also accepted as a fallback.)
- **Single Image:** Place a file named `sleep.bmp` in the root directory. In **Page Overlay** mode, `sleep.png` is also supported. These files are used as fallbacks if no valid image is found in the `.sleep`/`sleep` directory.

In **Page Overlay** mode, white BMP pixels and transparent PNG pixels let the current reader page show through; the remaining wallpaper pixels are drawn over the page. PNG wallpapers are supported only in this mode.

> [!TIP]
> For best results:
>
> - Use uncompressed BMP files with 24-bit color depth
> - X4: Use a resolution of 480x800 pixels to match the device's screen resolution.
> - X3: Use a resolution of 528x792 pixels to match the device's screen resolution.

> [!TIP]
> You can set an image as the sleep screen cover directly from the BMP image viewer in the **[Browse Files](#33-browse-files-screen)** screen.

---

### 3.8 Boot Screen

You can replace the default CrossInk logo shown during a cold boot with BMP images stored on the SD card. When a custom boot screen is configured, it is also shown after a power-button wake. This is separate from the sleep screen.

Choose one of these options:

- **Rotating images:** Create a `/.bootscreen` folder in the root of the SD card and place BMP files directly inside it. CrossInk randomly selects an image each time it starts and avoids recently used images where possible. A `/bootscreen` folder is also supported. Folder names are case-insensitive; if both folders exist, `/.bootscreen` takes priority.
- **One fixed image:** In **[Browse Files](#33-browse-files-screen)**, open a BMP image, open its context menu, and choose **Set Boot Screen**. The selected image takes priority over the rotating folder.

To return from a fixed image to the rotating folder or the default logo, open that same image and choose **Clear Boot Screen**. On a cold boot, CrossInk shows the standard logo if there is no selected image and the active boot-screen folder has no usable BMP. A power-button wake remains splashless and keeps the sleep screen visible only when there is neither a selected image nor a boot-screen folder; an empty or unusable boot-screen folder falls back to the standard logo instead.

> [!TIP]
> Use an uncompressed BMP at your device's screen resolution for the best result: 480x800 pixels on X4 or 528x792 pixels on X3. Images with other dimensions are centered and scaled down as needed.

---

### 3.9 Custom Fonts (SD Card)

CrossInk supports loading additional fonts from the SD card, extending beyond the built-in Lexend Deca and Bitter families. Custom fonts can include extended Unicode coverage, enabling CJK (Chinese, Japanese, Korean) and other scripts.

There are three ways to install fonts:

1. **Download from device (recommended):** Go to **Settings -> Reader -> Font Options -> Manage Fonts**, browse the available font families, and select one to download over Wi-Fi.
2. **Upload via web interface:** While in **File Transfer** mode, open the web UI in a browser and navigate to the **Fonts** tab to upload `.cpfont` files.
3. **Manual SD card copy:** Download font files from the [CrossInk-fonts repository](https://github.com/uxjulia/crossink-fonts/releases) and copy them to `/.fonts/` (preferred) or `/fonts/` on your SD card.

Once installed, custom fonts appear in **Settings -> Reader -> Font Options -> Font Family** alongside the built-in fonts.

See [SD Card Fonts](./sd-card-fonts.md) for full installation details and SD card folder structure.

---

## 4. Reading Mode

Once you have opened a book, the button layout changes to facilitate reading.

### Page Turning

| Action            | Buttons                       |
| ----------------- | ----------------------------- |
| **Previous Page** | Press **Left** _or_ **Up**    |
| **Next Page**     | Press **Right** _or_ **Down** |

The role of the side buttons can be swapped in **Settings > Controls > Side Buttons**.

If the **Short-press Action** setting is set to "Page Turn", you can also turn to the next page by briefly pressing the Power button.

### Chapter Navigation

- **Next Chapter:** Press and **hold** the **Right** (or **Down**) button briefly, then release.
- **Previous Chapter:** Press and **hold** the **Left** (or **Up**) button briefly, then release.

This feature can be disabled in **Settings > Controls > Front Buttons** to help avoid changing chapters by mistake.

### Auto Page Turn

Auto Page Turn automatically advances pages at a set interval, useful for hands-free reading. This feature can be enabled and configured from the **[Reader Menu](#5-reader-menu)** while reading an EPUB.

### Tilt Page Turn (X3 and Sticky)

On the **Xteink X3** and **Sticky**, the gyroscope can be used to turn pages by tilting the device. This feature and its left-right or forward-back direction are available in **Settings -> Controls**.

### Touch Reader Controls

On supported touchscreen devices, **Touch Reader Controls** is enabled by
default. **Page Turn Gesture**, in **Settings > Controls**, is set to **Tap &
Swipe** by default: tap the left third of the page to go back, tap the rest of
the page to go forward, swipe right for the previous page, or swipe left for
the next page. Choose **Tap Only**, **Swipe Only**, **Inverted Tap**, or
**Disabled** to change only page turns. **Inverted Tap** uses the left
two-thirds of the page to go forward and the right third to go back. The top
and bottom gesture bands are reserved for vertical gestures, so taps in those
bands do not turn pages.

Swipe down to open the reader menu and swipe up to return Home. On an X4 Pro,
which has a capacitive Home key, the vertical gestures are reversed: swipe up
to open the reader menu, and use a short press of the Home key to return Home.
A long press of that key also opens the reader menu.

Choose **Disabled** in **Page Turn Gesture** to stop touch page turns without
disabling the touchscreen's vertical reader-menu or frontlight gestures. Turn
**Touch Reader Controls** off in **Reader Options** to disable these page-turn
and reader gesture controls. **Disable Touchscreen** prevents touch input while
a book is open but keeps it available in reader menus. For the different touch
selection gestures used by [dictionary lookup](./dictionary.md#looking-up-a-word)
and [clippings](./reader-features.md#clippings-and-highlights), see those
feature guides.

On devices with multi-touch support, you can also assign actions to two-finger
swipes from **Settings > Controls > Taps & Gestures > Two-finger Swipe**. Set an
action for **Swipe Up**, **Swipe Down**, **Swipe Left**, or **Swipe Right**, then
move two fingers together in that direction while reading. Available actions
include brightness and warmth changes (when the hardware supports them),
chapter navigation for EPUBs, and font-size changes for EPUB or TXT books. Each
action can be assigned to only one direction; choosing it again moves it to the
new direction. See [Two-finger Swipe Actions](./controls.md#two-finger-swipe-actions)
for the complete list and reader-specific limitations.

### Slider Controls

For sliders with five-unit increments in the Reader Menu and Settings, tapping
the slider track rounds the selected value to the nearest multiple of five.

### Footnote Navigation

When reading an EPUB that contains footnotes, you can navigate to the footnote text by selecting the footnote reference in the book. From the footnote, you can return to your original reading position.

If the device goes to sleep or you close the book while viewing a footnote, the book reopens to your original reading position, not the footnote.

### System Navigation

- **Return to Home:** Press the **Back** button to close the book and return to the **[Home](#31-home-screen)** screen.
- **Return to Browse Files:** Press and hold the **Back** button to close the book and return to the **[Browse Files](#33-browse-files-screen)** screen.
- **Reader Menu:** Press **Confirm** to open the **[Reader Menu](#5-reader-menu)**, which includes chapter navigation, reading options, and more.

### Supported Languages

CrossInk renders text using the following Unicode character blocks, enabling support for a wide range of languages:

- **Latin Script (Basic, Supplement, Extended-A/B):** Covers English, German, French, Spanish, Portuguese, Italian, Dutch, Swedish, Norwegian, Danish, Finnish, Polish, Czech, Hungarian, Romanian, Slovak, Slovenian, Turkish, Catalan, and others.
- **Cyrillic Script (Standard and Extended):** Covers Russian, Ukrainian, Belarusian, Bulgarian, Serbian, Macedonian, Kazakh, Kyrgyz, Mongolian, and others.
- **Vietnamese:** Supported via extended Latin glyph coverage in the built-in reader fonts.

What is not supported with built-in reader fonts: Chinese, Japanese, Korean, Arabic, Greek, Hebrew, and Farsi. However, **CJK, Hebrew, Greek, and other extended scripts can be enabled by installing custom SD card fonts** — see [Custom Fonts (SD Card)](#38-custom-fonts-sd-card).

---

## 5. Reader Menu

Press **Confirm** while reading to open the Reader Menu. From here you can access reading utilities and navigation options without leaving the book.

Available options include:

- **Select Chapter** – Open the table of contents to jump to a specific chapter (see [Chapter Selection](#51-chapter-selection) below).
- **Footnotes** – Navigate to the footnotes for the current section _(only shown in books that contain footnotes)_.
- **Reader Options** – Open reader-specific options without leaving the book.
- **Controls** – Open reader control options without leaving the book.
- **Reading Orientation** – Cycle through screen orientations without leaving the reader.
- **Auto Turn Interval** – Configure automatic page turns for hands-free reading.
- **Go to %** – Jump to a specific position in the book by percentage.
- **Add Bookmark / Remove Bookmark** – Toggle a bookmark on the current page.
- **View Bookmarks / Delete Bookmarks** – Manage existing bookmarks when the book has bookmarks.
- **Take screenshot** – Save a screenshot of the current page to the `screenshots/` folder.
- **Show page as QR** – Display a QR code encoding the current reading position.
- **Delete Book Cache** – Clear the cached layout data for the current book, forcing a re-index on next open.
- **Sync Progress** – Push or pull reading progress with a KOReader sync server (see [KOReader Sync Quick Setup](#367-koreader-sync-quick-setup)).
- **Reading Stats** – Open the current book's reading stats.
- **Mark Finished / Mark Unfinished** – Toggle whether the current book is marked as finished.
- **Look Up Word / Lookup History** – Select words on the page and revisit recent per-book lookups when a dictionary is active.
- **Book Dictionary** – Choose a per-book dictionary override from the reader menu's settings tab.

Press **Back** at any time to close the menu and return to your current page.

### 5.1 Chapter Selection

Accessible by selecting **Chapters** from the Reader Menu.

1. Use **Left** (or **Up**), or **Right** (or **Down**) to highlight the desired chapter.
2. Press **Confirm** to jump to that chapter.
3. _Alternatively, press **Back** to cancel and return to your current page._

---

### 5.2 Bookmarks

Bookmarks can be created to quickly save and restore your place in a book.

To create a bookmark, hold **Confirm** for 1 second while inside a book. A popup will appear letting you know a bookmark was created. The popup message will automatically disappear in a couple of seconds.

To open bookmarks, press **Confirm** while inside a book. Then navigate to the **Bookmarks** menu. Bookmarks can be opened by navigating to them and pressing **Confirm**, which will redirect you to that place in the book. You can delete bookmarks by holding **Confirm** for 1 second, and then pressing **Confirm** again to confirm deletion, or **Back** to cancel.

Bookmarks are stored as per-book `.bin` files in the `.crosspoint/bookmarks` folder.

### 5.3 Dictionary

Dictionary lookup supports word selection, recent per-book history, chained lookups from definitions, and per-book dictionary overrides. See the [Dictionary guide](./dictionary.md) for installation and preparation instructions.

## 6. Current Limitations & Roadmap

Please note that this firmware is currently in active development. The following features are **not yet supported** but are planned for future updates:

- **Cover Images:** Large cover images embedded into EPUB can take several
  seconds to convert for the sleep screen and home-screen thumbnail. Use the
  built-in [EPUB optimization](./webserver.md#epub-optimization) before upload
  if a book is slow or memory-sensitive.
- **Unsupported Image Formats:** Most JPG and PNG images in EPUBs render correctly. GIFs and progressive JPEGs are not supported and will fall back to an `[Image]` placeholder.

---

## 7. Troubleshooting Issues & Escaping Bootloop

If an issue or crash is encountered while using CrossInk, feel free to raise an issue ticket and attach the logs.

**Crash reports on SD card:** After a crash, CrossInk automatically saves a crash report to the SD card (no USB connection needed). Check the root of the SD card for a crash log file and include it with any bug report.

**Serial monitor logs:** For more detailed debugging, connect the device to a computer and run the custom debugging monitor script (requires Python 3 with `pyserial`, `colorama`, and `matplotlib`; install via `pip3 install pyserial colorama matplotlib`):

```
python3 scripts/debugging_monitor.py
```

The script auto-detects the serial port. You can also specify one explicitly:

```
python3 scripts/debugging_monitor.py /dev/ttyACM0        # Linux
python3 scripts/debugging_monitor.py /dev/tty.usbmodem1  # macOS
python3 scripts/debugging_monitor.py COM7                # Windows
```

**Features:**

- Color-coded log output by category (errors, memory, display, EPUB parsing, etc.)
- Live memory usage graph (free RAM, total RAM, max contiguous allocation) updated every second
- Interactive command prompt — type a command and press Enter to send it to the device
- Screenshot capture — saves the current display to `screenshot.bmp` when triggered by the device

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

If the device is stuck in a bootloop, press and release the Reset button. Then, press and hold on to the configured Back button and the Power Button to boot to the Home Screen.

There can be issues with broken cache or config. In this case, delete the `.crosspoint` directory on your SD card (or consider deleting only `settings.json`, `state.json`, or `epub_*` cache directories in the `.crosspoint/` folder).
