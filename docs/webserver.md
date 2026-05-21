---
title: Web Server
nav_order: 3
---

# Web Server Guide

This guide explains how to use CrossInk's built-in File Transfer web server to manage files, settings, fonts, OPDS servers, and saved WiFi credentials from a browser.

## Overview

CrossInk includes a built-in web server that allows you to:

- Upload and download files wirelessly
- Browse, move, rename, and delete files on the SD card
- Create folders to organize your library
- Update supported reader settings from a browser
- Manage SD-card fonts, saved OPDS servers, and saved WiFi networks

## Prerequisites

- Your CrossInk device
- A WiFi network, or a computer/phone that can join the device hotspot
- A browser on a computer, phone, or tablet

---

## Step 1: Open File Transfer

1. From the main menu, select **File Transfer**.
2. Choose a network mode:
   - **Join Network**: connect CrossInk to an existing WiFi network.
   - **Create Hotspot**: have CrossInk create its own WiFi hotspot.
   - **Calibre Wireless**: use Calibre's wireless device connection flow.

For normal browser uploads, use **Join Network** or **Create Hotspot**.

---

## Step 2: Connect to WiFi

### Join Network

After selecting **Join Network**, CrossInk scans for available WiFi networks.

- Signal strength bars (`||||`, `|||`, `||`, `|`) show connection quality.
- `*` indicates the network is password-protected.
- `+` indicates saved credentials already exist for that network.

1. Use the navigation buttons to select a network.
2. Press **Confirm**.
3. Enter the password if prompted.
4. Save credentials if you want CrossInk to reconnect automatically later.

Saved WiFi passwords are stored on the device SD card with device-specific obfuscation. They are validated when read, so credentials copied from another device may need to be re-entered.

### Create Hotspot

After selecting **Create Hotspot**, connect your computer or phone to the hotspot shown on the device screen, then open the displayed web address. In hotspot mode, the device IP is usually `192.168.4.1`.

---

## Step 3: Open the Web Interface

When File Transfer is running, the device screen shows the IP address and web server URL.

1. Open a browser on your computer, phone, or tablet.
2. Enter the URL shown on the CrossInk screen.
3. Keep File Transfer open on the device while using the web interface.

---

## Web Interface

### Home

The home page shows device status, IP address, network mode, signal strength, free heap, and uptime.

### Files

The file manager lets you:

- Browse folders on the SD card.
- Upload files using HTTP or the faster WebSocket upload path used by the page.
- Download files from the SD card.
- Create folders.
- Move or rename files.
- Delete files and empty folders.

Folders must be empty before deletion. File uploads, overwrites, moves, renames, and deletes clear the affected EPUB cache so stale book metadata is not reused.

### Settings

The settings page exposes supported device settings through `/api/settings`. Changes are saved to `/.crosspoint/settings.json`.

### Fonts

The fonts page lists installed SD-card font families and allows `.cpfont` uploads and font family deletion. Uploaded fonts are installed under the SD-card font roots and are picked up by the reader font list.

### OPDS and WiFi Management

The web interface includes APIs for managing saved OPDS servers and saved WiFi credentials. Passwords are never returned by the API; the API only reports whether a password is set. Saved passwords use device-specific validation, so password fields copied from a different reader are ignored until re-entered.

---

## Command Line File Management

You can manage files directly from a terminal with `curl` while File Transfer is running. See [Webserver Endpoints](./webserver-endpoints.md).

## Security Notes

- The web server runs on HTTP port 80.
- The fast upload WebSocket runs on port 81.
- No authentication is required.
- Anyone on the same network, or connected to the CrossInk hotspot, can access the interface while File Transfer is running.
- The web server stops and WiFi disconnects when you exit File Transfer.

Use File Transfer only on trusted networks.

---

## Technical Details

- **Supported WiFi:** 2.4 GHz networks
- **HTTP port:** 80
- **WebSocket port:** 81
- **Upload size:** limited by available SD card space
- **Browser support:** modern Chrome, Firefox, Safari, and Edge

---

## Tips

1. Create folders before large upload batches to keep the library organized.
2. Prefer strong WiFi signal when uploading large EPUBs.
3. Use **Create Hotspot** when you do not want to join an existing WiFi network.
4. Press **Back** on the device when finished to stop the server and save battery.

---

## Related Documentation

- [Webserver Endpoints](./webserver-endpoints.md)
- [Common Issues](./troubleshooting.md)
- [SD Card Fonts](./sd-card-fonts.md)
