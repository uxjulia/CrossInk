#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Non-owning label access shared by popup layout and drawing. The owner keeps
// the backing catalog alive until the popup is cleared under the render lock.
struct OptionLabels {
  const void* context = nullptr;
  size_t count = 0;
  const char* (*read)(const void*, size_t) = nullptr;
  bool (*disabled)(const void*, size_t) = nullptr;
  const std::vector<bool>* flags = nullptr;

  OptionLabels() = default;
  OptionLabels(const void* owner, size_t size, const char* (*label)(const void*, size_t),
               bool (*isDisabled)(const void*, size_t) = nullptr)
      : context(owner), count(size), read(label), disabled(isDisabled) {}
  OptionLabels(const std::vector<std::string>& labels, const std::vector<bool>& disabledFlags)
      : context(&labels),
        count(labels.size()),
        read([](const void* owner, size_t i) {
          return (*static_cast<const std::vector<std::string>*>(owner))[i].c_str();
        }),
        flags(&disabledFlags) {}
  size_t size() const { return count; }
  bool empty() const { return count == 0; }
  const char* operator[](size_t i) const { return read(context, i); }
  bool isDisabled(size_t i) const {
    return disabled ? disabled(context, i) : flags && i < flags->size() && (*flags)[i];
  }
};
