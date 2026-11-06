#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdlib>

// A small wire reply can still contain many JSON nodes. Bound the parser's
// total live storage as well as the received bytes; ArduinoJson reports NoMemory.
class SyncJsonAllocator final : public ArduinoJson::Allocator {
 public:
  // Leave room for ArduinoJson's geometric string growth and node pools so
  // a valid >8 KiB single change still fits; TLS is already closed at this point.
  static constexpr size_t CAPACITY = 24 * 1024;
  void* allocate(size_t size) override {
    if (size > CAPACITY - used || sizeof(Header) > CAPACITY - used - size) return nullptr;
    // ArduinoJson owns these blocks and returns them through deallocate().
    auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + size));
    if (!header) return nullptr;
    header->size = sizeof(Header) + size;
    used += header->size;
    return header + 1;
  }
  void deallocate(void* ptr) override {
    if (!ptr) return;
    auto* header = static_cast<Header*>(ptr) - 1;
    used -= header->size;
    std::free(header);
  }
  void* reallocate(void* ptr, size_t size) override {
    if (!ptr) return allocate(size);
    if (!size) {
      deallocate(ptr);
      return nullptr;
    }
    auto* header = static_cast<Header*>(ptr) - 1;
    const size_t remaining = CAPACITY - (used - header->size);
    if (size > remaining || sizeof(Header) > remaining - size) return nullptr;
    const size_t previous = header->size;
    // Ownership stays with ArduinoJson; failure leaves the original block intact.
    auto* next = static_cast<Header*>(std::realloc(header, sizeof(Header) + size));
    if (!next) return nullptr;
    next->size = sizeof(Header) + size;
    used = used - previous + next->size;
    return next + 1;
  }

 private:
  struct alignas(std::max_align_t) Header {
    size_t size;
  };
  size_t used = 0;
};
