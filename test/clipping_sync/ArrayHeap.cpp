#include "ArrayHeap.h"

#include <cstdlib>
#include <new>
namespace {
struct alignas(std::max_align_t) Header {
  size_t size;
  bool tracked;
};
void* allocate(size_t size) noexcept {
  if (arrayHeap::tracking && size > arrayHeap::limit - arrayHeap::live) return nullptr;
  auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + size));
  if (!header) return nullptr;
  header->size = size;
  header->tracked = arrayHeap::tracking;
  if (header->tracked) {
    arrayHeap::live += size;
    if (arrayHeap::live > arrayHeap::peak) arrayHeap::peak = arrayHeap::live;
  }
  return header + 1;
}
void release(void* ptr) noexcept {
  if (!ptr) return;
  auto* header = static_cast<Header*>(ptr) - 1;
  if (header->tracked) arrayHeap::live -= header->size;
  std::free(header);
}
}  // namespace
void* operator new[](size_t size) {
  if (auto* ptr = allocate(size)) return ptr;
  throw std::bad_alloc();
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void operator delete[](void* ptr) noexcept { release(ptr); }
void operator delete[](void* ptr, size_t) noexcept { release(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { release(ptr); }
