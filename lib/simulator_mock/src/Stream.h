#pragma once
#include "Print.h"
#include "WString.h"
class Stream : public Print {
 public:
  virtual ~Stream() = default;
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
  virtual void flush() override {}
  String readStringUntil(char terminator) { return String(""); }
  size_t readBytes(char* buffer, size_t length) { return 0; }
};
