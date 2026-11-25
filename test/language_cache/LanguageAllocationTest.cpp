#include <I18n.h>
#include <Memory.h>
#include <esp_heap_caps.h>

#include <cassert>

int main() {
  // Exercise production capability allocation with exceptions disabled. Never
  // override operator new: ESP-IDF's nothrow allocation is not safely fallible.
  fakeheap::reset(false);
  {
    HeapObject<I18n::Catalog> catalog;
    fakeheap::internal.fail = 1;
    assert(!catalog.init(MemoryPool::None));
    assert(catalog.init(MemoryPool::None));
    catalog.reset();
    fakeheap::internal.fail = 1;
    language_cache::Inspector failed;
    assert(!failed.available());
    language_cache::Metadata metadata;
    assert(failed.inspect({}, metadata) == language_cache::Result::Memory);
    language_cache::Inspector recovered;
    assert(recovered.available());
    language_cache::Input input{nullptr, [](void*, void*, size_t) {
                                  assert(false);
                                  return 0;
                                }};
    language_cache::Schema schema{0, [](size_t) { return language_cache::Key{}; }, [](uint16_t) { return ""; }};
    fakeheap::internal.fail = 1;
    assert(language_cache::validate(input, schema, metadata) == language_cache::Result::Memory);
    language_cache::Installed installed;
    language_cache::Flash flash{};
    flash.size = 2 * language_cache::SLOT_SIZE;
    // Null callbacks prove allocation failure occurs before any flash access.
    fakeheap::internal.fail = 1;
    assert(language_cache::install({}, {}, flash, -1, installed) == language_cache::Result::Memory);
  }
  assert(fakeheap::live.empty());
}
