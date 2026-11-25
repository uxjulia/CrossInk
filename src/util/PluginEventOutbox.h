#pragma once

#include <cstddef>

namespace PluginEventOutbox {
inline constexpr char NAME[] = "/events.jsonl";
// Keep the existing rollover: the event that crosses 4 KiB is kept;
// when the next event arrives, drop the older queue as a whole.
inline constexpr size_t MAX_OUTBOX_BYTES = 4 * 1024;
inline constexpr size_t MAX_EVENT_LINE = 2048;
inline constexpr size_t DIRECTORY_CAPACITY = 112;  // includes NUL

// Append an already serialized, newline-terminated event. Caller serializes
// queue access. I/O failure must not replace the last complete queue.
bool append(const char* directory, const char* line, size_t lineSize);
}  // namespace PluginEventOutbox
