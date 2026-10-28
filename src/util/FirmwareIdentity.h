#pragma once

// Plugin-system contract advertised to catalogs and the web UI.
namespace firmwareidentity {
inline constexpr const char* FAMILY = "crosspoint";
inline constexpr int PLUGIN_API_VERSION = 1;
inline constexpr const char* PLUGIN_CAPABILITIES =
    "catalog.lists,catalog.notices,catalog.bundle_install,catalog.version_tracking,catalog.compatibility,catalog.item_"
    "details,events.reader.open,events.reader.exit,events.reader.session,events.book.downloaded,events.sleep.enter,"
    "crypto,book.key,book.protected,catalog.encoded_item_fields";
}  // namespace firmwareidentity
