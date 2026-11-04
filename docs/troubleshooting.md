---
title: Troubleshooting
nav_order: 14
---

# Troubleshooting

This document shows common issues and possible solutions while using the device features.

- [Cannot See the Device on the Network](#cannot-see-the-device-on-the-network)
- [Connection Drops or Times Out](#connection-drops-or-times-out)
- [Upload Fails](#upload-fails)
- [Saved Password Not Working](#saved-password-not-working)
- [Device Information](#device-information)
- [Sharing Settings for Support](#sharing-settings-for-support)
- [Export Support Information](#export-support-information)

### Cannot See the Device on the Network

**Problem:** Browser shows "Cannot connect" or "Site can't be reached"

**Solutions:**

1. Verify both devices are on the correct network (it must be a 2.4 GHz network; 5 GHz is not supported)
   - Check your computer/phone Wi-Fi settings
   - In **Join a Network** mode, your computer/phone and CrossInk must be on the same Wi-Fi network
   - In **Create Hotspot** mode, your computer/phone must be connected to the `CrossPoint-Reader` hotspot
2. Double-check the IP address
   - Make sure you typed it correctly
   - Include `http://` at the beginning
   - If `http://crosspoint.local/` does not work, type the numeric address shown on the reader instead
3. Try disabling VPN if you're using one
4. Some networks have "client isolation" enabled, which stops devices on the same Wi-Fi from talking to each other. Use **Create Hotspot** mode or ask your network administrator

### Connection Drops or Times Out

**Problem:** Wi-Fi connection is unstable

**Solutions:**

1. Move closer to the Wi-Fi router, or use Create Hotspot mode for a direct connection
2. Check signal strength on the device (two bars or better)
3. Avoid interference from other devices
4. Try a different Wi-Fi network if available (it must be a 2.4 GHz network; 5 GHz is not supported)

### Upload Fails

**Problem:** File upload doesn't complete or shows an error

**Solutions:**

1. Check that the SD card has enough free space
2. Check that the filename is simple, without special characters such as `:`, `*`, or `?`
3. Try uploading a smaller file first to test
4. Refresh the browser page and try again
5. If uploads keep failing, refresh the page and try again. The page falls back to a slower upload method automatically

### Saved Password Not Working

**Problem:** Device fails to connect with saved credentials. This can happen after changing the reader software or moving the SD card to another reader.

**Solutions:**

1. When the connection fails, the reader offers to forget the network
2. Choose **Forget** to remove the saved password
3. Reconnect and enter the password again
4. Choose to save the new password

### Device Information

To see details about your reader, open **Settings > System > About**. Scroll with Up/Down on button devices, or swipe up and down on touch devices. Back returns you to Settings.

The page shows read-only details such as the device model, firmware version, screen, storage, and memory. It does not show Wi-Fi names, passwords, or other personal identifiers, and it does not need a network connection. Close and reopen the page to refresh the numbers.

### Sharing Settings for Support

When you report a problem, it helps to share your settings. CrossInk keeps global preferences (reader defaults, controls, status bars, library options, and so on) in `/.crosspoint/crossink-settings.json` on the SD card. This file does not include hardware details or per-book settings.

Some values, such as your device name, font names, and transfer folder paths, could reveal private information. Look through the file and remove anything you do not want to share.

Do not share the whole `.crosspoint` folder. It also holds saved Wi-Fi, OPDS, and KOReader details (including passwords, which are only lightly scrambled, not encrypted), plus your reading history, bookmarks, and clippings. Use the support export below instead.

### Export Support Information

The easiest way to give a developer what they need is a support file. It is saved on the SD card only. Nothing is uploaded or sent anywhere.

1. Open **Settings > System > About**.
2. Choose **Export** (on touch devices, tap the Export button).
3. Pick what to include:
   - **Device and global settings**, or
   - **Also include last opened EPUB settings**, which adds the reader settings for the one book you opened most recently.
4. Read the confirmation, then confirm. **Cancel** is selected by default on both screens, so nothing is saved by accident.

The reader saves `/crossink-support.json` in the top folder of the SD card. A new export replaces the previous one. Attach that file to your report.

The file leaves out passwords, Wi-Fi and server names, usernames, device names, custom font names, folder and book paths, book titles and authors, bookmarks, clippings, reading history, and logs. It still shows your device model and your preference choices, so give it a quick look before sharing.

If the reader says a previous export was kept as `.bak`, check
`/crossink-support.json.bak` on the SD card before removing it. If an export
fails, try again or keep the previous file for your support report.
