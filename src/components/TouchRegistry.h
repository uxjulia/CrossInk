#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "AppCapabilities.h"
#include "components/themes/BaseTheme.h"

#if CROSSINK_APP_CAP_TOUCH
class TouchRegistry {
 public:
  enum Kind : uint8_t { Item = 0, Back = 1, Tab = 2, Cover = 3, Button = 4, Footnote = 5 };

  static TouchRegistry& getInstance();

  void setEnabled(bool enabled) { enabled_ = enabled; }
  bool isEnabled() const { return enabled_; }

  void beginFrame();
  void add(const Rect& rect, int id, Kind kind);
  void publish();
  void clear();
  bool hitTest(int x, int y, Kind kind, int& outId) const;

 private:
  static constexpr size_t CAPACITY = 64;

  struct Target {
    Target() : rect(0, 0, 0, 0), id(-1), kind(Item) {}
    Target(const Rect& rect, int16_t id, uint8_t kind) : rect(rect), id(id), kind(kind) {}

    Rect rect;
    int16_t id;
    uint8_t kind;
  };

  uint8_t backIndex() const { return live_.load(std::memory_order_relaxed) ^ 1u; }

  std::array<std::array<Target, CAPACITY>, 2> buffers_{};
  std::array<size_t, 2> counts_{0, 0};
  std::atomic<uint8_t> live_{0};
  bool enabled_ = false;
};
#else
// Header-only empty type lets shared render code compile while the compiler
// removes every registration call from button-only images.
class TouchRegistry {
 public:
  enum Kind : uint8_t { Item = 0, Back = 1, Tab = 2, Cover = 3, Button = 4, Footnote = 5 };

  static TouchRegistry& getInstance() {
    static TouchRegistry instance;
    return instance;
  }

  constexpr void setEnabled(bool) {}
  constexpr bool isEnabled() const { return false; }
  constexpr void beginFrame() {}
  constexpr void add(const Rect&, int, Kind) {}
  constexpr void publish() {}
  constexpr void clear() {}
  constexpr bool hitTest(int, int, Kind, int&) const { return false; }
};
#endif
