#pragma once

#include <cstdint>

// Read-only copies of .cpfont layout fonts in internal flash, memory-mapped
// for the shaper so a complex-script face costs no heap for its tables.
//
// No-PSRAM boards only: layout fonts (5-80 KB) otherwise occupy reader RAM.
// CrossInk reserves 384 KiB at the end of hyphenation bank 1 for a directory
// and two 128 KiB slots. Bank starts and language slots keep their existing
// addresses. Old installed patterns that occupy the tail disable this cache
// until a later install/removal frees it and the device restarts.
// Blobs are keyed and verified by content hash. Only idle slots are recycled.
//
// Everything here is best-effort: acquire() returns nullptr whenever the
// partition is missing or too small, the blob is too large, or flash access
// fails, and the caller then decides whether the blob may stay in RAM.
namespace FlashBlobCache {

// Largest blob a slot holds: Noto Devanagari's layout font is ~77 KB.
constexpr uint32_t MAX_BLOB_BYTES = 128 * 1024;

// Fills buf with `length` bytes of the blob starting at `offset`.
using Reader = bool (*)(void* ctx, uint32_t offset, uint8_t* buf, uint32_t length);

// Maps the blob whose content hashes to `key` (FNV-1a | 1), copying it in
// through `read` first when flash does not already hold it.
const uint8_t* acquire(uint32_t key, uint32_t length, Reader read, void* ctx);

// Unmaps a pointer returned by acquire(). Signature matches
// ComplexShaper::Blob::release.
void release(void* data);

// Whether acquire() could ever map this blob: the partition is usable, the
// blob fits a slot and its copy has not failed. When true, a failed
// acquire() may succeed later (a copy buffer, a mapped slot freeing up).
bool canHold(uint32_t key, uint32_t length);

}  // namespace FlashBlobCache
