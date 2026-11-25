---
title: Plugins
nav_order: 10
---

# Plugins

Plugins add features such as online book catalogs and connections to other
services. Some work on the reader; others are used from its web page on your
phone or computer. Each plugin has its own setup instructions.

## Enable plugins

1. Open **Settings > System > Plugins** and choose **Enabled**.
2. Return to **Home** and open **Plugins**.

Plugins are off on a fresh installation. If you upgrade with plugins already
on your SD card, they may already be enabled.

## Install Plugin Hub

Plugin Hub lets you discover, install, and update plugins.

1. Open **Home > Plugins** and select **Plugin Hub**.
2. Choose **Download**, then connect to Wi-Fi when asked.
3. Wait for the download to finish, then return to the plugin list and open
   **Plugin Hub**.
4. Browse the catalog, select a plugin, read its description and any notices,
   then follow the download prompts.

**Hide** removes the Hub installation offer from the list. To bring it back,
turn Plugins off and on again in Settings. The Hub is optional; you can also
install a plugin manually.

## Install from a computer

1. Download the plugin from its publisher and unzip it if needed.
2. Copy its complete plugin folder into `/plugins/` on your SD card. For
   example, a plugin named `my-library` goes in `/plugins/my-library/`.
3. Keep the files directly inside that folder. Avoid an extra folder layer
   such as `/plugins/my-library/my-library/`.
4. Safely eject the card, put it back in the reader, and open **Home > Plugins**.

`/.crosspoint/plugins/` and `/.plugins/` are also supported. Use just one copy
of each plugin. If duplicates exist, the reader prefers `.crosspoint/plugins`,
then `plugins`, then `.plugins`. Folders starting with a dot may be hidden by
your computer's file manager.

You can download the Hub itself from
[Plugin Hub releases](https://github.com/crosspoint-reader/crosspoint-plugin-hub/releases)
if its on-reader download is unavailable.

## Open and set up a plugin

### On the reader

Open **Home > Plugins** and select a plugin that offers an on-reader catalog.
Connect to Wi-Fi if asked, then browse and download from its lists. Some
catalogs also offer search or several collections to choose from.

If sign-in is required, follow the plugin's instructions. It may ask you to
sign in through the web page, or show a code and a QR link to approve on your
phone or computer. If a code expires, start sign-in again.

### On your phone or computer

1. Open **Home > File Transfer > Join a Network** on the reader.
2. Connect your phone or computer to the same Wi-Fi network.
3. Open the web address shown on the reader.
4. Look for the plugin in **Settings** or **File Manager**, as directed by its
   instructions. Plugin Hub's browser page also offers plugin management.

Keep File Transfer and the browser page open while using a browser plugin.
See [File Transfer](./webserver.md) for connection help. A **Web-only plugin**
entry does not open a catalog on the reader; use its browser page instead.
Some plugins work in the background after setup and have no catalog to open.

Downloaded books appear in their chosen folder. If a book is missing from
Library, choose **Refresh Library** or look in **Browse Files**.

## Update, disable, or remove a plugin

- **Update:** Open Plugin Hub and use its update option when available. Follow
  the publisher's instructions for manually installed plugins. Back up any
  personal plugin settings before replacing files yourself.
- **Disable all plugins:** Choose **Disabled** in **Settings > System > Plugins**.
  This stops plugin features and background activity while keeping their files.
- **Remove one plugin:** In **Home > Plugins**, highlight it and hold **Confirm**
  for about one second, or touch and hold its row. Choose **Uninstall** and
  confirm. This deletes that plugin's folder and everything inside it, including
  settings and sign-in details stored there. Books saved elsewhere stay on the
  card. **Cancel** keeps the plugin.

## Privacy and borrowed books

Install plugins only from publishers you trust. Browser plugins can manage files
through the reader's web page. Some plugins can send reading activity to a
service; their description in the plugin list shows **Receives:** followed by
activity such as opening a book or going to sleep. Check the plugin's own
privacy information before connecting an account.

Plugin settings and sign-in details may be stored on the SD card. Do not share
plugin folders that contain your account details. Use File Transfer on a
trusted network, and close it when finished.

Borrowed or protected books need a plugin supported by the lending service.
Copying their files to a different reader may not let that reader open them.
If a loan expires, renew it through the service. If the reader asks to check
the time, connect it to Wi-Fi and try opening the book again.

## Troubleshooting

| Problem | What to try |
| --- | --- |
| **Plugins** is missing from Home | Enable it in **Settings > System > Plugins**. |
| A manually copied plugin is missing | Check the folder location, unzip the download, and remove any extra folder layer. Reopen Plugins. The list supports up to 16 plugins. |
| A plugin appears but does not open | Check whether it is web-only or works in the background. Follow its setup instructions in the browser. |
| Sign-in fails or expires | Check your account details and restart sign-in. Approve a new code before it expires. |
| A catalog or download fails | Check Wi-Fi and SD-card free space, then retry. Check for a plugin update or a service outage. |
| The browser shows an old plugin page | Refresh the page after updating. Check for duplicate plugin folders if the old version keeps returning. |

Plugin authors can use the separate
[Plugin Development Guide](./development/plugin-development.md).
