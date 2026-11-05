#pragma once
#include <string>
// Remote streak lookup is disabled in these device-local storage tests.
struct MD5Builder {
  void begin() {}
  void add(const char*) {}
  void calculate() {}
  std::string toString() const { return "test-account"; }
};
