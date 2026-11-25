#pragma once
#define FREEINK_LOG_TRANSPORT 0
#define FREEINK_LOG_TRANSPORT_ROM_PRINTF 1
struct TestSerial {
  void begin(unsigned long) {}
  operator bool() const { return true; }
  void print(const char*) {}
};
namespace BoardConfig {
inline TestSerial& serialTransport() {
  static TestSerial serial;
  return serial;
}
}  // namespace BoardConfig
