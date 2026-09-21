#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class String : public std::string {
 public:
  using std::string::string;

  String() = default;
  String(const std::string& value) : std::string(value) {}

  bool isEmpty() const { return empty(); }
  bool concat(const char* value, const size_t length) {
    append(value, length);
    return true;
  }
  size_t write(const uint8_t value) {
    push_back(static_cast<char>(value));
    return 1;
  }
  size_t write(const uint8_t* value, const size_t length) {
    append(reinterpret_cast<const char*>(value), length);
    return length;
  }
};
